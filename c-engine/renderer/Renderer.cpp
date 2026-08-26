#include "renderer/Renderer.h"
#include "Utils.h"
#include "ecs/Ecs.h"
#include "ecs/system/System.h"
#include "ecs/system/window/WindowSystem.h"
#include "renderer/texture/TextureManager.h"
#include "ecs/system/light/LightComponent.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/scene/VulkanScene.h"
#include "renderer/vulkan/scene/VulkanVisibleScenes.h"
#include "renderer/vulkan/swapchain/VulkanSwapchain.h"
#include "renderer/vulkan/pass/ao/VulkanAOPass.h"
#include "renderer/vulkan/pass/bloom/VulkanBloomPass.h"
#include "renderer/vulkan/pass/contact_shadow/VulkanContactShadowPass.h"
#include "renderer/vulkan/pass/fsr/VulkanFsrUtils.h"
#include "renderer/vulkan/pass/shadow/VulkanShadowPass.h"
#include "renderer/vulkan/pass/ssr/VulkanSsrPass.h"
#include "renderer/vulkan/pass/volumetric/VulkanVolumetricPass.h"
#include "settings/Settings.h"
#include "material/MaterialManager.h"

namespace engine {
static AAMode sanitizeAAMode(AAMode mode);
static RendererAASettings sanitizeAASettings(RendererAASettings settings);
static RendererUpscalerMode sanitizeUpscalerMode(RendererUpscalerMode mode);
static float sanitizeRenderScale(float scale);

Renderer renderer;
static AAMode aaMode                     = AA_OFF;
static RendererUpscalerMode upscalerMode = RENDERER_UPSCALER_OFF;
static RendererAASettings aaSettings     = {
    .casStrength = 1.0f,
    .taaWeight   = 0.9f,
    .taaGhost    = 1.0f,
    .taaDepth    = 0.06f,
};
static float renderScale       = 1.0f;

RenderSystem renderSystem;

RenderSystem::RenderSystem() : System("renderer") {}

void RenderSystem::added() {
    utils::info("renderer: initializing null renderer stub");
    float savedScale = (float)utils::settingsGetDouble("renderScale");
    if (savedScale < 0.25f) {
        savedScale = 1.0f;
    }

    rendererSetUpscalerMode((RendererUpscalerMode)(int)utils::settingsGetDouble("upscalerMode"));
    rendererSetRenderScale(savedScale);
    /* taaEnabled is authoritative for TAA; a stale aaMode value must not
     * re-enable it. */
    rendererSetAAMode(utils::settingsGetBool("taaEnabled") ? AA_TAA : AA_OFF);
    rendererSetAASettings(RendererAASettings{
        .casStrength = (float)(utils::settingsGetDouble("aaCasStrength") / 100.0),
        .taaWeight   = (float)utils::settingsGetDouble("taaWeight"),
        .taaGhost    = (float)utils::settingsGetDouble("taaGhost"),
        .taaDepth    = (float)utils::settingsGetDouble("taaDepth"),
    });

    vulkanInit();
    createDefaultMaterial();
    textureManagerInit();

    /* Restore shadow quality (0=off 1=low 2=medium 3=high) from settings. */
    int shadowQuality = utils::settingsGetInt("shadowQuality");
    if (shadowQuality < 0 || shadowQuality >= engine::SHADOW_QUALITY_COUNT) {
        shadowQuality = engine::SHADOW_QUALITY_MEDIUM;
    }
    /* One-time migration from the legacy on/off toggle: an old settings file
     * without shadowQuality is seeded with the template default (medium), so
     * a user who had shadows disabled is honoured via the legacy key.  The
     * settings GUI keeps shadowsDisabled in sync, so this only ever fires
     * while the file predates the quality levels. */
    if (shadowQuality == engine::SHADOW_QUALITY_MEDIUM && utils::settingsGetBool("shadowsDisabled")) {
        shadowQuality = engine::SHADOW_QUALITY_OFF;
    }
    vulkanShadowPassSetQuality((engine::ShadowQuality)shadowQuality);

    if (utils::settingsGetBool("ssrDisabled")) vulkanSsrPassSetDisabled(1);
    if (utils::settingsGetBool("aoDisabled")) vulkanAOPassSetDisabled(1);
    if (utils::settingsGetBool("bloomDisabled")) vulkanBloomPassSetDisabled(1);
    if (utils::settingsGetBool("contactShadowDisabled")) vulkanContactShadowPassSetDisabled(1);

    /* Apply fog mode from settings */
    {
        int fogMode = (int)utils::settingsGetDouble("fogMode");
        if (fogMode < 0 || fogMode > 1) fogMode = 1;
        VulkanFogData fog = vulkanResourceGetFogData();
        switch (fogMode) {
            case 0: /* Off */
                fog.fogType = 0;
                vulkanVolumetricPassSetDisabled(1);
                break;
            case 1: /* On (exponential fog + god-rays) */
                fog.fogType = 2;
                vulkanVolumetricPassSetDisabled(0);
                break;
        }
        vulkanResourceSetFogData(fog);
    }

    utils::debug("renderer: renderer bridge initialized");
    utils::signalEmit("rendererInitialized", nullptr);
}

void RenderSystem::removed() {
    for (i32 i = (i32)static_cast<i32>(ecs.scenes.size()) - 1; i >= 0; i--) {
        rendererSceneDestroy(ecs.scenes[i]);
    }
    cleanupMaterials();
    textureManagerDestroy();
    vulkanDestroy();
}

void RenderSystem::postUpdate() {
    if (ecs.showStats) {
        renderSystem.cpuElapsed     = 0.0;
        renderer.rendererElapsedCpu = 0.0;
    }

    if (++renderer.flightIndex == FRAMES_IN_FLIGHT) {
        renderer.flightIndex = 0;
    }

    vulkanPostUpdate();
}

void rendererWaitIdle(const char* reason) {
    vulkanWaitIdle(reason);
}

void rendererSetVsync(bool vsync) {
    vulkanSetVsync(vsync);
}

void rendererSetAAMode(AAMode mode) {
    aaMode = sanitizeAAMode(mode);
    if (aaMode == AA_TAA && rendererIsUpscalerEnabled()) {
        /* Mutual exclusion: enabling TAA forces the FSR upscaler off. */
        rendererSetUpscalerMode(RENDERER_UPSCALER_OFF);
    }
}

AAMode rendererGetAAMode(void) {
    return aaMode;
}

bool rendererIsTAAEnabled(void) {
    return aaMode == AA_TAA;
}

void rendererSetAASettings(RendererAASettings settings) {
    aaSettings = sanitizeAASettings(settings);
}

RendererAASettings rendererGetAASettings(void) {
    return aaSettings;
}

void rendererSetCasStrength(float strength) {
    /* Above 1.0 (AMD reference max) amplifies the final-pass kernel's lobe
     * beyond the standard curve — player's choice, bounded by RCAS_LIMIT. */
    aaSettings.casStrength = strength < 0.0f ? 0.0f : (strength > 1.5f ? 1.5f : strength);
}

float rendererGetCasStrength(void) {
    return aaSettings.casStrength;
}

void rendererSetUpscalerMode(RendererUpscalerMode mode) {
    RendererUpscalerMode oldMode = upscalerMode;
    upscalerMode                 = sanitizeUpscalerMode(mode);
    if (upscalerMode != RENDERER_UPSCALER_OFF && aaMode == AA_TAA) {
        /* Mutual exclusion: enabling the FSR upscaler forces TAA off. */
        aaMode = AA_OFF;
    }
    rendererUpdateRenderDimensions();
    if (vulkan.device && upscalerMode != oldMode) {
        vulkanSwapchainRecreate();
    }
}

RendererUpscalerMode rendererGetUpscalerMode(void) {
    return upscalerMode;
}

bool rendererIsUpscalerEnabled(void) {
    return upscalerMode != RENDERER_UPSCALER_OFF;
}

void rendererUploadTexture(Texture* texture, bool nonColor, bool genMips) {
    if (!vulkan.device) return;  // vulkan not initialized yet
    vulkanLoadTexture(texture, nonColor, genMips);
}

void rendererDestroyTexture(Texture* texture) {
    vulkanDestroyImage(static_cast<VulkanImage*>(texture->backendImg), nullptr);
}

void rendererUploadMaterial(Material* material) {
    if (!vulkan.device) return;  // vulkan not initialized yet
    vulkanResourceUploadMaterial(material);
}

void rendererDestroyMaterial(Material* material) {
    (void)material;
}

void rendererUploadTransform(struct Scene* scene, u32 entity, struct Transform* transform) {
    vulkanSceneUploadTransform(scene, entity, transform);
}

void rendererReserveJointSpace(struct Skin* skin) {
    (void)skin;
}

void rendererUploadJoints(struct Skin* skin) {
    (void)skin;
}

void rendererSetCamera(const struct Camera* camera) {
    vulkanResourceUploadCamera((Camera*)camera);
}

void rendererUploadSun(DirectionalLightUbo* directionalLight) {
    vulkanResourceUploadDirectionalLight(directionalLight);
}

void rendererSetLighting(const LightUbo* lighting) {
    if (!lighting) return;
    vulkanResourceUploadLightUbo(lighting);
}

void rendererSetVisibleScenes(Scene** visibleScenes, u32 sceneCount) {
    vulkanSetVisibleScenes(visibleScenes, sceneCount);
}

void rendererSceneCreate(Scene* scene) {
    vulkanSceneCreate(scene);
}

void rendererSceneDestroy(Scene* scene) {
    rendererWaitIdle("scene destroy");
    vulkanSceneDestroy(scene);
}

RendererSunLight rendererGetSun(void) {
    /* Fixed sun — the scene is lit by direct sun + local lights only (IBL
     * was removed). Direction points TOWARD the sun; color is radiance.
     * LightSystem clamps its norm to 5.0 for the sun UBO intensity. */
    static const RendererSunLight sun = {
        .direction     = {0.3f, 0.8f, -0.5f},
        .color         = {2.6f, 2.4f, 2.08f},
        .angularRadius = 0.0087f,  // ~0.5° solar disc
    };
    return sun;
}

#define TAA_JITTER_PHASES 16

/* Sub-pixel TAA jitter: Halton sequence (bases 2 and 3), returned in
 * pixel units within [-0.5, 0.5] so each phase lands inside one pixel. */
static void taaJitterOffset(float* jitterX, float* jitterY, int32_t index) {
    double x = 0.0, y = 0.0;
    int ix = index + 1, iy = index + 1;
    double fx = 1.0, fy = 1.0;
    while (ix > 0) {
        fx /= 2.0;
        x += fx * (ix & 1);
        ix >>= 1;
    }
    while (iy > 0) {
        fy /= 3.0;
        y += fy * (iy % 3);
        iy /= 3;
    }
    if (jitterX) *jitterX = (float)(x - 0.5);
    if (jitterY) *jitterY = (float)(y - 0.5);
}

int32_t rendererGetJitterPhaseCount(u32 renderWidth, u32 displayWidth) {
    if (rendererIsUpscalerEnabled()) {
        return vulkanFsrGetJitterPhaseCount(renderWidth, displayWidth);
    }
    if (rendererIsTAAEnabled()) {
        return TAA_JITTER_PHASES;
    }
    return 0;
}

void rendererGetJitterOffset(float* jitterX, float* jitterY, int32_t index, int32_t phaseCount) {
    if (rendererIsUpscalerEnabled()) {
        if (phaseCount <= 0) {
            if (jitterX) *jitterX = 0.0f;
            if (jitterY) *jitterY = 0.0f;
            return;
        }
        vulkanFsrGetJitterOffset(jitterX, jitterY, index, phaseCount);
        return;
    }
    if (rendererIsTAAEnabled()) {
        taaJitterOffset(jitterX, jitterY, index);
        return;
    }
    if (jitterX) *jitterX = 0.0f;
    if (jitterY) *jitterY = 0.0f;
}

int rendererGetSwapchainImageCount(void) {
    return vulkanSwapchain.imageCount;
}

double rendererGetSwapchainCpuElapsed(void) {
    return 0.0;
}

void rendererDrawBox(vec3 location, vec4 rotation, vec3 scale) {
    (void)location;
    (void)rotation;
    (void)scale;
}

void rendererDrawSphere(vec3 location, vec4 rotation, vec3 scale) {
    (void)location;
    (void)rotation;
    (void)scale;
}

void rendererDrawLine(vec3 location, vec3 direction) {
    (void)location;
    (void)direction;
}

void rendererDrawCapsule(vec3 location, vec4 rotation, vec3 scale) {
    (void)location;
    (void)rotation;
    (void)scale;
}

static AAMode sanitizeAAMode(AAMode mode) {
    if (mode < AA_OFF || mode >= AA_MODE_COUNT) {
        return AA_OFF;
    }
    return mode;
}

static RendererAASettings sanitizeAASettings(RendererAASettings settings) {
    settings.casStrength = settings.casStrength < 0.0f ? 0.0f
                        : (settings.casStrength > 1.5f ? 1.5f : settings.casStrength);
    settings.taaWeight   = settings.taaWeight < 0.5f ? 0.5f : (settings.taaWeight > 0.95f ? 0.95f : settings.taaWeight);
    settings.taaGhost    = settings.taaGhost < 0.3f ? 0.3f : (settings.taaGhost > 1.0f ? 1.0f : settings.taaGhost);
    settings.taaDepth    = settings.taaDepth < 0.01f ? 0.01f : (settings.taaDepth > 0.5f ? 0.5f : settings.taaDepth);
    return settings;
}

static RendererUpscalerMode sanitizeUpscalerMode(RendererUpscalerMode mode) {
    if (mode < RENDERER_UPSCALER_OFF || mode >= RENDERER_UPSCALER_COUNT) {
        return RENDERER_UPSCALER_OFF;
    }
    return mode;
}

static float sanitizeRenderScale(float scale) {
    if (scale < 0.5f) scale = 0.5f;
    if (scale > 2.0f) scale = 2.0f;
    scale = (float)((int)(scale * 20.0f + 0.5f)) / 20.0f;
    return scale;
}

float rendererNormalizeRenderScale(float scale) {
    return sanitizeRenderScale(scale);
}

void rendererSetRenderScale(float scale) {
    float oldScale = renderScale;
    renderScale    = sanitizeRenderScale(scale);
    rendererUpdateRenderDimensions();
    if (vulkan.device && renderScale != oldScale) {
        vulkanSwapchainRecreate();
    }
}

void rendererApplyRenderScale(void) {}

float rendererGetRenderScale(void) {
    return renderScale;
}

void rendererUpdateRenderDimensions(void) {
    if (rendererIsUpscalerEnabled() && window.width > 0 && window.height > 0) {
        u32 rw = 0, rh = 0;
        if (vulkanFsrGetRenderResolution(rendererGetUpscalerMode(),
                                         (u32)window.width,
                                         (u32)window.height,
                                         &rw,
                                         &rh)) {
            window.renderWidth  = (int)rw;
            window.renderHeight = (int)rh;
            return;
        }
    }
    int rw = (int)(window.width * renderScale);
    int rh = (int)(window.height * renderScale);
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;
    window.renderWidth  = rw;
    window.renderHeight = rh;
}
}  // namespace engine
