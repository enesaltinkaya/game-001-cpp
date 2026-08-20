#include "ecs/Ecs.h"
#include "ecs/system/System.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/flyingCamera/FlyingCamera.h"
#include "ecs/system/transform/TransformComponent.h"
#include "ecs/system/transform/TransformDb.h"
#include "ecs/system/transform/TransformSystem.h"
#include "ecs/system/window/WindowSystem.h"
#include "renderer/Renderer.h"
#include "timer/Timer.h"

static void added(void);
static void preUpdate(void);
static void update(void);
static void postUpdate(void);
static void perspective(u32 cameraEntity);

static Entity* cameraEntityObj;
static u32 cameraEntity;
static Scene* scene;

struct System cameraSystem = {
    .name       = "camera",
    .added      = added,
    .preUpdate  = preUpdate,
    .update     = update,
    .postUpdate = postUpdate,
};

void added(void) {
    scene = ecs.defaultScene;

    cameraEntityObj = createEntity(scene, "camera");
    cameraEntity    = cameraEntityObj->id;
    auto camera     = createComponent(scene, Camera, cameraEntity);

    camera->aspectRatio = 1.77F;
    camera->yfov        = glm_rad(40.f);
    camera->znear       = 0.20F;
    camera->zfar        = 4096.0F;
    // TEMP DEBUG: ENGINE_CAM_ZFAR (metres) overrides the far plane for
    // distant-landmark screenshots (e.g. the volcano from 5 km).
    {
        const char* zfarEnv = getenv("ENGINE_CAM_ZFAR");
        if (zfarEnv && *zfarEnv) camera->zfar = strtof(zfarEnv, NULL);
    }
    camera->exposure    = 1.0f;

    auto transform = createComponent(scene, Transform, cameraEntity);
    createComponent(scene, LastTransform, cameraEntity);

    if (!transformDbLoad("camera", transform)) {
        transform->pos[0] = 59.51;
        transform->pos[1] = 1.66;
        transform->pos[2] = 28.87;
        transform->pos[3] = 1.0;
        transform->rot[0] = -0.01742;
        transform->rot[1] = 0.97608;
        transform->rot[2] = 0.19918;
        transform->rot[3] = 0.08538;
    }
    transform->pos[3] = 1.0;

    transformQuatToPitchYaw(transform->rot, &camera->pitch, &camera->yaw);
    const float pitch_limit_rad = glm_rad(88.0F);
    camera->pitch               = glm_clamp(camera->pitch, -pitch_limit_rad, pitch_limit_rad);

    // DEBUG: offset yaw for IBL investigation
    const char* yawOff = getenv("ENGINE_YAW_OFFSET");
    if (yawOff && *yawOff) {
        camera->yaw += glm_rad((float)atof(yawOff));
    }

    flyingCameraInit(cameraEntity);
}

void preUpdate(void) {
    flyingCameraPreUpdate(cameraEntity);
}

void update(void) {
    flyingCameraUpdate(cameraEntity);
}

void postUpdate(void) {
    Camera* camera = getComponent(scene, Camera, cameraEntity);
    perspective(cameraEntity);
    rendererSetCamera(camera);

    static double lastSave;
    double now = millies();
    if (now > lastSave + 1000) {
        lastSave             = now;
        Transform* transform = getComponent(scene, Transform, cameraEntity);
        transformDbSave("camera", transform);
    }
    flyingCameraPostUpdate();
}

void perspective(u32 cameraEntity) {
    auto camera = getComponent(scene, Camera, cameraEntity);

    camera->aspectRatio           = window.ratio;
    camera->cameraUbo.viewport[0] = (float)window.renderWidth;
    camera->cameraUbo.viewport[1] = (float)window.renderHeight;
    camera->cameraUbo.znear       = camera->znear;
    camera->cameraUbo.zfar        = camera->zfar;

    auto transform = getComponent(scene, Transform, cameraEntity);
    auto last      = getComponent(scene, LastTransform, cameraEntity);

    vec3 currentPos;
    vec3 currentDir;

    if (last) {
        vec3 direction;
        transformGetDirection(scene, cameraEntity, direction);
        vec3 lastDirection;
        glm_quat_rotatev(last->rot, GLM_FORWARD, lastDirection);
        glm_vec3_normalize(lastDirection);

        glm_vec3_lerp(last->pos, transform->pos, timer.alpha, currentPos);
        glm_vec3_lerp(lastDirection, direction, timer.alpha, currentDir);
    } else {
        vec3 direction;
        transformGetDirection(scene, cameraEntity, direction);
        glm_vec3_copy(transform->pos, currentPos);
        glm_vec3_copy(direction, currentDir);
    }

    /* TAA ghost test: dolly the camera forward/back along its view direction
     * (amplitude = ENGINE_TAA_GHOST_DOLLY, metres).  Applied to the UBO
     * position (not the transform) so it works regardless of which camera mode
     * is driving the transform.  The velocity pre-pass reads the UBO's view,
     * so it captures this motion — letting us check disocclusion ghosting. */
    {
        static float dollyAmp = 0.0f;
        static char dollyInit = 0;
        if (!dollyInit) {
            dollyInit = 1;
            const char* env = getenv("ENGINE_TAA_GHOST_DOLLY");
            if (env && *env) dollyAmp = (float)atof(env);
        }
        if (dollyAmp != 0.0f) {
            float dist = sinf((float)camera->frameIndex * 0.03f) * dollyAmp;
            vec3 off;
            glm_vec3_scale(currentDir, dist, off);
            glm_vec3_add(currentPos, off, currentPos);
        }
    }

    glm_vec3_copy(currentPos, camera->cameraUbo.renderLocation);
    camera->cameraUbo.renderLocation[3] = 1.0f;

    glm_vec3_copy(currentDir, camera->cameraUbo.renderDirection);
    camera->cameraUbo.renderDirection[3] = 0.0f;

    glm_look(currentPos, currentDir, GLM_YUP, camera->cameraUbo.view);

    // Build the stable projection.
    mat4 projectionNoJitter;
    glm_perspective(camera->yfov,
                    camera->aspectRatio,
                    camera->zfar,   // <--- Passed as "near" (Reverse-Z)
                    camera->znear,  // <--- Passed as "far"  (Reverse-Z)
                    projectionNoJitter);

    // Stable VP (used for velocity calculation and frustum planes)
    mat4 vpNoJitter;
    glm_mat4_mul(projectionNoJitter, camera->cameraUbo.view, vpNoJitter);
    glm_mat4_copy(vpNoJitter, camera->cameraUbo.viewProjectionNoJitter);
    glm_mat4_inv(vpNoJitter, camera->cameraUbo.invViewProjectionNoJitter);

    camera->frameIndex++;
    camera->cameraUbo.frameIndex = camera->frameIndex;
    camera->cameraUbo.exposure   = camera->exposure;

    /* Apply sub-pixel jitter when upscaler is enabled. */
    mat4 projectionJittered;
    glm_mat4_copy(projectionNoJitter, projectionJittered);

    float prevJitterX             = camera->cameraUbo.jitterX;
    float prevJitterY             = camera->cameraUbo.jitterY;
    camera->cameraUbo.jitterX     = 0.0f;
    camera->cameraUbo.jitterY     = 0.0f;
    camera->cameraUbo.prevJitterX = prevJitterX;
    camera->cameraUbo.prevJitterY = prevJitterY;

    if (window.renderWidth > 0 && window.renderHeight > 0) {
        float jitterX  = 0.0f;
        float jitterY  = 0.0f;
        char useJitter = 0;

        if ((rendererIsUpscalerEnabled() || rendererIsTAAEnabled()) && window.width > 0) {
            int32_t phaseCount =
                rendererGetJitterPhaseCount((uint32_t)window.renderWidth, (uint32_t)window.width);
            if (phaseCount > 0) {
                rendererGetJitterOffset(&jitterX,
                                        &jitterY,
                                        (int32_t)((camera->frameIndex - 1) % (u32)phaseCount),
                                        phaseCount);
                useJitter = 1;
            }
        }

        if (useJitter) {
            /* Convert from pixel offset to NDC (±1 range).
             *
             * cglm's RH_ZO perspective matrix has projection[2][3] = -1,
             * so clip.xy += projection[2][0..1] * v.z.  Because v.z is
             * negative (RH, camera looks down -Z), modifying projection[2][k]
             * by delta shifts ndc[k] by -delta.
             *
             * X: SUBTRACT jxNdc so ndc.x shifts by +jxNdc → image RIGHT.
             * Y: ADD jyNdc so ndc.y shifts by -jyNdc.  With the flipped
             *    viewport (negative height) ndc.y-down = screen-down, so
             *    the image moves DOWN by jitterY pixels, matching FSR's
             *    convention.                                               */
            float jxNdc = jitterX * 2.0f / (float)window.renderWidth;
            float jyNdc = jitterY * 2.0f / (float)window.renderHeight;
            projectionJittered[2][0] -= jxNdc;
            projectionJittered[2][1] += jyNdc;
            /* Store jitter as the UV unjitter offset.  Shaders recover
             * the unjittered position via:
             *   unjitteredUv = uv + vec2(jitterX, jitterY)
             *
             * X: Δ(uv.x) = +jxNdc/2       → unjitter = −jxNdc/2
             * Y: Δ(uv.y) = +jyNdc/2        → unjitter = −jyNdc/2
             *    (flipped viewport: uv.y = (1−ndc.y)/2, Δ(ndc.y) = −jyNdc
             *     → Δ(uv.y) = +jyNdc/2)                                   */
            camera->cameraUbo.jitterX = -jxNdc * 0.5f;
            camera->cameraUbo.jitterY = -jyNdc * 0.5f;
        }
    }

    glm_mat4_copy(projectionJittered, camera->cameraUbo.projection);

    glm_mat4_mul(camera->cameraUbo.projection,
                 camera->cameraUbo.view,
                 camera->cameraUbo.viewProjection);
    glm_mat4_inv(camera->cameraUbo.viewProjection, camera->cameraUbo.invViewProjection);
    glm_mat4_inv(camera->cameraUbo.view, camera->cameraUbo.invView);
    glm_mat4_inv(camera->cameraUbo.projection, camera->cameraUbo.invProjection);

    if (camera->frameIndex > 1) {
        glm_mat4_copy(camera->prevViewProjection, camera->cameraUbo.prevViewProjection);
        glm_mat4_copy(camera->prevViewProjectionNoJitter,
                      camera->cameraUbo.prevViewProjectionNoJitter);
    } else {
        glm_mat4_copy(camera->cameraUbo.viewProjection, camera->cameraUbo.prevViewProjection);
        glm_mat4_copy(vpNoJitter, camera->cameraUbo.prevViewProjectionNoJitter);
    }

    // Frustum planes from the stable VP.
    // Custom extraction for Vulkan ZO depth (glm_frustum_planes uses OpenGL NO formulas).
    // ZO clip volume: -w <= x <= w, -w <= y <= w, 0 <= z <= w
    //   left/right/bottom/top: same as OpenGL (w±x, w±y)
    //   near: z >= 0  → row2           (NOT row3+row2 which assumes z >= -w)
    //   far:  w-z >= 0 → row3 - row2   (same as OpenGL)
    {
        mat4 t;
        glm_mat4_transpose_to(vpNoJitter, t);
        vec4* fp = camera->cameraUbo.frustumPlanes;
        glm_vec4_add(t[3], t[0], fp[0]); /* left   */
        glm_vec4_sub(t[3], t[0], fp[1]); /* right  */
        glm_vec4_add(t[3], t[1], fp[2]); /* bottom */
        glm_vec4_sub(t[3], t[1], fp[3]); /* top    */
        glm_vec4_copy(t[2], fp[4]);      /* near   — ZO: z >= 0 */
        glm_vec4_sub(t[3], t[2], fp[5]); /* far    — ZO: w - z >= 0 */
        glm_plane_normalize(fp[0]);
        glm_plane_normalize(fp[1]);
        glm_plane_normalize(fp[2]);
        glm_plane_normalize(fp[3]);
        glm_plane_normalize(fp[4]);
        glm_plane_normalize(fp[5]);
    }

    glm_mat4_copy(camera->cameraUbo.viewProjection, camera->prevViewProjection);
    glm_mat4_copy(vpNoJitter, camera->prevViewProjectionNoJitter);
}

Entity* cameraGetEntity(void) {
    return cameraEntityObj;
}
