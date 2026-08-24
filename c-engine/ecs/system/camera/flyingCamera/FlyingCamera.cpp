#include "ecs/Ecs.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/window/WindowSystem.h"
#include "ecs/system/transform/TransformComponent.h"
#include "ecs/system/transform/TransformDb.h"
#include "ecs/system/transform/TransformSystem.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "timer/Timer.h"

namespace engine {
static vec3 Y_UP   = {0.0f, 1.0f, 0.0f};
static vec3 X_AXIS = {1.0f, 0.0f, 0.0f};
static void checkKeyboardMovement(u32 cameraEntity);
static void checkMouseMovement(u32 cameraEntity);
static void stutterTest(u32 cameraEntity);
static void toggleStutterTest(u32 cameraEntity);
static void shimmerTest(u32 cameraEntity);
static void toggleShimmerTest(u32 cameraEntity);
static void toggleFlying(u32 cameraEntity);
static void cameraReset(u32 cameraEntity);
static void saveOriginalRT(u32 cameraEntity);
static void checkScrollZoom(u32 cameraEntity);
static void flyingCameraDbInit(void);
static void flyingCameraSaveStatus(char saveTransform);
static char flyingCameraLoadStatus(char* activeOut);

static char flying;
static char flyingFrame;
static float flySpeed           = 1;
static float flySpeedMultiplier = 5;
static float sensitivity        = 2;

static char scanMode;

// Mouse delta accumulated once per render frame (in preUpdate) so that
// the fixed-timestep update loop doesn't consume it on the first
// iteration and leave subsequent iterations with zero delta.
static float pendingMouseDy;
static float pendingMouseDx;
static float pendingScrollY;

static char stutterTestMode;
static float stutterTestDistance;
static vec3 stutterTestCircleAround;
static float stutterTestSpeed;
static float stutterTestAngle;

static char shimmerTestMode;
static vec3 shimmerTestDirection;
static vec3 shimmerTestOriginalPos;
static float shimmerTestTimer;
static float shimmerTestSpeed = 0.5f;

static vec4 originalLocation;
static vec4 originalRotation;
static float originalYaw;
static float originalPitch;

static Scene* scene;
static u32 flyingCameraEntity;
static char flyingCameraGameplayLoaded;

typedef struct FlyingCameraDb {
    int active;
} FlyingCameraDb;

void flyingCameraInit(u32 cameraEntity) {
    scene              = ecs.defaultScene;
    flyingCameraEntity = cameraEntity;
    flyingCameraDbInit();
    saveOriginalRT(cameraEntity);
    transformSaveLast(scene, cameraEntity);

    // Auto-start shimmer test if ENGINE_SHIMMER_TEST is set
    const char* env = getenv("ENGINE_SHIMMER_TEST");
    if (env && *env) {
        auto transform = getComponent(ecs.defaultScene, Transform, cameraEntity);
        vec3 direction;
        transformGetDirection(scene, cameraEntity, direction);
        glm_normalize(direction);

        const char* spdEnv = getenv("ENGINE_SHIMMER_TEST_SPEED");
        if (spdEnv && *spdEnv) shimmerTestSpeed = (float)atof(spdEnv);
        glm_vec3_copy(direction, shimmerTestDirection);
        glm_vec3_copy(transform->pos, shimmerTestOriginalPos);
        shimmerTestTimer = 0.0f;
        shimmerTestMode  = 1;
    }
}

void flyingCameraPreUpdate(u32 cameraEntity) {
    // Capture mouse delta once per render frame so that the fixed-timestep
    // loop in flyingCameraUpdate can apply rotation correctly across
    // multiple iterations (relative mouse delta is destructive — calling
    // it inside the fixed loop would consume the delta on the first
    // iteration and leave subsequent ones with zero).
    float aPendingMouseDx = 0;
    float aPendingMouseDy = 0;
    if (flying) {
        windowSystemGetRelativeMouseDelta(&aPendingMouseDx, &aPendingMouseDy);
        pendingMouseDx += aPendingMouseDx;
        pendingMouseDy += aPendingMouseDy;
        pendingScrollY += input.scrollY;

        // Skip the first frame after entering flying mode — SDL sends a
        // bogus mouse-motion event right after enabling relative mode /
        // warp.  The flyingFrame flag is set by toggleFlying().
        if (flyingFrame) {
            flyingFrame    = 0;
            pendingMouseDx = 0;
            pendingMouseDy = 0;
        }
    }

    if (input.ctrl && input.pressed == KEY_F) {
        toggleFlying(cameraEntity);
    }

    if (input.ctrl && input.shift && input.pressed == KEY_R) {
        auto transform    = getComponent(ecs.defaultScene, Transform, cameraEntity);
        auto camera       = getComponent(ecs.defaultScene, Camera, cameraEntity);
        transform->pos[0] = 0;
        transform->pos[1] = 500;
        transform->pos[2] = 0;
        transform->rot[0] = -0.06;
        transform->rot[1] = -0.93;
        transform->rot[2] = -0.20;
        transform->rot[3] = 0.3;
        transformQuatToPitchYaw(transform->rot, &camera->pitch, &camera->yaw);
        const float pitch_limit_rad = glm_rad(88.0F);
        camera->pitch               = glm_clamp(camera->pitch, -pitch_limit_rad, pitch_limit_rad);
        transformSaveLast(scene, cameraEntity);
        // transformSetGlobal(cameraEntity, 0);
    } else if (input.ctrl && input.pressed == KEY_R) {
        cameraReset(cameraEntity);
    }

    // if (input.ctrl && input.pressed == KEY_P) {
    //     toggleStutterTest(cameraEntity);
    // }

    if (stutterTestMode) {
        if (input.pressed == KEY_KP_PLUS) {
            stutterTestSpeed += .25f;
        }
        if (input.pressed == KEY_KP_MINUS) {
            stutterTestSpeed -= .25f;
        }
        if (input.pressed == KEY_KP_ENTER) {
            stutterTestSpeed = .25f;
        }
    }

    if (flying && input.pressed == KEY_ESCAPE) {
        toggleFlying(cameraEntity);
    }
    if (stutterTestMode && input.pressed == KEY_ESCAPE) {
        toggleStutterTest(cameraEntity);
    }

    if (input.ctrl && input.shift && input.pressed == KEY_T) {
        toggleShimmerTest(cameraEntity);
    }

    if (shimmerTestMode && input.pressed == KEY_ESCAPE) {
        toggleShimmerTest(cameraEntity);
    }
}

void flyingCameraUpdate(u32 cameraEntity) {
    if (flying) {
        vulkanResourceSetCameraOccluders(NULL, NULL, 0);
        transformSaveLast(scene, cameraEntity);
        checkKeyboardMovement(cameraEntity);
        checkMouseMovement(cameraEntity);
        checkScrollZoom(cameraEntity);

        // Prevent camera from passing through terrain by casting rays
        // in 5 directions and pushing back when too close to a surface.
        // Hold Ctrl to disable collision and pass through terrain.
        if (!input.ctrl) {
            auto transform      = getComponent(ecs.defaultScene, Transform, cameraEntity);
            const float minDist = 0.5f;
            const float rayLen  = 2.0f;

            vec3 dirs[5] = {
                {0.0f, -1.0f, 0.0f},  // down
                {0.0f, 0.0f, -1.0f},  // forward (-Z)
                {0.0f, 0.0f, 1.0f},   // backward (+Z)
                {-1.0f, 0.0f, 0.0f},  // left
                {1.0f, 0.0f, 0.0f},   // right
            };

            for (int i = 0; i < 5; i++) {
                vec3 hit;
                if (joltCastRay(transform->pos, dirs[i], rayLen, hit)) {
                    float dist = glm_vec3_distance(transform->pos, hit);
                    if (dist < minDist) {
                        // Push camera away from the surface
                        float pushBack = minDist - dist;
                        vec3 pushDir;
                        glm_vec3_negate_to(dirs[i], pushDir);
                        glm_vec3_muladds(pushDir, pushBack, transform->pos);
                    }
                }
            }
        }

        transformActivate(scene, cameraEntity);
    }

    if (stutterTestMode) {
        transformSaveLast(scene, cameraEntity);
        stutterTest(cameraEntity);
    }

    if (shimmerTestMode) {
        transformSaveLast(scene, cameraEntity);
        shimmerTest(cameraEntity);
    }
}

void flyingCameraPostUpdate(void) {
    if (!flyingCameraGameplayLoaded) return;

    static double lastSave;
    double now = utils::millies();
    if (now > lastSave + 1000) {
        lastSave = now;
        flyingCameraSaveStatus(flying);
    }
}

static void printSpeed(Transform* transform) {
    static double lastPrinted;
    static vec3 last;
    double now = utils::millies();
    if (now > lastPrinted + 1000) {
        float distance = glm_vec3_distance(transform->pos, last);
        if (distance) utils::info("change in a second %.2lf", distance);
        lastPrinted = now;
        glm_vec3_copy(transform->pos, last);
    }
}

void checkKeyboardMovement(u32 cameraEntity) {
    auto transform = getComponent(ecs.defaultScene, Transform, cameraEntity);
    if (input.shift) {
        if (flySpeedMultiplier < 50.F) {
            flySpeedMultiplier = 50.F;
        }
        if (flySpeedMultiplier < 350.F) {
            flySpeedMultiplier += utils::timer.dt * 10.F;
        }
    } else {
        flySpeedMultiplier = 5.0F;
    }
    if (input.alt) {
        flySpeedMultiplier = 1000.F;
    }

    printSpeed(transform);

    if (input.ctrl) {
        flySpeedMultiplier = .5F;
    }

    vec4 temp = {};
    vec3 direction;
    transformGetDirection(scene, cameraEntity, direction);

    if (input.repeating[KEY_A]) {
        glm_vec3_cross(direction, Y_UP, temp);
        glm_vec3_muladds(temp, -flySpeed * flySpeedMultiplier * utils::timer.dt, transform->pos);
    }

    if (input.repeating[KEY_W]) {
        glm_vec3_muladds(direction, flySpeed * flySpeedMultiplier * utils::timer.dt, transform->pos);
    }

    if (input.repeating[KEY_S]) {
        glm_vec3_muladds(direction, -flySpeed * flySpeedMultiplier * utils::timer.dt, transform->pos);
    }

    if (input.repeating[KEY_D]) {
        glm_vec3_cross(direction, Y_UP, temp);
        glm_vec3_muladds(temp, flySpeed * flySpeedMultiplier * utils::timer.dt, transform->pos);
    }
    if (input.repeating[KEY_SPACE]) {
        transform->pos[1] += flySpeed * flySpeedMultiplier * utils::timer.dt;
    }
    if (input.repeating[KEY_X]) {
        transform->pos[1] -= flySpeed * flySpeedMultiplier * utils::timer.dt;
    }
}

static void checkScrollZoom(u32 cameraEntity) {
    if (pendingScrollY == 0) return;

    auto transform = getComponent(ecs.defaultScene, Transform, cameraEntity);
    vec3 direction;
    transformGetDirection(scene, cameraEntity, direction);

    float zoomSpeed = flySpeed * flySpeedMultiplier;
    glm_vec3_muladds(direction, pendingScrollY * zoomSpeed, transform->pos);
    pendingScrollY = 0;
}

void checkMouseMovement(u32 cameraEntity) {
    auto transform = getComponent(ecs.defaultScene, Transform, cameraEntity);
    auto camera    = getComponent(ecs.defaultScene, Camera, cameraEntity);

    if (!transform) {
        utils::terminate("camera should have a transform kek");
        return;
    }

    // Use the mouse delta captured once per render frame in preUpdate.
    // The first-frame skip (flyingFrame) is already handled there.
    float mouse_dx = pendingMouseDx;
    float mouse_dy = pendingMouseDy;
    pendingMouseDx = 0;
    pendingMouseDy = 0;

    if (mouse_dx == 0 && mouse_dy == 0) {
        return;
    }

    float yaw_delta_rad   = glm_rad(-mouse_dx * sensitivity * utils::timer.dt);
    float pitch_delta_rad = glm_rad(-mouse_dy * sensitivity * utils::timer.dt);

    camera->yaw += yaw_delta_rad;
    camera->pitch += pitch_delta_rad;

    const float pitch_limit_rad = glm_rad(88.F);
    camera->pitch               = glm_clamp(camera->pitch, -pitch_limit_rad, pitch_limit_rad);

    versor q_pitch = {};
    versor q_yaw   = {};
    glm_quatv(q_pitch, camera->pitch, X_AXIS);
    glm_quatv(q_yaw, camera->yaw, Y_UP);
    glm_quat_mul(q_yaw, q_pitch, transform->rot);
    glm_quat_normalize(transform->rot);
}

void toggleFlying(u32 cameraEntity) {
    auto camera    = getComponent(ecs.defaultScene, Camera, cameraEntity);
    auto transform = getComponent(ecs.defaultScene, Transform, cameraEntity);

    char wasFlying  = flying;
    stutterTestMode = false;
    shimmerTestMode = false;
    flying          = !flying;
    flyingFrame     = flying;

    if (flying) {
        windowSystemHideCursor();
        // Drain any accumulated relative mouse delta so the first real
        // frame after entering flying mode starts clean.
        float dummyX, dummyY;
        windowSystemGetRelativeMouseDelta(&dummyX, &dummyY);
    } else {
        windowSystemShowCursor();
    }

    flyingCameraSaveStatus(wasFlying || flying);
    transformQuatToPitchYaw(transform->rot, &camera->pitch, &camera->yaw);
    transformSaveLast(scene, cameraEntity);
}

void cameraReset(u32 cameraEntity) {
    if (originalLocation[0] == 0 && originalLocation[1] == 0 && originalLocation[2] == 0 &&
        originalRotation[0] == 0 && originalRotation[1] == 0 && originalRotation[2] == 0 &&
        originalRotation[3] == 0) {
        return;
    }

    auto transform = getComponent(ecs.defaultScene, Transform, cameraEntity);
    auto camera    = getComponent(ecs.defaultScene, Camera, cameraEntity);
    // transformSetGlobal(cameraEntity, 1);

    camera->yaw   = originalYaw;
    camera->pitch = originalPitch;
    memcpy(transform->pos, originalLocation, sizeof(vec4));
    memcpy(transform->rot, originalRotation, sizeof(vec4));
    transformSaveLast(scene, cameraEntity);

    stutterTestMode = 0;
    shimmerTestMode = 0;
    flying          = 0;
    scanMode        = 0;
    windowSystemShowCursor();
    flyingCameraSaveStatus(0);
}

void toggleShimmerTest(u32 cameraEntity) {
    flying          = 0;
    shimmerTestMode = !shimmerTestMode;

    if (shimmerTestMode) {
        auto transform = getComponent(ecs.defaultScene, Transform, cameraEntity);
        vec3 direction;
        transformGetDirection(scene, cameraEntity, direction);
        glm_normalize(direction);

        glm_vec3_copy(direction, shimmerTestDirection);
        glm_vec3_copy(transform->pos, shimmerTestOriginalPos);
        shimmerTestTimer = 0.0f;
    }
}

void toggleStutterTest(u32 cameraEntity) {
    // if (flying || stutterTestMode) {
    // cameraReset(camera);
    // }

    flying           = 0;
    shimmerTestMode  = 0;
    stutterTestMode  = !stutterTestMode;
    stutterTestAngle = 0.0F;

    if (stutterTestMode) {
        auto transform = getComponent(ecs.defaultScene, Transform, cameraEntity);
        vec3 direction;
        transformGetDirection(scene, cameraEntity, direction);

        vec3 touch;
        if (joltCastRay(transform->pos, direction, 5000, touch)) {
            glm_vec3_copy(touch, stutterTestCircleAround);

            stutterTestDistance = glm_vec3_distance(stutterTestCircleAround, transform->pos);
            utils::info("yup you touch my tralala %f %f %f dist:%f",
                 touch[0],
                 touch[1],
                 touch[2],
                 stutterTestDistance);
        } else {
            stutterTestDistance = 500.0F;
            glm_vec3_scale(direction, stutterTestDistance, direction);
            glm_vec3_add(transform->pos, direction, stutterTestCircleAround);
        }

        stutterTestSpeed = .8F;
    }
}

void look_at(vec3 objectLocation, vec3 target, versor objectRotationOut) {
    mat4 lookAtMatrix;
    glm_lookat(objectLocation, target, Y_UP, lookAtMatrix);
    glm_mat4_inv(lookAtMatrix, lookAtMatrix);
    glm_mat4_quat(lookAtMatrix, objectRotationOut);
}

void shimmerTest(u32 cameraEntity) {
    auto transform = getComponent(ecs.defaultScene, Transform, cameraEntity);

    shimmerTestTimer += utils::timer.dt;

    // Oscillation period: 4 seconds total
    // 0-2s: move forward (0 -> 1)
    // 2-4s: move backward (1 -> 0)
    float cycleTime = 4.0f;
    float phase     = fmodf(shimmerTestTimer, cycleTime) / cycleTime;  // [0, 1)

    // Triangle wave: 0 -> 1 -> 0
    float t;
    if (phase < 0.5f) {
        t = phase * 2.0f;  // 0 -> 1 during first half
    } else {
        t = (1.0f - phase) * 2.0f;  // 1 -> 0 during second half
    }

    float dist = t * shimmerTestSpeed;

    // Position = original + direction * dist
    vec3 displacement;
    glm_vec3_scale(shimmerTestDirection, dist, displacement);
    glm_vec3_add(shimmerTestOriginalPos, displacement, transform->pos);

    // Rotation never changes — do nothing with rotation
}

void stutterTest(u32 cameraEntity) {
    auto transform = getComponent(ecs.defaultScene, Transform, cameraEntity);
    if (input.repeating[KEY_W]) {
        vec3 direction;
        transformGetDirection(scene, cameraEntity, direction);
        glm_vec3_scale(direction, utils::timer.dt / 10, direction);
        glm_vec3_add(transform->pos, direction, transform->pos);

        vec3 temp1;
        vec3 temp2;
        glm_vec3_copy(transform->pos, temp1);
        glm_vec3_copy(stutterTestCircleAround, temp2);
        temp1[1] = 0;
        temp2[1] = 0;

        stutterTestDistance = glm_vec3_distance(temp1, temp2);

        // stutterTestDistance -= 5.F;
    }
    if (input.repeating[KEY_S]) {
        stutterTestDistance += 5.F;
    }

    stutterTestAngle += utils::timer.dt * stutterTestSpeed;
    if (stutterTestAngle > 2.0F * M_PI) {
        stutterTestAngle -= 2.0F * M_PI;
    }

    transform->pos[0] = stutterTestCircleAround[0] + sinf(stutterTestAngle) * stutterTestDistance;
    transform->pos[2] = stutterTestCircleAround[2] + cosf(stutterTestAngle) * stutterTestDistance;

    mat4 lookAtMatrix;
    glm_lookat(transform->pos, stutterTestCircleAround, Y_UP, lookAtMatrix);
    glm_mat4_quat(lookAtMatrix, transform->rot);
    glm_quat_inv(transform->rot, transform->rot);
}

char flyingCameraIsActive(void) {
    return flying;
}

static void flyingCameraDbInit(void) {
    if (!utils::sqliteTableExists("flying_camera")) {
        utils::sqliteExecute(
            "CREATE TABLE IF NOT EXISTS flying_camera ("
            "name TEXT PRIMARY KEY, "
            "data BLOB);");
    }
}

void flyingCameraLoadForGameplay(void) {
    if (!scene || !flyingCameraEntity) return;

    flyingCameraGameplayLoaded = 1;

    char savedActive = 0;
    if (!flyingCameraLoadStatus(&savedActive)) return;

    char wasFlying  = flying;
    flying          = savedActive;
    flyingFrame     = flying;
    stutterTestMode = 0;
    shimmerTestMode = 0;
    pendingMouseDx  = 0;
    pendingMouseDy  = 0;
    pendingScrollY  = 0;

    Transform* transform = getComponent(scene, Transform, flyingCameraEntity);
    Camera* camera       = getComponent(scene, Camera, flyingCameraEntity);

    if (flying) {
        if (transform && transformDbLoad("flying_camera", transform)) {
            transform->pos[3] = 1.0f;
            transformSaveLast(scene, flyingCameraEntity);
            transformActivate(scene, flyingCameraEntity);
        }
        if (transform && camera) {
            transformQuatToPitchYaw(transform->rot, &camera->pitch, &camera->yaw);
            const float pitch_limit_rad = glm_rad(88.0F);
            camera->pitch = glm_clamp(camera->pitch, -pitch_limit_rad, pitch_limit_rad);
        }
        windowSystemHideCursor();
        float dummyX, dummyY;
        windowSystemGetRelativeMouseDelta(&dummyX, &dummyY);
        utils::info("flying camera: restored active state for gameplay");
    } else if (wasFlying) {
        windowSystemShowCursor();
    }
}

static void flyingCameraSaveStatus(char saveTransform) {
    FlyingCameraDb data = {
        .active = flying ? 1 : 0,
    };

    void* stmt = utils::sqliteStatement("REPLACE INTO flying_camera (name, data) VALUES (?, ?);");
    utils::sqliteBindText(stmt, 1, "status");
    utils::sqliteBindBlob(stmt, 2, &data, sizeof(data));
    utils::sqliteStep(stmt);
    utils::sqliteFinalize(stmt);

    if (saveTransform && scene && flyingCameraEntity) {
        Transform* transform = getComponent(scene, Transform, flyingCameraEntity);
        if (transform) transformDbSave("flying_camera", transform);
    }
}

static char flyingCameraLoadStatus(char* activeOut) {
    if (!activeOut) return 0;

    void* stmt  = utils::sqliteStatement("SELECT data, length(data) FROM flying_camera WHERE name = ?;");
    char result = 0;
    utils::sqliteBindText(stmt, 1, "status");
    if (utils::sqliteStep(stmt)) {
        FlyingCameraDb data = {};
        void* blob          = utils::sqliteGetBlob(stmt, 0);
        int blobSize        = utils::sqliteGetInt(stmt, 1);
        if (blob && blobSize > 0) {
            size_t copySize = (size_t)blobSize;
            if (copySize > sizeof(data)) copySize = sizeof(data);
            memcpy(&data, blob, copySize);
            *activeOut = data.active ? 1 : 0;
            result     = 1;
        }
    }
    utils::sqliteFinalize(stmt);
    return result;
}

void saveOriginalRT(u32 cameraEntity) {
    if (originalLocation[0] == 0 && originalLocation[1] == 0 && originalLocation[2] == 0 &&
        originalRotation[0] == 0 && originalRotation[1] == 0 && originalRotation[2] == 0 &&
        originalRotation[3] == 0) {
        auto transform = getComponent(scene, Transform, cameraEntity);
        auto camera    = getComponent(scene, Camera, cameraEntity);
        memcpy(originalLocation, transform->pos, sizeof(vec4));
        memcpy(originalRotation, transform->rot, sizeof(vec4));
        originalYaw   = camera->yaw;
        originalPitch = camera->pitch;
    }
}
}  // namespace engine
