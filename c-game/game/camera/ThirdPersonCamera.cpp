#include "camera/ThirdPersonCamera.h"

#include "ecs/Ecs.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/transform/TransformComponent.h"
#include "ecs/system/transform/TransformSystem.h"
#include "renderer/vulkan/pass/dof/VulkanDofPass.h"
#include "renderer/vulkan/pass/shadow/VulkanShadowPass.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "timer/Timer.h"

// ── Orbit camera parameters (1 unit = 1 meter, character height 1.39m) ──────
namespace game {
static float tpCameraDistance    = 10.0f;  // default orbit distance (m)
static float tpCameraSensitivity = 0.15f;
static float tpPitchMin          = -20.0f * GLM_PIf / 180.0f;
static float tpPitchMax          = 60.0f * GLM_PIf / 180.0f;
static float tpLookAtHeight      = 1.1f;  // camera look-at ~chest height
/* DoF focus height above the feet: the player transform sits at the feet, so
 * the focus point is offset up to the head to keep the character sharp. */
static const float tpDofFocusHeight = 1.5f;

static vec3 Y_UP = {0.0f, 1.0f, 0.0f};
static vec3 X_AXIS = {1.0f, 0.0f, 0.0f};

static struct {
    u32 followEntityId;
    engine::Scene* followScene;
    engine::Entity* camEntity;
    engine::Camera* camera;
    engine::Transform* camTransform;
    float yaw;
    float pitch;
    float distance;
    // Shared state exposed for player movement queries
    bool moving;
    bool anyDrag;
    float mouseY;      // raw dy from last input
    // Smooth obstacle recovery
    float smoothDist;
    // Sky-look tilt
    float skyPitchOffset;
} tpCam;

void thirdPersonCameraInit(void) {
    tpCam.followScene    = nullptr;
    tpCam.followEntityId = 0;
    tpCam.yaw            = glm_rad(180.0f);
    tpCam.pitch          = glm_rad(8.0f);
    tpCam.distance       = tpCameraDistance;
    tpCam.smoothDist     = -1.0f;
    tpCam.skyPitchOffset = 0.0f;
    tpCam.moving         = false;
    tpCam.anyDrag        = false;
    tpCam.mouseY         = 0.0f;

    tpCam.camEntity    = engine::cameraGetEntity();
    tpCam.camera       = getComponent(engine::ecs.defaultScene, engine::Camera, tpCam.camEntity->id);
    tpCam.camTransform = getComponent(engine::ecs.defaultScene, engine::Transform, tpCam.camEntity->id);
}

void thirdPersonCameraSetTarget(engine::Scene* scene, u32 entityId) {
    tpCam.followScene    = scene;
    tpCam.followEntityId = entityId;
}

void thirdPersonCameraHandleInput(float dx, float dy) {
    if (dx != 0.0f || dy != 0.0f) {
        tpCam.yaw   -= dx * tpCameraSensitivity * utils::timer.dt;
        tpCam.pitch += dy * tpCameraSensitivity * utils::timer.dt;
        tpCam.pitch  = glm_clamp(tpCam.pitch, tpPitchMin, tpPitchMax);
        tpCam.mouseY = dy;
    }
}

void thirdPersonCameraSetDistance(float distance) {
    tpCam.distance = glm_clamp(distance, 1.5f, 20.0f);
}

float thirdPersonCameraGetDistance(void) {
    return tpCam.distance;
}

float thirdPersonCameraGetYaw(void) {
    return tpCam.yaw;
}

float thirdPersonCameraGetPitch(void) {
    return tpCam.pitch;
}

void thirdPersonCameraSetAngles(float yaw, float pitch) {
    tpCam.yaw   = yaw;
    tpCam.pitch = glm_clamp(pitch, tpPitchMin, tpPitchMax);
}

bool thirdPersonCameraIsMoving(void) {
    return tpCam.moving;
}

void thirdPersonCameraSetMoving(bool moving) {
    tpCam.moving = moving;
}

bool thirdPersonCameraIsAnyDrag(void) {
    return tpCam.anyDrag;
}

void thirdPersonCameraSetAnyDrag(bool anyDrag) {
    tpCam.anyDrag = anyDrag;
}

void thirdPersonCameraSetMouseDy(float dy) {
    tpCam.mouseY = dy;
}

/* TAA ghost test: pan the orbit camera's yaw at a constant rate (deg/s) to
 * sweep the view across the scene.  This creates real disocclusions at the
 * screen edges and depth-dependent parallax at object silhouettes — the exact
 * conditions where temporal ghosting would show up.  ENGINE_TAA_GHOST_PAN. */
static float tpGhostPan = 0.0f;
static bool tpGhostPanInit = false;

void thirdPersonCameraUpdate(void) {
    engine::vulkanResourceSetCameraOccluders(nullptr, nullptr, 0);
    if (!tpCam.camEntity || !tpCam.camera || !tpCam.camTransform) return;
    if (!tpCam.followScene || !tpCam.followEntityId) return;

    if (!tpGhostPanInit) {
        tpGhostPanInit = true;
        const char* env = getenv("ENGINE_TAA_GHOST_PAN");
        if (env && *env) tpGhostPan = static_cast<float>(atof(env));
    }
    if (tpGhostPan != 0.0f) {
        tpCam.yaw += tpGhostPan * GLM_PIf / 180.0f * utils::timer.dt;
    }

    engine::Transform* transform = getComponent(tpCam.followScene, engine::Transform, tpCam.followEntityId);
    if (!transform) return;

    // ── Position camera behind player (orbit on a sphere) ──────────────
    float camYaw   = tpCam.yaw;
    float camPitch = tpCam.pitch;

    // Look-at point: player position + chest height
    vec3 playerPos;
    glm_vec3_copy(transform->pos, playerPos);
    playerPos[1] += tpLookAtHeight;

    // Camera offset from the look-at point: a fixed spherical DIRECTION
    // (from yaw/pitch) scaled by the zoom distance. Scaling the whole
    // vector means scrolling the wheel slides the camera along a straight
    // line (the view ray) toward/away from the character while preserving
    // the viewing angle — no arc, and no flip to top-down when close.
    float cp   = cosf(camPitch);
    float sp   = sinf(camPitch);
    vec3 dir   = {-sinf(camYaw) * cp, sp, -cosf(camYaw) * cp};
    vec3 offset;
    glm_vec3_scale(dir, tpCam.distance, offset);

    vec3 desiredCamPos;
    glm_vec3_add(playerPos, offset, desiredCamPos);
    float fullOrbitDist = glm_vec3_norm(offset);

    // ── Collision raycast (sphere approximation) ───────────────────────
    {
        static const float cameraRadius = 0.5f;

        vec3 mainDir;
        glm_vec3_sub(desiredCamPos, playerPos, mainDir);
        float mainLen = glm_vec3_norm(mainDir);

        if (mainLen > 0.001f) {
            glm_vec3_scale(mainDir, 1.0f / mainLen, mainDir);

            vec3 right, up;
            if (fabsf(mainDir[1]) < 0.99f) {
                glm_vec3_cross(mainDir, Y_UP, right);
            } else {
                glm_vec3_cross(mainDir, X_AXIS, right);
            }
            glm_vec3_normalize(right);
            glm_vec3_cross(right, mainDir, up);
            glm_vec3_normalize(up);

            vec3 offsets[5];
            glm_vec3_zero(offsets[0]);
            glm_vec3_scale(right, cameraRadius, offsets[1]);
            glm_vec3_scale(right, -cameraRadius, offsets[2]);
            glm_vec3_scale(up, cameraRadius, offsets[3]);
            glm_vec3_scale(up, -cameraRadius, offsets[4]);

            float closestDist = mainLen;

            for (int i = 0; i < 5; i++) {
                vec3 target;
                glm_vec3_add(desiredCamPos, offsets[i], target);
                vec3 rayDir;
                glm_vec3_sub(target, playerPos, rayDir);
                float rayLen = glm_vec3_norm(rayDir);
                if (rayLen < 0.001f) continue;
                glm_vec3_scale(rayDir, 1.0f / rayLen, rayDir);

                vec3 hit;
                if (joltCastRay(playerPos, rayDir, rayLen, hit)) {
                    float hitDist  = glm_vec3_distance(playerPos, hit);
                    float projected = hitDist - cameraRadius;
                    if (projected < closestDist) closestDist = projected;
                }
            }

            float safeDist = closestDist;
            if (safeDist < 0.05f) safeDist = 0.05f;
            if (safeDist < mainLen) {
                vec3 safeOffset;
                glm_vec3_scale(mainDir, safeDist, safeOffset);
                glm_vec3_add(playerPos, safeOffset, desiredCamPos);
            }
        }
    }

    // ── Smooth obstacle recovery ──────────────────────────────────────
    {
        vec3 probePos = {
            playerPos[0] + offset[0],
            playerPos[1] + offset[1],
            playerPos[2] + offset[2],
        };
        float fullDist    = glm_vec3_distance(playerPos, probePos);
        float clampedDist = glm_vec3_distance(playerPos, desiredCamPos);
        bool wasClamped   = (clampedDist < fullDist - 0.01f);

        if (tpCam.smoothDist < 0.0f) tpCam.smoothDist = clampedDist;

        if (wasClamped) {
            tpCam.smoothDist = clampedDist;
        } else {
            float t          = glm_clamp(12.0f * utils::timer.dt, 0.0f, 1.0f);
            tpCam.smoothDist = glm_lerp(tpCam.smoothDist, clampedDist, t);
        }

        if (fabsf(tpCam.smoothDist - clampedDist) > 0.001f) {
            vec3 dir;
            glm_vec3_sub(desiredCamPos, playerPos, dir);
            glm_vec3_normalize(dir);
            glm_vec3_scale(dir, tpCam.smoothDist, dir);
            glm_vec3_add(playerPos, dir, desiredCamPos);
        }
    }

    // ── Sky-look ──────────────────────────────────────────────────────
    {
        float actualCamDist = glm_vec3_distance(playerPos, desiredCamPos);
        bool cameraClipped  = (actualCamDist < fullOrbitDist - 0.2f);
        bool pitchAtMax     = (tpCam.pitch >= tpPitchMax - 0.01f);
        bool pushingUp      = (tpCam.mouseY > 0.0f);

        if (!tpCam.moving && cameraClipped && pitchAtMax && pushingUp && tpCam.anyDrag) {
            tpCam.skyPitchOffset += tpCam.mouseY * tpCameraSensitivity * utils::timer.dt;
            tpCam.skyPitchOffset  = glm_clamp(tpCam.skyPitchOffset, 0.0f, GLM_PIf * 0.44f);
        } else {
            if (tpCam.moving || !cameraClipped || tpCam.pitch < tpPitchMax - 0.05f) {
                float decay           = glm_clamp(5.0f * utils::timer.dt, 0.0f, 1.0f);
                tpCam.skyPitchOffset  = glm_lerp(tpCam.skyPitchOffset, 0.0f, decay);
                if (tpCam.skyPitchOffset < 0.001f) tpCam.skyPitchOffset = 0.0f;
            }
        }
    }

    // Save previous camera pose BEFORE writing the new one so that
    // render-frame interpolation (LastTransform → WorldTransform)
    // has two distinct endpoints to lerp between.
    engine::transformSaveLast(engine::ecs.defaultScene, tpCam.camEntity->id);

    glm_vec3_copy(desiredCamPos, tpCam.camTransform->pos);
    tpCam.camTransform->pos[3] = 1.0f;

    engine::vulkanShadowPassSetFocusDistance(fullOrbitDist);

    /* DoF focuses on the player's head: the transform is at the feet, so
     * offset the focus point up by the head height before measuring the
     * (obstacle-clamped) camera-to-subject distance. Keeps the character
     * sharp and blurs the background; follows wheel zoom. */
    vec3 dofFocusPoint;
    glm_vec3_copy(transform->pos, dofFocusPoint);
    dofFocusPoint[1] += tpDofFocusHeight;
    engine::vulkanDofPassSetFocusDistance(glm_vec3_distance(dofFocusPoint, desiredCamPos));

    // Camera looks at the player's look-at point, tilted up by sky-look offset.
    vec3 lookTarget;
    glm_vec3_copy(playerPos, lookTarget);
    if (tpCam.skyPitchOffset > 0.001f) {
        float camDist = glm_vec3_distance((float*)tpCam.camTransform->pos, playerPos);
        lookTarget[1] += tanf(tpCam.skyPitchOffset) * camDist;
    }

    mat4 lookMat;
    glm_lookat(tpCam.camTransform->pos, lookTarget, Y_UP, lookMat);
    glm_mat4_quat(lookMat, tpCam.camTransform->rot);
    glm_quat_inv(tpCam.camTransform->rot, tpCam.camTransform->rot);

    engine::transformQuatToPitchYaw(tpCam.camTransform->rot, &tpCam.camera->pitch, &tpCam.camera->yaw);
}
}  // namespace game
