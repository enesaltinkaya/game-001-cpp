#include "ecs/system/light/LightSystem.h"
#include "ecs/Ecs.h"
#include "ecs/system/light/LightComponent.h"
#include "ecs/system/scene/SceneSystem.h"
#include "ecs/system/transform/TransformComponent.h"
#include "ecs/system/transform/TransformSystem.h"
#include "events/Events.h"
#include "renderer/Renderer.h"

static void added(void);
static void update(void);
static void postUpdate(void);
static void applyIblSun(void* _);
static void buildGpuLight(Scene* scene, u32 entity, Light* light, GpuLight* out);

static LightUbo frameLightUbo;
static u32 frameLightCount;

struct System lightSystem = {
    .name       = "light",
    .added      = added,
    .update     = update,
    .postUpdate = postUpdate,
};

static void buildGpuLight(Scene* scene, u32 entity, Light* light, GpuLight* out) {
    WorldTransform* wt = transformGetWorld(scene, entity);
    if (!wt) {
        memset(out, 0, sizeof(*out));
        return;
    }

    glm_vec3_copy(wt->pos, out->positionAndRange);
    out->positionAndRange[3] = light->range;

    vec3 fwd = {0.0f, 0.0f, -1.0f};
    vec3 dir;
    glm_quat_rotatev(wt->rot, fwd, dir);
    glm_vec3_normalize(dir);
    glm_vec3_copy(dir, out->directionAndType);
    out->directionAndType[3] = static_cast<float>(light->lightType);

    glm_vec3_copy(light->color, out->colorAndIntensity);
    out->colorAndIntensity[3] = light->intensity;

    out->spotAngles[0] = cosf(light->innerConeAngle);
    out->spotAngles[1] = cosf(light->outerConeAngle);
    out->spotAngles[2] = 0.0f;
    out->spotAngles[3] = 0.0f;
}

void lightMarkDirty(Scene* scene, u32 entity) {
    (void)scene;
    (void)entity;
}

void added(void) {
    signalSubscribe("rendererInitialized", applyIblSun);
    signalSubscribe("iblChanged", applyIblSun);
}

static void applyIblSun(void* _) {
    (void)_;
    RendererSunLight iblSun = rendererGetExtractedSun();
    DirectionalLightUbo sun = {};
    vec3 negDir;
    glm_vec3_negate_to(iblSun.direction, negDir);
    float rawIntensity = glm_vec3_norm(iblSun.color);
    // Clamp sun intensity to a physically reasonable range.
    // HDR environment maps can have sun pixels in the tens of thousands,
    // which would blow out the entire scene.
    float intensity = fminf(rawIntensity, 5.0f);
    vec4 dirVec = {negDir[0], negDir[1], negDir[2], intensity};
    glm_vec4_copy(dirVec, sun.direction);
    vec3 normalizedColor;
    if (intensity > 0.001f) {
        glm_vec3_scale(iblSun.color, 1.0f / intensity, normalizedColor);
    } else {
        vec3 white = {1.0f, 1.0f, 1.0f};
        glm_vec3_copy(white, normalizedColor);
    }
    vec4 colorVec = {normalizedColor[0], normalizedColor[1], normalizedColor[2], 0.0f};
    glm_vec4_copy(colorVec, sun.color);
    vec4 ambientVec = {0.05f, 0.05f, 0.05f, 0.0f};
    glm_vec4_copy(ambientVec, sun.ambient);
    rendererUploadSun(&sun);
}

void update(void) {
}

void postUpdate(void) {
    memset(&frameLightUbo, 0, sizeof(frameLightUbo));
    frameLightCount = 0;

    ivec4 counts = {};
    Array(Scene*) visibleScenes = sceneSystemGetVisibleScenes();
    foreach (Scene* scene, visibleScenes) {
        SparseSet* lights = getComponents(scene, Light);
        if (!lights) {
            continue;
        }

        for (u32 i = 0; i < lights->size; i++) {
            u32 entity = ssGetValueByIndex(lights, i);
            Light* light  = static_cast<Light*>(ssGetDataByIndex(lights, i));
            if (!light) {
                continue;
            }

            if (frameLightCount >= MAX_GPU_LIGHTS) break;
            buildGpuLight(scene, entity, light, &frameLightUbo.lights[frameLightCount]);
            frameLightCount++;

            switch (light->lightType) {
                case LIGHT_DIRECTIONAL:
                    counts[0]++;
                    break;
                case LIGHT_POINT:
                    counts[1]++;
                    break;
                case LIGHT_SPOT:
                    counts[2]++;
                    break;
            }
        }
    }

    counts[3] = counts[0] + counts[1] + counts[2];
    glm_ivec4_copy(counts, frameLightUbo.counts);
    rendererSetLighting(&frameLightUbo);
}
