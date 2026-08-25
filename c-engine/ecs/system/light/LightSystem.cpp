#include "ecs/system/light/LightSystem.h"
#include "ecs/Ecs.h"
#include "ecs/system/light/LightComponent.h"
#include "ecs/system/scene/SceneSystem.h"
#include "ecs/system/transform/TransformComponent.h"
#include "ecs/system/transform/TransformSystem.h"
#include "events/Events.h"
#include "renderer/Renderer.h"

namespace engine {
static void applySun(void* _);
static void buildGpuLight(Scene* scene, u32 entity, Light* light, GpuLight* out);

static LightUbo frameLightUbo;
static u32 frameLightCount;

LightSystem lightSystem;

LightSystem::LightSystem() : System("light") {}

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

void LightSystem::added() {
    utils::signalSubscribe("rendererInitialized", applySun);
}

static void applySun(void* _) {
    (void)_;  //
    RendererSunLight sun = rendererGetSun();
    DirectionalLightUbo sunUbo = {};
    vec3 negDir;
    glm_vec3_negate_to(sun.direction, negDir);
    float rawIntensity = glm_vec3_norm(sun.color);
    // Clamp sun intensity to a physically reasonable range.
    float intensity = fminf(rawIntensity, 5.0f);
    vec4 dirVec = {negDir[0], negDir[1], negDir[2], intensity};
    glm_vec4_copy(dirVec, sunUbo.direction);
    vec3 normalizedColor;
    if (intensity > 0.001f) {
        glm_vec3_scale(sun.color, 1.0f / intensity, normalizedColor);
    } else {
        vec3 white = {1.0f, 1.0f, 1.0f};
        glm_vec3_copy(white, normalizedColor);
    }
    vec4 colorVec = {normalizedColor[0], normalizedColor[1], normalizedColor[2], 0.0f};
    glm_vec4_copy(colorVec, sunUbo.color);
    /* Fixed slight ambient (sky tint): lifts shadowed areas just above
     * black.  IBL was removed; this is a constant, not a per-frame value. */
    vec4 ambientVec = {0.03f, 0.04f, 0.05f, 0.0f};
    glm_vec4_copy(ambientVec, sunUbo.ambient);
    rendererUploadSun(&sunUbo);
}

void LightSystem::update() {
}

void LightSystem::postUpdate() {
    memset(&frameLightUbo, 0, sizeof(frameLightUbo));
    frameLightCount = 0;

    ivec4 counts = {};
    std::vector<Scene*> visibleScenes = sceneSystemGetVisibleScenes();
    for (Scene* scene : visibleScenes) {
        utils::SparseSet* lights = getComponents(scene, Light);
        if (!lights) {
            continue;
        }

        for (u32 i = 0; i < lights->size; i++) {
            u32 entity = utils::ssGetValueByIndex(lights, i);
            Light* light  = static_cast<Light*>(utils::ssGetDataByIndex(lights, i));
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
}  // namespace engine
