#include "VulkanFsrPass.h"
#include "VulkanFsrUtils.h"
#include "ecs/Ecs.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/window/WindowSystem.h"
#include "events/Events.h"
#include "renderer/Renderer.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/pipeline/VulkanProfile.h"
#include "renderer/vulkan/pass/dof/VulkanDofPass.h"
#include "timer/Timer.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#pragma GCC diagnostic pop
#include <stdlib.h>

namespace engine {
static void swapchainCreated(void* _);
static void destroyOutput(void);
static void destroyContext(void);
static char ensureContext(u32 renderWidth, u32 renderHeight, u32 displayWidth, u32 displayHeight);
static FfxFsr3UpscalerQualityMode rendererUpscalerModeToFfxQuality(RendererUpscalerMode mode);
static VkImageCreateInfo makeImageCreateInfo(VulkanImage* image);
static FfxResource wrapImageResource(VulkanImage* image,
                                     FfxResourceUsage usage,
                                     FfxResourceStates state,
                                     const wchar_t* name);
static void generateReactiveMask(VulkanCommand* cmd,
                                 VulkanImage* sceneColor,
                                 VulkanImage* compositeColor,
                                 VulkanImage* material,
                                 VulkanImage* depth,
                                 VulkanImage* normals);

static double elapsedCPU;
static double elapsedGPU;
static VulkanImage outputImage;
static VulkanImage dilatedDepthImage;
static VulkanImage dilatedMotionVectorsImage;
static VulkanImage reconstructedPrevNearestDepthImage;
static VulkanImage reactiveMaskImage;
static VulkanImage tcMaskImage;
static u32 outputWidth;
static u32 outputHeight;
static RendererUpscalerMode contextMode = RENDERER_UPSCALER_OFF;

static void* scratchBuffer;
static size_t scratchBufferSize;
static FfxInterface backendInterface;
static char backendReady;
static FfxFsr3UpscalerContext context;
static char contextReady;
static char contextJustCreated;  // set on context creation, triggers dispatch.reset
static VulkanProfile profile;
static char profileReady;
static VulkanPipe reactivePipe;
static char reactivePipeReady;
static VulkanPipe skyVelocityPipe;
static char skyVelocityPipeReady;
static VulkanPipe reflVelocityPipe;
static char reflVelocityPipeReady;
static char reactiveMaskEnabled = 1;

typedef struct ReactivePushConstants {
    u32 opaqueColorIndex;
    u32 compositeColorIndex;
    u32 reactiveMaskIndex;
    u32 width;
    u32 height;
    u32 materialIndex;
    u32 depthIndex;
    u32 normalsIndex;
    u32 tcMaskIndex;
} ReactivePushConstants;

typedef struct SkyVelocityPushConstants {
    u32 depthIndex;
    u32 velocityIndex;
    u32 width;
    u32 height;
} SkyVelocityPushConstants;

typedef struct ReflVelocityPushConstants {
    u32 depthIndex;
    u32 normalsIndex;
    u32 velocityIndex;
    u32 materialIndex;
    u32 width;
    u32 height;
} ReflVelocityPushConstants;

VulkanFsrPass vulkanFsrPass;

VulkanFsrPass::VulkanFsrPass() : System("fsr") {}

void VulkanFsrPass::added() {
    utils::signalSubscribe("swapchainCreated", swapchainCreated);
    profile              = vulkanCreateProfile("fsr");
    profileReady         = 1;
    reactivePipe         = vulkanCreatePipe(.name = "fsr_reactive",
                                            .comp = "shaders/pass/fsr/spv/reactive.comp.spv");
    reactivePipeReady    = 1;
    skyVelocityPipe      = vulkanCreatePipe(.name = "fsr_sky_velocity",
                                            .comp = "shaders/pass/fsr/spv/sky_velocity.comp.spv");
    skyVelocityPipeReady = 1;
    reflVelocityPipe =
        vulkanCreatePipe(.name = "fsr_refl_velocity",
                         .comp = "shaders/pass/fsr/spv/reflection_velocity.comp.spv");
    reflVelocityPipeReady = 1;
}

void VulkanFsrPass::preUpdate() {
    if (profileReady) {
        vulkanResetProfile(vulkan.currentCmd, &profile, 0);
    }
}

static void swapchainCreated(void* _) {
    (void)_;
    destroyContext();
    destroyOutput();
}

static void destroyOutput(void) {
    if (outputImage.img) {
        vulkanDestroyImage(&outputImage, NULL);
        outputImage = VulkanImage{};
    }
    if (dilatedDepthImage.img) {
        vulkanDestroyImage(&dilatedDepthImage, NULL);
        dilatedDepthImage = VulkanImage{};
    }
    if (dilatedMotionVectorsImage.img) {
        vulkanDestroyImage(&dilatedMotionVectorsImage, NULL);
        dilatedMotionVectorsImage = VulkanImage{};
    }
    if (reconstructedPrevNearestDepthImage.img) {
        vulkanDestroyImage(&reconstructedPrevNearestDepthImage, NULL);
        reconstructedPrevNearestDepthImage = VulkanImage{};
    }
    if (reactiveMaskImage.img) {
        vulkanDestroyImage(&reactiveMaskImage, NULL);
        reactiveMaskImage = VulkanImage{};
    }
    if (tcMaskImage.img) {
        vulkanDestroyImage(&tcMaskImage, NULL);
        tcMaskImage = VulkanImage{};
    }
    outputWidth  = 0;
    outputHeight = 0;
}

static void destroyContext(void) {
    if (contextReady) {
        ffxFsr3UpscalerContextDestroy(&context);
        context      = FfxFsr3UpscalerContext{};
        contextReady = 0;
    }
}

static char ensureContext(u32 renderWidth, u32 renderHeight, u32 displayWidth, u32 displayHeight) {
    if (!backendReady) {
        scratchBufferSize = ffxGetScratchMemorySizeVK(vulkan.physicalDevice, 1);
        scratchBuffer     = calloc(1, scratchBufferSize);
        if (!scratchBuffer) {
            utils::error("vulkanFsrPass: failed to allocate %zu bytes of backend scratch memory",
                  scratchBufferSize);
            return 0;
        }

        VkDeviceContext deviceContext = {
            .vkDevice         = vulkan.device,
            .vkPhysicalDevice = vulkan.physicalDevice,
            .vkDeviceProcAddr = vkGetDeviceProcAddr,
        };

        FfxDevice device = ffxGetDeviceVK(&deviceContext);
        FfxErrorCode backendResult =
            ffxGetInterfaceVK(&backendInterface, device, scratchBuffer, scratchBufferSize, 1);
        if (backendResult != FFX_OK) {
            utils::error("vulkanFsrPass: ffxGetInterfaceVK failed: %d", backendResult);
            free(scratchBuffer);
            scratchBuffer     = NULL;
            scratchBufferSize = 0;
            return 0;
        }

        backendReady = 1;
    }

    RendererUpscalerMode mode = rendererGetUpscalerMode();
    if (contextReady && outputImage.img && outputWidth == displayWidth &&
        outputHeight == displayHeight && contextMode == mode) {
        return 1;
    }

    destroyContext();
    destroyOutput();

    outputImage =
        vulkanCreateImage(.name   = "FsrOutput",
                          .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                          .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          .width  = (int)displayWidth,
                          .height = (int)displayHeight);
    dilatedDepthImage =
        vulkanCreateImage(.name   = "FsrDilatedDepth",
                          .format = VK_FORMAT_R32_SFLOAT,
                          .usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          .width  = (int)renderWidth,
                          .height = (int)renderHeight);
    dilatedMotionVectorsImage =
        vulkanCreateImage(.name   = "FsrDilatedMotionVectors",
                          .format = VK_FORMAT_R16G16_SFLOAT,
                          .usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          .width  = (int)renderWidth,
                          .height = (int)renderHeight);
    reconstructedPrevNearestDepthImage =
        vulkanCreateImage(.name   = "FsrReconstructedPrevNearestDepth",
                          .format = VK_FORMAT_R32_UINT,
                          .usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          .width  = (int)renderWidth,
                          .height = (int)renderHeight);
    reactiveMaskImage =
        vulkanCreateImage(.name   = "FsrReactiveMask",
                          .format = VK_FORMAT_R32_SFLOAT,
                          .usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          .width  = (int)renderWidth,
                          .height = (int)renderHeight);
    tcMaskImage =
        vulkanCreateImage(.name   = "FsrTCMask",
                          .format = VK_FORMAT_R32_SFLOAT,
                          .usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          .width  = (int)renderWidth,
                          .height = (int)renderHeight);
    outputWidth  = displayWidth;
    outputHeight = displayHeight;

    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanTransition(cmd, &outputImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, &dilatedDepthImage, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    vulkanTransition(cmd, &dilatedMotionVectorsImage, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    vulkanTransition(cmd, &reconstructedPrevNearestDepthImage, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    vulkanTransition(cmd, &reactiveMaskImage, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    vulkanTransition(cmd, &tcMaskImage, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    vulkanTransientEnd(cmd, 1);

    FfxFsr3UpscalerContextDescription desc = {};
    desc.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE |
                 FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED | FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
    desc.maxRenderSize.width   = renderWidth;
    desc.maxRenderSize.height  = renderHeight;
    desc.maxUpscaleSize.width  = displayWidth;
    desc.maxUpscaleSize.height = displayHeight;
    desc.backendInterface      = backendInterface;

    FfxErrorCode createResult = ffxFsr3UpscalerContextCreate(&context, &desc);
    if (createResult != FFX_OK) {
        if ((u32)createResult == FFX_ERROR_OUT_OF_MEMORY ||
            (u32)createResult == FFX_ERROR_INSUFFICIENT_MEMORY) {
            utils::terminate(
                "vulkanFsrPass: not enough GPU memory to create FSR context. "
                "Free VRAM by closing other GPU applications and try again.");
        }
        utils::error("vulkanFsrPass: ffxFsr3UpscalerContextCreate failed: %d", createResult);
        destroyOutput();
        return 0;
    }

    contextReady       = 1;
    contextJustCreated = 1;
    contextMode        = mode;

    /* FSR3 temporal accumulation constants are left at defaults.
     * The reactive mask shader + reflection velocity pass handle
     * ghosting for composited content while allowing convergence.
     * If ghosting reappears during fast camera motion, prefer fixing
     * motion vectors or the reactive mask rather than scaling these
     * constants aggressively — high values cause shimmer. */

    utils::info("vulkanFsrPass: created context for %ux%u -> %ux%u mode %d",
         renderWidth,
         renderHeight,
         displayWidth,
         displayHeight,
         mode);
    return 1;
}

static FfxFsr3UpscalerQualityMode rendererUpscalerModeToFfxQuality(RendererUpscalerMode mode) {
    switch (mode) {
        case RENDERER_UPSCALER_NATIVE_AA:
            return FFX_FSR3UPSCALER_QUALITY_MODE_NATIVEAA;
        case RENDERER_UPSCALER_QUALITY:
            return FFX_FSR3UPSCALER_QUALITY_MODE_QUALITY;
        case RENDERER_UPSCALER_BALANCED:
            return FFX_FSR3UPSCALER_QUALITY_MODE_BALANCED;
        case RENDERER_UPSCALER_PERFORMANCE:
            return FFX_FSR3UPSCALER_QUALITY_MODE_PERFORMANCE;
        case RENDERER_UPSCALER_ULTRA_PERFORMANCE:
            return FFX_FSR3UPSCALER_QUALITY_MODE_ULTRA_PERFORMANCE;
        case RENDERER_UPSCALER_OFF:
        case RENDERER_UPSCALER_COUNT:
        default:
            return FFX_FSR3UPSCALER_QUALITY_MODE_NATIVEAA;
    }
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

static void generateReactiveMask(VulkanCommand* cmd,
                                 VulkanImage* sceneColor,
                                 VulkanImage* compositeColor,
                                 VulkanImage* material,
                                 VulkanImage* depth,
                                 VulkanImage* normals) {
    if (!reactivePipeReady || !reactiveMaskImage.img || !material || !depth || !normals) {
        return;
    }

    vulkanBindPipe(cmd, &reactivePipe);

    ReactivePushConstants pc = {
        .opaqueColorIndex    = sceneColor ? (u32)sceneColor->sampledPoolIndex : (u32)compositeColor->sampledPoolIndex,
        .compositeColorIndex = (u32)compositeColor->sampledPoolIndex,
        .reactiveMaskIndex   = (u32)reactiveMaskImage.storagePoolIndex,
        .width               = reactiveMaskImage.extent.width,
        .height              = reactiveMaskImage.extent.height,
        .materialIndex       = (u32)material->sampledPoolIndex,
        .depthIndex          = (u32)depth->sampledPoolIndex,
        .normalsIndex        = (u32)normals->sampledPoolIndex,
        .tcMaskIndex         = (u32)tcMaskImage.storagePoolIndex,
    };
    vulkanPush(cmd, &reactivePipe, sizeof(pc), &pc);

    u32 groupsX = (reactiveMaskImage.extent.width + 7) / 8;
    u32 groupsY = (reactiveMaskImage.extent.height + 7) / 8;
    vulkanDispatch(cmd, &reactivePipe, groupsX, groupsY, 1);
}

static void fillSkyVelocity(VulkanCommand* cmd, VulkanImage* depth, VulkanImage* velocity) {
    if (!skyVelocityPipeReady || !velocity || !depth) {
        return;
    }

    vulkanBindPipe(cmd, &skyVelocityPipe);

    SkyVelocityPushConstants pc = {
        .depthIndex    = (u32)depth->sampledPoolIndex,
        .velocityIndex = (u32)velocity->storagePoolIndex,
        .width         = velocity->extent.width,
        .height        = velocity->extent.height,
    };
    vulkanPush(cmd, &skyVelocityPipe, sizeof(pc), &pc);

    u32 groupsX = (velocity->extent.width + 7) / 8;
    u32 groupsY = (velocity->extent.height + 7) / 8;
    vulkanDispatch(cmd, &skyVelocityPipe, groupsX, groupsY, 1);
}

void VulkanFsrPass::update() {
    if (vulkan.skipFrame || !rendererIsUpscalerEnabled()) {
        return;
    }

    VulkanImage* sceneColor = vulkanFrameResourcesGetSceneColor();
    VulkanImage* compositeColor = vulkanFrameResourcesGetCompositeColor();
    /* DOF (when active) consumed the composite/TAA color and produced the
     * blurred HDR image — feed it to the upscaler so RCAS sharpens
     * in-focus detail, not bokeh. */
    VulkanImage* dofColor = vulkanDofPassGetOutput();
    VulkanImage* color    = dofColor ? dofColor : (compositeColor ? compositeColor : sceneColor);
    VulkanImage* depth       = vulkanFrameResourcesGetDepth();
    VulkanImage* velocity    = vulkanFrameResourcesGetVelocity();
    VulkanImage* material    = vulkanFrameResourcesGetMaterial();
    VulkanImage* normals     = vulkanFrameResourcesGetNormals();
    if (!color || !sceneColor || !depth || !velocity || !material || !normals ||
        window.renderWidth <= 0 || window.renderHeight <= 0 || window.width <= 0 ||
        window.height <= 0) {
        return;
    }

    if (!ensureContext(color->extent.width,
                       color->extent.height,
                       (u32)window.width,
                       (u32)window.height)) {
        return;
    }

    Entity* camEntity = cameraGetEntity();
    Camera* camera    = getComponent(camEntity->scene, Camera, camEntity->id);
    if (!camera) {
        return;
    }

    float jitterX = 0.0f;
    float jitterY = 0.0f;
    int32_t phaseCount =
        ffxFsr3UpscalerGetJitterPhaseCount((uint32_t)window.renderWidth, (uint32_t)window.width);
    if (phaseCount > 0) {
        ffxFsr3UpscalerGetJitterOffset(&jitterX,
                                       &jitterY,
                                       (int32_t)((camera->frameIndex - 1) % (u32)phaseCount),
                                       phaseCount);
    }

    VulkanCommand* cmd = vulkan.currentCmd;
    vulkanTransition(cmd, color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, sceneColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, material, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, normals, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, &outputImage, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    vulkanTransition(cmd, &dilatedDepthImage, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    vulkanTransition(cmd, &dilatedMotionVectorsImage, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    vulkanTransition(cmd, &reconstructedPrevNearestDepthImage, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    vulkanTransition(cmd, &reactiveMaskImage, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    vulkanTransition(cmd, &tcMaskImage, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

    /* Fill background motion vectors for sky pixels */
    vulkanTransition(cmd, velocity, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    fillSkyVelocity(cmd, depth, velocity);

    /* Overwrite floor reflection MVs */
    if (reflVelocityPipeReady) {
        vulkanBindPipe(cmd, &reflVelocityPipe);
        ReflVelocityPushConstants rpc = {
            .depthIndex    = (u32)depth->sampledPoolIndex,
            .normalsIndex  = (u32)normals->sampledPoolIndex,
            .velocityIndex = (u32)velocity->storagePoolIndex,
            .materialIndex = (u32)material->sampledPoolIndex,
            .width         = velocity->extent.width,
            .height        = velocity->extent.height,
        };
        vulkanPush(cmd, &reflVelocityPipe, sizeof(rpc), &rpc);
        u32 gX = (velocity->extent.width + 7) / 8;
        u32 gY = (velocity->extent.height + 7) / 8;
        vulkanDispatch(cmd, &reflVelocityPipe, gX, gY, 1);
    }

    vulkanTransition(cmd, velocity, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    /* Generate reactive mask when upscaler is active.
     * The alpha-cutout edge detection must always run (it's independent
     * of compositing).  Only skip the composite-difference portion
     * when scene and composite are identical (no SSR/AO active). */
    VulkanImage* opaqueColor = (color != sceneColor) ? sceneColor : NULL;
    if (reactiveMaskEnabled) {
        generateReactiveMask(cmd, opaqueColor, color, material, depth, normals);
        /* DOF-blurred pixels are marked reactive so the upscaler does not
         * accumulate detail the blur will destroy (max-blend into the mask
         * the dispatch above just wrote). */
        vulkanDofPassApplyReactiveMask(cmd, depth);
    }

    vulkanTransition(cmd, &reactiveMaskImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, &tcMaskImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    /* Main FSR dispatch */
    FfxFsr3UpscalerDispatchDescription dispatch = {};
    dispatch.commandList                        = ffxGetCommandListVK(cmd->cmd);
    dispatch.color                              = wrapImageResource(color,
                                                                    FFX_RESOURCE_USAGE_READ_ONLY,
                                                                    FFX_RESOURCE_STATE_COMPUTE_READ,
                                                                    L"fsr_color");
    dispatch.depth                              = wrapImageResource(depth,
                                                                    FFX_RESOURCE_USAGE_READ_ONLY,
                                                                    FFX_RESOURCE_STATE_COMPUTE_READ,
                                                                    L"fsr_depth");
    dispatch.motionVectors                      = wrapImageResource(velocity,
                                                                    FFX_RESOURCE_USAGE_READ_ONLY,
                                                                    FFX_RESOURCE_STATE_COMPUTE_READ,
                                                                    L"fsr_velocity");
    if (reactiveMaskEnabled) {
        dispatch.reactive                   = wrapImageResource(&reactiveMaskImage,
                                                                FFX_RESOURCE_USAGE_READ_ONLY,
                                                                FFX_RESOURCE_STATE_COMPUTE_READ,
                                                                L"fsr_reactive");
        dispatch.transparencyAndComposition = wrapImageResource(&tcMaskImage,
                                                                FFX_RESOURCE_USAGE_READ_ONLY,
                                                                FFX_RESOURCE_STATE_COMPUTE_READ,
                                                                L"fsr_tc");
    }
    dispatch.dilatedMotionVectors = wrapImageResource(&dilatedMotionVectorsImage,
                                                      FFX_RESOURCE_USAGE_UAV,
                                                      FFX_RESOURCE_STATE_UNORDERED_ACCESS,
                                                      L"fsr_dilated_motion_vectors");
    dispatch.dilatedDepth         = wrapImageResource(&dilatedDepthImage,
                                                      FFX_RESOURCE_USAGE_UAV,
                                                      FFX_RESOURCE_STATE_UNORDERED_ACCESS,
                                                      L"fsr_dilated_depth");
    dispatch.reconstructedPrevNearestDepth =
        wrapImageResource(&reconstructedPrevNearestDepthImage,
                          FFX_RESOURCE_USAGE_UAV,
                          FFX_RESOURCE_STATE_UNORDERED_ACCESS,
                          L"fsr_reconstructed_prev_nearest_depth");
    dispatch.output              = wrapImageResource(&outputImage,
                                                     FFX_RESOURCE_USAGE_UAV,
                                                     FFX_RESOURCE_STATE_UNORDERED_ACCESS,
                                                     L"fsr_output");
    dispatch.jitterOffset.x      = jitterX;
    dispatch.jitterOffset.y      = jitterY;
    dispatch.motionVectorScale.x = -1.0f;
    dispatch.motionVectorScale.y = -1.0f;
    dispatch.renderSize.width    = color->extent.width;
    dispatch.renderSize.height   = color->extent.height;
    dispatch.upscaleSize.width   = outputImage.extent.width;
    dispatch.upscaleSize.height  = outputImage.extent.height;
    /* RCAS (AMD's CAS kernel) runs inside the upscaler dispatch: the same
     * FSR3 context sharpens the upscaled image — no separate CAS context
     * and no engine-side sharpening pass. The settings-GUI strength slider
     * (aaCasStrength) feeds this directly. When the upscaler is off the
     * engine applies no sharpening at all. */
    float casStrength         = rendererGetCasStrength();
    dispatch.enableSharpening = casStrength > 0.0f;
    /* The SDK validates sharpness in [0,1] (1.0 = RCAS max). Strengths above
     * 1.0 are a final-pass-only extension and are clamped here to keep the
     * upscaler's own RCAS within AMD's reference range. */
    dispatch.sharpness        = casStrength > 1.0f ? 1.0f : casStrength;
    dispatch.frameTimeDelta      = (float)(utils::timer.frameTime / MILLION); /* ns → ms */
    dispatch.preExposure         = 1.0f;
    dispatch.reset               = contextJustCreated || (camera->frameIndex <= 2);
    if (contextJustCreated) {
        contextJustCreated = 0;
    }
    dispatch.cameraNear              = camera->znear;
    dispatch.cameraFar               = camera->zfar;
    dispatch.cameraFovAngleVertical  = camera->yfov;
    dispatch.viewSpaceToMetersFactor = 1.0f;

    vulkanBeginProfile(cmd, &profile, 0);
    FfxErrorCode result = ffxFsr3UpscalerContextDispatch(&context, &dispatch);
    vulkanEndProfile(cmd, &profile, 0);

    if (result != FFX_OK) {
        utils::error("vulkanFsrPass: ffxFsr3UpscalerContextDispatch failed: %d", result);
        destroyContext();
        destroyOutput();
        return;
    }

    vulkanTransition(cmd, &outputImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    elapsedGPU = profile.elapsed;
}

void VulkanFsrPass::postUpdate() {
    vulkanFsrPass.cpuElapsed = elapsedCPU;
    vulkanFsrPass.gpuElapsed = elapsedGPU;
    elapsedCPU               = utils::nanos();
    elapsedCPU               = utils::nanos() - elapsedCPU;
}

void VulkanFsrPass::removed() {
    destroyContext();
    destroyOutput();
    if (scratchBuffer) {
        free(scratchBuffer);
        scratchBuffer     = NULL;
        scratchBufferSize = 0;
    }
    backendInterface = FfxInterface{};
    backendReady     = 0;
    if (profileReady) {
        vulkanDestroyProfile(&profile);
        profile      = VulkanProfile{};
        profileReady = 0;
    }
    if (reactivePipeReady) {
        vulkanDestroyPipe(&reactivePipe);
        reactivePipe      = VulkanPipe{};
        reactivePipeReady = 0;
    }
    if (skyVelocityPipeReady) {
        vulkanDestroyPipe(&skyVelocityPipe);
        skyVelocityPipe      = VulkanPipe{};
        skyVelocityPipeReady = 0;
    }
    if (reflVelocityPipeReady) {
        vulkanDestroyPipe(&reflVelocityPipe);
        reflVelocityPipe      = VulkanPipe{};
        reflVelocityPipeReady = 0;
    }
}

VulkanImage* vulkanFsrPassGetOutput(void) {
    return outputImage.img ? &outputImage : NULL;
}

char vulkanFsrPassIsEnabled(void) {
    return rendererIsUpscalerEnabled() && outputImage.img;
}

char vulkanFsrGetRenderResolution(RendererUpscalerMode mode,
                                  u32 displayWidth,
                                  u32 displayHeight,
                                  u32* renderWidth,
                                  u32* renderHeight) {
    if (!renderWidth || !renderHeight || mode == RENDERER_UPSCALER_OFF) {
        return 0;
    }

    return ffxFsr3UpscalerGetRenderResolutionFromQualityMode(
               renderWidth,
               renderHeight,
               displayWidth,
               displayHeight,
               rendererUpscalerModeToFfxQuality(mode)) == FFX_OK;
}

int32_t vulkanFsrGetJitterPhaseCount(u32 renderWidth, u32 displayWidth) {
    return ffxFsr3UpscalerGetJitterPhaseCount(renderWidth, displayWidth);
}

void vulkanFsrGetJitterOffset(float* jitterX, float* jitterY, int32_t index, int32_t phaseCount) {
    ffxFsr3UpscalerGetJitterOffset(jitterX, jitterY, index, phaseCount);
}

void vulkanFsrPassSetReactiveMask(char enabled) {
    reactiveMaskEnabled = enabled;
}

char vulkanFsrPassGetReactiveMask(void) {
    return reactiveMaskEnabled;
}

VulkanImage* vulkanFsrPassGetReactiveMaskImage(void) {
    return reactiveMaskImage.img ? &reactiveMaskImage : NULL;
}
}  // namespace engine
