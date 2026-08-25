#pragma once
#include "ecs/system/System.h"

namespace engine {
class RenderSystem : public System {
public:
    RenderSystem();
    void added() override;
    void removed() override;
    void postUpdate() override;
};

extern RenderSystem renderSystem;

struct Material;
struct Texture;
struct Mesh;
struct Skin;
struct Camera;
struct GpuLight;
struct DirectionalLightUbo;
struct LightUbo;
struct Transform;
struct Scene;

struct RendererSunLight {
    vec3 direction;
    vec3 color;
    float angularRadius;
};

#define MAX_MATERIALS 256
#define MAX_JOINTS 80
#define MAX_ENTITIES 100000
#define FRAMES_IN_FLIGHT 2

#define SAMPLER_NEAREST 0
#define SAMPLER_LINEAR 1
#define SAMPLER_NORMALMAP 3
#define SAMPLER_BORDER 5
#define SAMPLER_BORDER_NEAREST 6
#define SAMPLER_SHADOW_CMP 7
#define SAMPLER_CLAMP_LINEAR 8
#define SAMPLER_CLAMP_NEAREST 9

struct Renderer {
    std::vector<System*> passes;
    u32 drawCalls;
    u32 instanceCount;
    u32 triangleCount;
    double rendererElapsedCpu;
    double rendererElapsedGpu;
    int flightIndex;
};

extern struct Renderer renderer;

/*
 * Renderer.h is the public engine-facing renderer boundary.
 * ECS, scene parsing, and game code should talk to the renderer only through
 * this file, not through backend headers or Vulkan-shaped APIs.
 *
 * Near-term direction:
 * - keep resource lifetime here
 * - move frame submission toward renderer-owned aggregate inputs
 * - treat per-entity upload functions below as compatibility shims to replace
 *   once scene submission is redesigned
 */

void rendererWaitIdle(const char* reason);
void rendererSetVsync(bool vsync);

typedef enum AAMode {
    AA_OFF = 0,
    AA_TAA,
    AA_MODE_COUNT,
} AAMode;

/* Temporal AA. Mutually exclusive with the FSR upscaler:
 * enabling TAA forces the upscaler off, and enabling the upscaler
 * forces TAA off. */
void rendererSetAAMode(AAMode mode);
AAMode rendererGetAAMode(void);
bool rendererIsTAAEnabled(void);

struct RendererAASettings {
    float casStrength;    // RCAS strength 0–1.5 (0 = off, 1.0 = AMD reference max)
    float taaWeight;      // temporal blend factor (0.5–0.95, default 0.9)
    float taaGhost;       // color ghost rejection threshold (0.3–1.0, default 1.0)
    float taaDepth;       // temporal depth rejection threshold (0.01–0.5, default 0.06)
};

void rendererSetAASettings(RendererAASettings settings);
RendererAASettings rendererGetAASettings(void);

void rendererSetCasStrength(float strength);
float rendererGetCasStrength(void);

typedef enum RendererUpscalerMode {
    RENDERER_UPSCALER_OFF = 0,
    RENDERER_UPSCALER_NATIVE_AA,
    RENDERER_UPSCALER_QUALITY,
    RENDERER_UPSCALER_BALANCED,
    RENDERER_UPSCALER_PERFORMANCE,
    RENDERER_UPSCALER_ULTRA_PERFORMANCE,
    RENDERER_UPSCALER_COUNT,
} RendererUpscalerMode;

// upscaler
void rendererSetUpscalerMode(RendererUpscalerMode mode);
RendererUpscalerMode rendererGetUpscalerMode(void);
bool rendererIsUpscalerEnabled(void);

// render scale
float rendererNormalizeRenderScale(float scale);
void rendererSetRenderScale(float scale);
float rendererGetRenderScale(void);
void rendererUpdateRenderDimensions(void);
void rendererApplyRenderScale(void);

/////////////////////
/// resource lifetime
void rendererUploadTexture(Texture* texture, bool nonColor, bool genMips);
void rendererDestroyTexture(Texture* texture);

void rendererUploadMaterial(Material* material);
void rendererDestroyMaterial(Material* material);

void rendererSceneCreate(Scene* scene);
void rendererSceneDestroy(Scene* scene);

/////////////////////
/// frame state
void rendererSetCamera(const Camera* camera);
void rendererUploadSun(DirectionalLightUbo* directionalLight);
void rendererSetLighting(const LightUbo* lighting);
void rendererSetVisibleScenes(Scene** visibleScenes, u32 sceneCount);
/* Fixed scene sun (direction points TOWARD the sun; color is radiance).
 * IBL was removed — this is the single source of the sun for lighting,
 * shadowing and the debug GUI. */
RendererSunLight rendererGetSun(void);

/* Persistent scene-state upload APIs. These patch renderer-owned scene data
 * and are intentionally separate from frame-level state above. */
void rendererUploadTransform(struct Scene* scene, u32 entity, Transform* transform);
void rendererReserveJointSpace(Skin* skin);
void rendererUploadJoints(Skin* skin);

int32_t rendererGetJitterPhaseCount(u32 renderWidth, u32 displayWidth);
void rendererGetJitterOffset(float* jitterX, float* jitterY, int32_t index, int32_t phaseCount);
int rendererGetSwapchainImageCount(void);
double rendererGetSwapchainCpuElapsed(void);

/////////////////////
/// draw shapes
void rendererDrawBox(vec3 location, vec4 rotation, vec3 scale);
void rendererDrawSphere(vec3 location, vec4 rotation, vec3 scale);
void rendererDrawLine(vec3 location, vec3 direction);
void rendererDrawCapsule(vec3 location, vec4 rotation, vec3 scale);

/////////////////////
/// tonemapping
/* Tone/gamut mapping is done by the FFX LPM pass (vulkanLpmPass) — the
 * custom tonemapping curves (AgX/ACES/Filmic/...) were removed with the
 * LPM migration. LPM parameters live in the LPM pass. */
}  // namespace engine
