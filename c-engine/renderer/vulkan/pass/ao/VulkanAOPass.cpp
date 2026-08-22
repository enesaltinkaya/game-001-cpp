#include "VulkanAOPass.h"
#include "ecs/Ecs.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/window/WindowSystem.h"
#include "events/Events.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/pipeline/VulkanProfile.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include <stdlib.h>
#include <string.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <FidelityFX/host/ffx_cacao.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#pragma GCC diagnostic pop

namespace engine {

static void swapchainCreated(void* _);
static void cacaoDestroyContext(void);
static void cacaoDestroyOutput(void);
static char  cacaoEnsureContext(u32 width, u32 height);
static void  cacaoUpdate(VulkanCommand* cmd, VulkanImage* depth, VulkanImage* normals, Camera* camera);
static VkImageCreateInfo makeImageCreateInfo(VulkanImage* image);
static FfxResource wrapImageResource(VulkanImage* image,
                                     FfxResourceUsage usage,
                                     FfxResourceStates state,
                                     const wchar_t* name);
static void temporalDestroyAccumulators(void);
static char  temporalEnsureAccumulators(u32 width, u32 height);
static void  temporalDispatch(VulkanCommand* cmd, VulkanImage* depth, VulkanImage* velocity);

static double elapsedCPU;
static double elapsedGPU;
static char aoDisabled;

/* Env-parsed float with fallback ("" falls back to the default too). */
static float aoEnvFloat(const char* name, float def) {
    const char* env = getenv(name);
    return (env && *env) ? (float)atof(env) : def;
}

/* CACAO (FidelityFX) state.  Own FfxInterface + scratch buffer (same
 * pattern as the FSR pass); the CACAO context's internal GPU resources
 * are freed on swapchain recreation / pass removal. */
static void* cacaoScratchBuffer;
static size_t cacaoScratchBufferSize;
static FfxInterface cacaoBackendInterface;
static char cacaoBackendReady;
static FfxCacaoContext cacaoContext;
static char cacaoContextReady;
static VulkanImage cacaoOutput;
static u32 cacaoWidth;
static u32 cacaoHeight;
static VulkanProfile cacaoProfile;
static char cacaoProfileReady;

/* Temporal accumulation of the CACAO output (see ao_temporal.comp).  The
 * color TAA pass cannot average CACAO's spatially correlated noise (its
 * 3×3 clamp tracks the current frame's mode for block-scale pops), so the
 * AO gets its own history with a wide scalar clamp.  ENGINE_AO_TEMPORAL=0
 * disables it (output falls back to the raw CACAO buffer). */
static VulkanPipe temporalPipe;
static char temporalPipeReady;
static char temporalEnabled = 1;
static VulkanImage temporalA;
static VulkanImage temporalB;
static u32 temporalWidth;
static u32 temporalHeight;
static u32 temporalFrame;
static VulkanImage* temporalOutput;
static char aoWasDisabled;

VulkanAOPass vulkanAOPass;

VulkanAOPass::VulkanAOPass() : System("ao") {}

void VulkanAOPass::added() {
    const char* env = getenv("ENGINE_AO_DISABLED");
    if (env && *env && atoi(env)) aoDisabled = 1;
    env = getenv("ENGINE_AO_TEMPORAL");
    if (env && *env && !atoi(env)) temporalEnabled = 0;

    utils::signalSubscribe("swapchainCreated", swapchainCreated);

    cacaoProfile      = vulkanCreateProfile("cacao_ao");
    cacaoProfileReady = 1;

    temporalPipe      = vulkanCreatePipe(.name = "ao_temporal",
                                         .comp  = "shaders/pass/ao/spv/ao_temporal.comp.spv");
    temporalPipeReady = 1;
}

void VulkanAOPass::preUpdate() {
    if (vulkan.skipFrame) {
        return;
    }
    if (cacaoProfileReady) {
        vulkanResetProfile(vulkan.currentCmd, &cacaoProfile, 0);
    }
}

static void swapchainCreated(void* _) {
    (void)_;
    cacaoDestroyContext();
    cacaoDestroyOutput();
    temporalDestroyAccumulators();
}

static void cacaoDestroyContext(void) {
    if (cacaoContextReady) {
        ffxCacaoContextDestroy(&cacaoContext);
        cacaoContext      = FfxCacaoContext{};
        cacaoContextReady = 0;
    }
}

static void cacaoDestroyOutput(void) {
    if (cacaoOutput.img) {
        vulkanDestroyImage(&cacaoOutput, NULL);
        cacaoOutput = VulkanImage{};
    }
    cacaoWidth  = 0;
    cacaoHeight = 0;
}

static char cacaoEnsureContext(u32 width, u32 height) {
    if (!cacaoBackendReady) {
        cacaoScratchBufferSize = ffxGetScratchMemorySizeVK(vulkan.physicalDevice, 1);
        cacaoScratchBuffer     = calloc(1, cacaoScratchBufferSize);
        if (!cacaoScratchBuffer) {
            utils::error("vulkanAOPass: failed to allocate %zu bytes of CACAO backend scratch memory",
                         cacaoScratchBufferSize);
            return 0;
        }

        VkDeviceContext deviceContext = {
            .vkDevice         = vulkan.device,
            .vkPhysicalDevice = vulkan.physicalDevice,
            .vkDeviceProcAddr = vkGetDeviceProcAddr,
        };

        FfxDevice device = ffxGetDeviceVK(&deviceContext);
        FfxErrorCode backendResult =
            ffxGetInterfaceVK(&cacaoBackendInterface, device, cacaoScratchBuffer, cacaoScratchBufferSize, 1);
        if (backendResult != FFX_OK) {
            utils::error("vulkanAOPass: ffxGetInterfaceVK failed: %d", backendResult);
            free(cacaoScratchBuffer);
            cacaoScratchBuffer     = NULL;
            cacaoScratchBufferSize = 0;
            return 0;
        }

        cacaoBackendReady = 1;
    }

    if (cacaoContextReady && cacaoOutput.img && cacaoWidth == width && cacaoHeight == height) {
        return 1;
    }

    cacaoDestroyContext();
    cacaoDestroyOutput();

    /* CACAO writes rgba16f: .r = final AO (1 = unoccluded). */
    cacaoOutput = vulkanCreateImage(.name   = "CacaoOutput",
                                    .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                    .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                    .width  = (int)width,
                                    .height = (int)height);
    if (!cacaoOutput.img) {
        return 0;
    }

    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanTransition(cmd, &cacaoOutput, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    vulkanTransientEnd(cmd, 1);

    FfxCacaoContextDescription desc = {};
    desc.width              = width;
    desc.height             = height;
    desc.useDownsampledSsao = true; /* cheaper; bilateral 5x5 upscale reconstructs full-res AO */
    desc.backendInterface   = cacaoBackendInterface;

    FfxErrorCode createResult = ffxCacaoContextCreate(&cacaoContext, &desc);
    if (createResult != FFX_OK) {
        utils::error("vulkanAOPass: ffxCacaoContextCreate failed: %d", createResult);
        cacaoDestroyOutput();
        return 0;
    }

    cacaoContextReady = 1;
    cacaoWidth        = width;
    cacaoHeight       = height;
    utils::info("vulkanAOPass: created CACAO context for %ux%u", width, height);
    return 1;
}

static VkImageCreateInfo makeImageCreateInfo(VulkanImage* image) {
    VkImageCreateInfo info = {};
    info.sType             = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType         = VK_IMAGE_TYPE_2D;
    info.format            = image->format;
    info.extent            = image->extent;
    info.mipLevels         = (u32)image->mipLevels;
    info.arrayLayers       = (u32)image->layers;
    info.samples           = image->samples;
    info.tiling            = VK_IMAGE_TILING_OPTIMAL;
    info.usage             = image->usage;
    info.sharingMode       = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    return info;
}

static FfxResource wrapImageResource(VulkanImage* image,
                                     FfxResourceUsage usage,
                                     FfxResourceStates state,
                                     const wchar_t* name) {
    VkImageCreateInfo createInfo = makeImageCreateInfo(image);
    FfxResourceDescription desc  = ffxGetImageResourceDescriptionVK(image->img, createInfo, usage);
    return ffxGetResourceVK(image->img, desc, name, state);
}

static void cacaoUpdate(VulkanCommand* cmd, VulkanImage* depth, VulkanImage* normals, Camera* camera) {
    if (!cacaoEnsureContext(depth->extent.width, depth->extent.height)) {
        return;
    }

    /* Depth/normals were left SHADER_READ_ONLY by the SSR pass; stage them
     * for the compute reads. */
    vulkanTransition(cmd, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, normals, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, &cacaoOutput, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

    FfxCacaoSettings settings = FFX_CACAO_DEFAULT_SETTINGS;
    /* The engine's normal buffer is oct-encoded (.rg), which CACAO's affine
     * unpack (mul/add) cannot decode — reconstruct normals from depth. */
    settings.generateNormals = true;
    /* Tuned down from the CACAO defaults (radius 1.2, multiplier 1.0, power
     * 1.5, clamp 0.98): obscurance = multiplier * 4.3, then occlusion =
     * pow(1 - min(obscurance, clamp), power) — so at the defaults even weak
     * occlusion multiplies the image by ~0.7 and creases bottom out near
     * black, reading as an over-dark, too-wide contact band.  The smaller
     * radius keeps it a believable contact shadow; the linear power and the
     * clamped floor keep creases from going pitch black. */
    settings.radius           = aoEnvFloat("ENGINE_AO_RADIUS", 0.6f);
    settings.shadowMultiplier = aoEnvFloat("ENGINE_AO_STRENGTH", 0.8f);
    settings.shadowPower      = aoEnvFloat("ENGINE_AO_POWER", 1.0f);
    settings.shadowClamp      = aoEnvFloat("ENGINE_AO_CLAMP", 0.75f);
    /* Debug knobs: ENGINE_AO_ANGLE_OFF / ENGINE_AO_DETAIL / ENGINE_AO_QUALITY
     * (see the header comment for the full list). */
    static const char* angleOffEnv = getenv("ENGINE_AO_ANGLE_OFF");
    static const char* detailEnv   = getenv("ENGINE_AO_DETAIL");
    static const float detailOverride =
        (detailEnv && *detailEnv) ? (float)atof(detailEnv) : -1.0f;
    if (detailOverride >= 0.0f) {
        settings.detailShadowStrength = detailOverride;
    }
    static const char* qualityEnv = getenv("ENGINE_AO_QUALITY");
    if (qualityEnv && *qualityEnv) {
        /* FFX_CACAO_QUALITY_HIGHEST is adaptive (load-counter driven sample
         * counts, temporally nondeterministic); HIGH is fixed-tap. */
        settings.qualityLevel = (FfxCacaoQuality)atoi(qualityEnv);
    }
    /* Rotate/scale the sampling kernel per frame (AMD's TAA recommendation)
     * so the spatial kernel doesn't alias against the jitter sequence. */
    if (!(angleOffEnv && *angleOffEnv && atoi(angleOffEnv))) {
        const u32 phase = camera->frameIndex % 3;
        settings.temporalSupersamplingAngleOffset  = (float)phase / 3.0f * 3.14159265f;
        settings.temporalSupersamplingRadiusOffset = 1.0f + (((float)phase - 1.0f) / 3.0f) * 0.1f;
    }
    ffxCacaoUpdateSettings(&cacaoContext, &settings, true);

    /* CACAO derives the depth linearization from the projection matrix
     * (proj[10]/proj[11]); the depth buffer holds standard [0,1] depth. */
    FfxFloat32x4x4 proj          = {};
    FfxFloat32x4x4 normalsToView = {};
    memcpy(proj, camera->cameraUbo.projection, sizeof(proj));
    memcpy(normalsToView, camera->cameraUbo.view, sizeof(normalsToView));

    FfxCacaoDispatchDescription dispatch = {};
    dispatch.commandList  = ffxGetCommandListVK(cmd->cmd);
    dispatch.depthBuffer  = wrapImageResource(depth,
                                              FFX_RESOURCE_USAGE_READ_ONLY,
                                              FFX_RESOURCE_STATE_COMPUTE_READ,
                                              L"cacao_depth");
    dispatch.normalBuffer = wrapImageResource(normals,
                                              FFX_RESOURCE_USAGE_READ_ONLY,
                                              FFX_RESOURCE_STATE_COMPUTE_READ,
                                              L"cacao_normals");
    dispatch.outputBuffer = wrapImageResource(&cacaoOutput,
                                              FFX_RESOURCE_USAGE_UAV,
                                              FFX_RESOURCE_STATE_UNORDERED_ACCESS,
                                              L"cacao_output");
    dispatch.proj            = &proj;
    dispatch.normalsToView   = &normalsToView;
    dispatch.normalUnpackMul = 1.0f;
    dispatch.normalUnpackAdd = 0.0f;

    vulkanBeginProfile(cmd, &cacaoProfile, 0);
    FfxErrorCode result = ffxCacaoContextDispatch(&cacaoContext, &dispatch);
    vulkanEndProfile(cmd, &cacaoProfile, 0);

    if (result != FFX_OK) {
        utils::error("vulkanAOPass: ffxCacaoContextDispatch failed: %d", result);
        cacaoDestroyContext();
        cacaoDestroyOutput();
        return;
    }

    vulkanTransition(cmd, &cacaoOutput, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
}

/* ── Temporal AO accumulation ─────────────────────────────────────────── */

typedef struct AoTemporalPushConstants {
    u32 aoIndex;       /* current-frame CACAO output (sampled)        */
    u32 velocityIndex; /* motion vectors (sampled)                   */
    u32 depthIndex;    /* depth buffer (sampled)                     */
    u32 prevIndex;     /* previous accumulator (sampled)             */
    u32 outIndex;      /* current accumulator (storage)              */
    u32 width;
    u32 height;
    float blendWeight;    /* max temporal blend                     */
    float depthThreshold; /* relative inv-depth rejection threshold */
    float clampSlack;     /* scalar AABB expansion (fraction of range) */
    float clampFloor;     /* scalar AABB expansion (absolute floor)    */
} AoTemporalPushConstants;

static float temporalEnvOrDefault(const char* name, float def) {
    const char* env = getenv(name);
    return (env && *env) ? (float)atof(env) : def;
}

static void temporalDestroyAccumulators(void) {
    if (temporalA.img) {
        vulkanDestroyImage(&temporalA, NULL);
        temporalA = VulkanImage{};
    }
    if (temporalB.img) {
        vulkanDestroyImage(&temporalB, NULL);
        temporalB = VulkanImage{};
    }
    temporalWidth   = 0;
    temporalHeight  = 0;
    temporalFrame   = 0;
    temporalOutput  = NULL;
}

static char temporalEnsureAccumulators(u32 width, u32 height) {
    if (temporalA.img && temporalB.img && temporalWidth == width && temporalHeight == height) {
        return 1;
    }

    temporalDestroyAccumulators();

    /* .r = accumulated AO, .g = inverse view depth for the next frame's
     * disocclusion test.  Cleared to 0 = "no history": the first frame's
     * depth test rejects it and starts accumulation cleanly. */
    temporalA = vulkanCreateImage(.name   = "AoTemporalA",
                                  .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                  .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                  .width  = (int)width,
                                  .height = (int)height);
    temporalB = vulkanCreateImage(.name   = "AoTemporalB",
                                  .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                  .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                  .width  = (int)width,
                                  .height = (int)height);
    if (!temporalA.img || !temporalB.img) {
        temporalDestroyAccumulators();
        return 0;
    }
    temporalWidth  = width;
    temporalHeight = height;
    temporalFrame  = 0;

    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanTransition(cmd, &temporalA, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    vulkanTransition(cmd, &temporalB, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    VkClearColorValue black = {};
    vulkanClearColorImage(cmd, &temporalA, black);
    vulkanClearColorImage(cmd, &temporalB, black);
    vulkanTransientEnd(cmd, 1);
    return 1;
}

static void temporalDispatch(VulkanCommand* cmd, VulkanImage* depth, VulkanImage* velocity) {
    if (!temporalPipeReady || !temporalEnsureAccumulators(cacaoWidth, cacaoHeight)) {
        return;
    }

    VulkanImage* prev = (temporalFrame % 2) ? &temporalA : &temporalB;
    VulkanImage* out  = (temporalFrame % 2) ? &temporalB : &temporalA;

    /* cacaoUpdate left depth + cacaoOutput in SHADER_READ_ONLY. */
    vulkanTransition(cmd, velocity, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, prev, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, out, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

    AoTemporalPushConstants pc = {
        .aoIndex       = (u32)cacaoOutput.sampledPoolIndex,
        .velocityIndex = (u32)velocity->sampledPoolIndex,
        .depthIndex    = (u32)depth->sampledPoolIndex,
        .prevIndex     = (u32)prev->sampledPoolIndex,
        .outIndex      = (u32)out->storagePoolIndex,
        .width         = cacaoWidth,
        .height        = cacaoHeight,
        .blendWeight   = temporalEnvOrDefault("ENGINE_AO_TWEIGHT", 0.92f),
        .depthThreshold = temporalEnvOrDefault("ENGINE_AO_TDEPTH", 0.05f),
        .clampSlack    = temporalEnvOrDefault("ENGINE_AO_TCLAMP", 0.35f),
        .clampFloor    = temporalEnvOrDefault("ENGINE_AO_TFLOOR", 0.15f),
    };

    vulkanBindPipe(cmd, &temporalPipe);
    vulkanPush(cmd, &temporalPipe, sizeof(pc), &pc);

    u32 groupsX = (cacaoWidth + 7) / 8;
    u32 groupsY = (cacaoHeight + 7) / 8;
    vulkanDispatch(cmd, &temporalPipe, groupsX, groupsY, 1);

    vulkanTransition(cmd, out, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    temporalOutput = out;
    temporalFrame++;
}

void VulkanAOPass::update() {
    elapsedCPU = utils::nanos();

    if (vulkan.skipFrame) {
        elapsedCPU = utils::nanos() - elapsedCPU;
        return;
    }

    VulkanCommand* cmd     = vulkan.currentCmd;
    VulkanImage* depth     = vulkanFrameResourcesGetDepth();
    VulkanImage* normals   = vulkanFrameResourcesGetNormals();
    if (!depth || !normals) {
        elapsedCPU = utils::nanos() - elapsedCPU;
        return;
    }

    if (aoWasDisabled && !aoDisabled) {
        /* Re-enabled after a disable: history is stale — reset it BEFORE
         * this frame's accumulation runs. */
        temporalDestroyAccumulators();
    }
    aoWasDisabled = aoDisabled;

    if (!aoDisabled) {
        Entity* camEntity = cameraGetEntity();
        Camera* camera    = getComponent(camEntity->scene, Camera, camEntity->id);
        if (camera) {
            cacaoUpdate(cmd, depth, normals, camera);
        }
        /* Accumulate the AO so CACAO's spatially correlated noise (half-res
         * block pops, jitter-shifted pattern) is averaged before the
         * composite multiply — the color TAA pass alone passes it through
         * (see ao_temporal.comp). */
        if (temporalEnabled && cacaoContextReady && cacaoOutput.img) {
            VulkanImage* velocity = vulkanFrameResourcesGetVelocity();
            if (velocity) {
                temporalDispatch(cmd, depth, velocity);
            }
        }
    }
    /* While disabled the composite skips the AO multiply entirely
     * (absent-sentinel index), so no output needs producing. */

    elapsedGPU = cacaoProfileReady ? cacaoProfile.elapsed : 0.0;
    elapsedCPU = utils::nanos() - elapsedCPU;
}

void VulkanAOPass::postUpdate() {
    vulkanAOPass.cpuElapsed = elapsedCPU;
    vulkanAOPass.gpuElapsed = elapsedGPU;
}

void VulkanAOPass::removed() {
    cacaoDestroyContext();
    cacaoDestroyOutput();
    temporalDestroyAccumulators();
    if (temporalPipeReady) {
        vulkanDestroyPipe(&temporalPipe);
        temporalPipe      = VulkanPipe{};
        temporalPipeReady = 0;
    }
    if (cacaoScratchBuffer) {
        free(cacaoScratchBuffer);
        cacaoScratchBuffer     = NULL;
        cacaoScratchBufferSize = 0;
    }
    cacaoBackendInterface = FfxInterface{};
    cacaoBackendReady     = 0;
    if (cacaoProfileReady) {
        vulkanDestroyProfile(&cacaoProfile);
        cacaoProfile      = VulkanProfile{};
        cacaoProfileReady = 0;
    }
}

void vulkanAOPassSetDisabled(char disabled) {
    aoDisabled = disabled;
    utils::info("AO: %s", aoDisabled ? "disabled" : "enabled");
}

char vulkanAOPassIsDisabled(void) {
    return aoDisabled;
}

VulkanImage* vulkanAOPassGetOutput(void) {
    /* Temporally accumulated AO when available (.r = AO); the raw CACAO
     * buffer when the temporal filter is disabled.  NULL until the
     * context exists (i.e. before the first enabled frame after swapchain
     * creation). */
    if (temporalEnabled && temporalOutput) {
        return temporalOutput;
    }
    return cacaoOutput.img ? &cacaoOutput : NULL;
}
}  // namespace engine