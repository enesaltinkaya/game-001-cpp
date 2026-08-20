#include "camera/TopDownCamera.h"

#include "ecs/Ecs.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/transform/TransformComponent.h"
#include "ecs/system/transform/TransformSystem.h"
#include "ecs/system/window/WindowSystem.h"
#include "renderer/vulkan/pass/shadow/VulkanShadowPass.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "timer/Timer.h"

// Default top-down camera params
static float TD_ELEVATION          = 30 * GLM_PIf / 180.0f;  // 30 degrees in radians
static float TD_DISTANCE           = 10.0f;  // default orbit distance from look-at point
static float TD_MIN_DIST           = 3.0f;   // minimum zoom-in distance
static float TD_MAX_DIST           = 40.0f;  // maximum zoom-out distance
static float TD_LOOK_HEIGHT        = 0.0f;   // look-at offset above player feet
static float TD_ROTATE_SENSITIVITY = 0.15f;  // match third-person mouse orbit

static float TD_ZOOM_SPEED = 15.0f;  // exponential-smooth interpolation rate

#define TD_OCCLUDER_ALPHA 0.35f
#define TD_OCCLUDER_RAY_COUNT 5
#define TD_OCCLUDER_MAX_STATES 32
#define TD_OCCLUDER_FADE_IN_SPEED 8.0f
#define TD_OCCLUDER_FADE_OUT_SPEED 8.0f
#define TD_OCCLUDER_DONE_ALPHA 0.9999f

static struct {
    u32 followEntityId;
    Scene* followScene;
    vec3 currentPos;
    char initPos;
    Entity* camEntity;
    Camera* camera;
    Transform* camTransform;
    float yaw;             // orbit yaw angle
    float targetDistance;  // desired orbit distance (set by input)
    float distance;        // current (smoothed) orbit distance
} tdCam;

typedef struct TopDownOccluderFade {
    u32 entity;
    float alpha;
    bool hitThisFrame;
} TopDownOccluderFade;

static TopDownOccluderFade tdOccluders[TD_OCCLUDER_MAX_STATES];
static u32 tdOccluderCount;

static void topDownCameraUpdateOccluders(vec3 cameraPos, vec3 targetPos) {
    u32 hitEntities[VULKAN_MAX_CAMERA_OCCLUDERS];
    u32 hitEntityCount = 0;

    vec3 viewRight = {1.0f, 0.0f, 0.0f};
    vec3 viewDir;
    glm_vec3_sub(targetPos, cameraPos, viewDir);
    viewDir[1] = 0.0f;
    if (glm_vec3_norm(viewDir) > 0.001f) {
        glm_vec3_normalize(viewDir);
        glm_vec3_cross(viewDir, GLM_YUP, viewRight);
        glm_vec3_normalize(viewRight);
    }

    vec3 rayTargets[TD_OCCLUDER_RAY_COUNT] = {
        {targetPos[0], targetPos[1] + 0.90f, targetPos[2]},
        {targetPos[0], targetPos[1] + 1.35f, targetPos[2]},
        {targetPos[0], targetPos[1] + 0.35f, targetPos[2]},
        {targetPos[0] + viewRight[0] * 0.35f,
         targetPos[1] + 0.90f,
         targetPos[2] + viewRight[2] * 0.35f},
        {targetPos[0] - viewRight[0] * 0.35f,
         targetPos[1] + 0.90f,
         targetPos[2] - viewRight[2] * 0.35f},
    };

    for (u32 r = 0; r < TD_OCCLUDER_RAY_COUNT; r++) {
        vec3 rayDir;
        glm_vec3_sub(rayTargets[r], cameraPos, rayDir);
        float rayLen = glm_vec3_norm(rayDir);
        if (rayLen < 0.001f) continue;
        glm_vec3_scale(rayDir, 1.0f / rayLen, rayDir);

        JoltRayHit hits[16];
        u32 hitCount = joltCastRayAll((float*)cameraPos, rayDir, rayLen, hits, 16);
        for (u32 i = 0; i < hitCount; i++) {
            u32 entity = (u32)hits[i].userData;
            if (entity == 0 || entity == tdCam.followEntityId) continue;

            bool duplicate = false;
            for (u32 e = 0; e < hitEntityCount; e++) {
                if (hitEntities[e] == entity) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;

            hitEntities[hitEntityCount++] = entity;
            if (hitEntityCount >= VULKAN_MAX_CAMERA_OCCLUDERS) break;
        }
        if (hitEntityCount >= VULKAN_MAX_CAMERA_OCCLUDERS) break;
    }

    for (u32 i = 0; i < tdOccluderCount; i++) {
        tdOccluders[i].hitThisFrame = false;
    }

    for (u32 h = 0; h < hitEntityCount; h++) {
        u32 stateIndex = TD_OCCLUDER_MAX_STATES;
        for (u32 i = 0; i < tdOccluderCount; i++) {
            if (tdOccluders[i].entity == hitEntities[h]) {
                stateIndex = i;
                break;
            }
        }

        if (stateIndex == TD_OCCLUDER_MAX_STATES && tdOccluderCount < TD_OCCLUDER_MAX_STATES) {
            stateIndex = tdOccluderCount++;
            tdOccluders[stateIndex] =
                (TopDownOccluderFade){.entity = hitEntities[h], .alpha = 1.0f};
        }

        if (stateIndex != TD_OCCLUDER_MAX_STATES) {
            tdOccluders[stateIndex].hitThisFrame = true;
        }
    }

    float dt = glm_clamp(timer.dt, 0.0f, 0.1f);
    for (u32 i = 0; i < tdOccluderCount;) {
        TopDownOccluderFade* occluder = &tdOccluders[i];
        float targetAlpha             = occluder->hitThisFrame ? TD_OCCLUDER_ALPHA : 1.0f;
        float speed                   = occluder->hitThisFrame ? TD_OCCLUDER_FADE_IN_SPEED
                                                               : TD_OCCLUDER_FADE_OUT_SPEED;
        float fade                    = 1.0f - expf(-speed * dt);
        occluder->alpha += (targetAlpha - occluder->alpha) * fade;

        if (occluder->hitThisFrame && occluder->alpha < TD_OCCLUDER_ALPHA + 0.001f) {
            occluder->alpha = TD_OCCLUDER_ALPHA;
        }

        if (!occluder->hitThisFrame && occluder->alpha > TD_OCCLUDER_DONE_ALPHA) {
            tdOccluders[i] = tdOccluders[--tdOccluderCount];
            continue;
        }

        i++;
    }

    u32 entities[VULKAN_MAX_CAMERA_OCCLUDERS];
    float alphas[VULKAN_MAX_CAMERA_OCCLUDERS];
    u32 entityCount = 0;
    for (u32 i = 0; i < tdOccluderCount && entityCount < VULKAN_MAX_CAMERA_OCCLUDERS; i++) {
        if (tdOccluders[i].alpha >= TD_OCCLUDER_DONE_ALPHA) continue;
        entities[entityCount] = tdOccluders[i].entity;
        alphas[entityCount]   = glm_clamp(tdOccluders[i].alpha, TD_OCCLUDER_ALPHA, 1.0f);
        entityCount++;
    }

    vulkanResourceSetCameraOccluders(entities, alphas, entityCount);
}

void topDownCameraInit(void) {
    tdCam.followScene    = NULL;
    tdCam.followEntityId = 0;
    tdCam.initPos        = 0;
    tdCam.yaw            = GLM_PIf / 4.0f;  // 45° default
    tdCam.targetDistance = TD_DISTANCE;
    tdCam.distance       = TD_DISTANCE;

    tdCam.camEntity    = cameraGetEntity();
    tdCam.camera       = getComponent(ecs.defaultScene, Camera, tdCam.camEntity->id);
    tdCam.camTransform = getComponent(ecs.defaultScene, Transform, tdCam.camEntity->id);
}

void topDownCameraSetTarget(Scene* scene, u32 entityId) {
    tdCam.followScene    = scene;
    tdCam.followEntityId = entityId;
    tdCam.initPos        = 0;

    // Immediately snap camera to target so the first frame isn't clipped
    if (!tdCam.camEntity || !tdCam.camera || !tdCam.camTransform) return;

    Transform* t = getComponent(scene, Transform, entityId);
    if (!t) return;

    vec3 targetPos = {t->pos[0], t->pos[1], t->pos[2]};

    vec3 lookAt;
    lookAt[0] = targetPos[0];
    lookAt[1] = targetPos[1] + TD_LOOK_HEIGHT;
    lookAt[2] = targetPos[2];

    float yaw   = tdCam.yaw;
    float pitch = TD_ELEVATION;
    float cp    = cosf(pitch);
    float sp    = sinf(pitch);

    vec3 desired;
    desired[0] = lookAt[0] - sinf(yaw) * cp * tdCam.distance;
    desired[1] = lookAt[1] + sp * tdCam.distance;
    desired[2] = lookAt[2] - cosf(yaw) * cp * tdCam.distance;

    if (desired[1] < targetPos[1] + 1.0f) desired[1] = targetPos[1] + 1.0f;

    glm_vec3_copy(desired, tdCam.camTransform->pos);
    glm_vec3_copy(desired, tdCam.currentPos);
    tdCam.camTransform->pos[3] = 1.0f;
    tdCam.initPos              = 1;

    mat4 lookMat;
    glm_lookat(tdCam.camTransform->pos, lookAt, GLM_YUP, lookMat);
    glm_mat4_quat(lookMat, tdCam.camTransform->rot);
    glm_quat_inv(tdCam.camTransform->rot, tdCam.camTransform->rot);

    transformSaveLast(ecs.defaultScene, tdCam.camEntity->id);
    transformQuatToPitchYaw(tdCam.camTransform->rot, &tdCam.camera->pitch, &tdCam.camera->yaw);
}

float topDownCameraGetYaw(void) {
    return tdCam.yaw;
}

void topDownCameraHandleInput(float dx) {
    if (dx != 0.0f) {
        tdCam.yaw -= dx * TD_ROTATE_SENSITIVITY * timer.dt;
    }
}

void topDownCameraSetDistance(float distance) {
    tdCam.targetDistance = glm_clamp(distance, TD_MIN_DIST, TD_MAX_DIST);
}

void topDownCameraScrollZoom(float scrollY) {
    if (scrollY != 0.0f) {
        tdCam.targetDistance =
            glm_clamp(tdCam.targetDistance - scrollY * 1.5f, TD_MIN_DIST, TD_MAX_DIST);
    }
}

float topDownCameraGetDistance(void) {
    return tdCam.distance;
}

void topDownCameraPreUpdate(void) {
    // Input is accumulated by the player system and applied with
    // topDownCameraHandleInput() during update.
}

void topDownCameraUpdate(void) {
    if (!tdCam.camEntity || !tdCam.camera || !tdCam.camTransform) {
        vulkanResourceSetCameraOccluders(NULL, NULL, 0);
        return;
    }

    // Smooth zoom: exponentially interpolate toward target distance
    float t = 1.0f - expf(-TD_ZOOM_SPEED * timer.dt);
    tdCam.distance += (tdCam.targetDistance - tdCam.distance) * t;

    // Get follow target position
    vec3 targetPos = {0.0f, 0.0f, 0.0f};
    bool hasTarget = false;
    if (tdCam.followScene && tdCam.followEntityId) {
        Transform* t = getComponent(tdCam.followScene, Transform, tdCam.followEntityId);
        if (t) {
            targetPos[0] = t->pos[0];
            targetPos[1] = t->pos[1];
            targetPos[2] = t->pos[2];
            hasTarget    = true;
        }
    }
    if (!hasTarget) {
        vulkanResourceSetCameraOccluders(NULL, NULL, 0);
    }

    // Look-at point: player feet + chest height
    vec3 lookAt;
    lookAt[0] = targetPos[0];
    lookAt[1] = targetPos[1] + TD_LOOK_HEIGHT;
    lookAt[2] = targetPos[2];

    // Yaw from mouse drag, elevation fixed at the configured angle
    float yaw   = tdCam.yaw;
    float pitch = TD_ELEVATION;

    // Camera position: offset behind and above the look-at point
    float cp = cosf(pitch);
    float sp = sinf(pitch);

    vec3 desired;
    desired[0] = lookAt[0] - sinf(yaw) * cp * tdCam.distance;
    desired[1] = lookAt[1] + sp * tdCam.distance;
    desired[2] = lookAt[2] - cosf(yaw) * cp * tdCam.distance;

    // Clamp Y to prevent underground
    if (desired[1] < targetPos[1] + 1.0f) desired[1] = targetPos[1] + 1.0f;

    transformSaveLast(ecs.defaultScene, tdCam.camEntity->id);

    if (hasTarget) {
        topDownCameraUpdateOccluders(desired, targetPos);
    }

    glm_vec3_copy(desired, tdCam.camTransform->pos);
    tdCam.camTransform->pos[3] = 1.0f;

    mat4 lookMat;
    glm_lookat(tdCam.camTransform->pos, lookAt, GLM_YUP, lookMat);
    glm_mat4_quat(lookMat, tdCam.camTransform->rot);
    glm_quat_inv(tdCam.camTransform->rot, tdCam.camTransform->rot);

    transformQuatToPitchYaw(tdCam.camTransform->rot, &tdCam.camera->pitch, &tdCam.camera->yaw);

    vulkanShadowPassSetFocusDistance(tdCam.distance);
    tdCam.camera->zfar = 4096.0f;
}

void topDownCameraUnproject(float screenX, float screenY, vec3 outOrigin, vec3 outDir) {
    if (!tdCam.camera) return;

    // Use window dimensions (not render viewport) because cursor coords
    // are reported in window pixel space, not render resolution space.
    float viewW = (float)window.width;
    float viewH = (float)window.height;
    if (viewW <= 0 || viewH <= 0) return;

    // Normalize screen coords to NDC
    float ndcX = (screenX / viewW) * 2.0f - 1.0f;
    float ndcY = -(screenY / viewH) * 2.0f + 1.0f;

    // Unproject near and far points
    vec4 nearClip = {ndcX, ndcY, -1.0f, 1.0f};
    vec4 farClip  = {ndcX, ndcY, 1.0f, 1.0f};

    vec4 nearWorld, farWorld;
    glm_mat4_mulv(tdCam.camera->cameraUbo.invViewProjection, nearClip, nearWorld);
    glm_mat4_mulv(tdCam.camera->cameraUbo.invViewProjection, farClip, farWorld);

    // Perspective divide
    nearWorld[0] /= nearWorld[3];
    nearWorld[1] /= nearWorld[3];
    nearWorld[2] /= nearWorld[3];
    farWorld[0] /= farWorld[3];
    farWorld[1] /= farWorld[3];
    farWorld[2] /= farWorld[3];

    glm_vec3_copy(nearWorld, outOrigin);
    glm_vec3_sub(farWorld, nearWorld, outDir);
    glm_vec3_normalize(outDir);
}
