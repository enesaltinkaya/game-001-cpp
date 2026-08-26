#include "VulkanShadowDenoisePass.h"
#include "ecs/Ecs.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/transform/TransformComponent.h"
#include "ecs/system/window/WindowSystem.h"
#include "events/Events.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanProfile.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanBuffer.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/utils/VulkanUtils.h"
#include "renderer/vulkan/pass/shadow/VulkanShadowPass.h"
#include <stdlib.h>
#include <string.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <FidelityFX/host/ffx_classifier.h>
#include <FidelityFX/host/ffx_denoiser.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#pragma GCC diagnostic pop

namespace engine {
VulkanShadowDenoisePass vulkanShadowDenoisePass;

/* ── State ─────────────────────────────────────────────────────────── */
static char hsEnabled       = 0;
static char hsEnvChecked    = 0;

/* FFX backend (interface + scratch, shared by both contexts) */
static FfxInterface ffxInterface;
static void*        ffxScratch     = NULL;
static size_t       ffxScratchSize = 0;
static char         ffxBackendReady = 0;

/* FFX contexts (recreated on swapchainCreated / resolution change) */
static FfxClassifierContext classifierContext;
static FfxDenoiserContext   denoiserContext;
static char                 contextsReady = 0;
static u32                  contextsWidth  = 0;
static u32                  contextsHeight = 0;

/* GPU resources (recreated on swapchainCreated / resolution change) */
static VulkanImage  rayHitImage;       /* tile-res R32_UINT, UAV+SRV */
static VulkanBuffer workQueueBuf;      /* 4*uint32 per tile, UAV */
static VulkanBuffer workQueueCountBuf; /* 3*uint32, UAV */
static VulkanImage  shadowMaskImage;   /* full-res RGBA8, SAMPLED+STORAGE, in pool */
static VulkanImage  depthCopyImage;    /* full-res R32_SFLOAT, SAMPLED+STORAGE, in pool */
static VulkanPipe   depthCopyPipe;
static char         depthCopyPipeReady = 0;
/* The FFX VK backend always builds image views with VK_REMAINING_ARRAY_LAYERS,
 * so it cannot sample a layer of the CSM 2D-array shadow map directly.  Each
 * active cascade's depth is copied into a single-layer 2D image that the FFX
 * backend can wrap. */
static VulkanImage  cascadeDepthImages[2]; /* single-layer 2D D32, per cascade */
static int          cascadeDepthCount = 0;
/* The classifier's r_input_shadowMap is an array of 4 textures; the inactive
 * cascade slots (beyond the active count) still need a valid (non-null)
 * resource bound, so a 1x1 dummy depth image fills them. */
static VulkanImage  dummyCascadeDepth;

/* env params */
static float sunAngleDeg   = 1.0f;   /* sun angular diameter, degrees (plan: 0.5-1) */
static float blockerOffset = 0.00015f; /* receiver bias, zero-to-one depth units */
static float depthSigma    = 1.0f;
static char  dumpChecked   = 0;
static char  dumpEnabled   = 0;

/* frame counter for the denoiser's temporal filter */
static u32 frameIndex = 0;

/* profile */
static VulkanProfile hsProfile;
static char          hsProfileReady = 0;

/* tile geometry (FFX classifier processes 8x4 tiles) */
static const u32 HS_TILE_X = 8;
static const u32 HS_TILE_Y = 4;

/* ── Forward declarations ──────────────────────────────────────────── */
static void swapchainCreated(void* _);
static void hsDestroyResources(void);
static char hsEnsureContexts(u32 width, u32 height);
static void hsDispatch(VulkanCommand* cmd,
                       VulkanImage*   depth,
                       VulkanImage*   worldNormal,
                       VulkanImage*   velocity,
                       Camera*        camera,
                       Transform*     cameraTransform);
static VkImageCreateInfo makeImageCreateInfo(VulkanImage* image);
static FfxResource wrapImageResource(VulkanImage* image,
                                     FfxResourceUsage usage,
                                     FfxResourceStates state,
                                     const wchar_t* name);
static FfxResource wrapBufferResource(VulkanBuffer* buffer,
                                      u64           size,
                                      FfxResourceUsage usage,
                                      FfxResourceStates state,
                                      const wchar_t* name);

/* ── Resource management ──────────────────────────────────────────── */
static void hsDestroyResources(void) {
    if (contextsReady) {
        ffxClassifierContextDestroy(&classifierContext);
        ffxDenoiserContextDestroy(&denoiserContext);
        classifierContext = FfxClassifierContext{};
        denoiserContext   = FfxDenoiserContext{};
        contextsReady     = 0;
    }
    if (rayHitImage.img) {
        vulkanDestroyImage(&rayHitImage, NULL);
        rayHitImage = VulkanImage{};
    }
    if (workQueueBuf.buf) {
        vulkanDestroyBuffer(&workQueueBuf, NULL);
        workQueueBuf = VulkanBuffer{};
    }
    if (workQueueCountBuf.buf) {
        vulkanDestroyBuffer(&workQueueCountBuf, NULL);
        workQueueCountBuf = VulkanBuffer{};
    }
    if (shadowMaskImage.img) {
        if (shadowMaskImage.inPool) vulkanRemoveImageFromPool(&shadowMaskImage);
        vulkanDestroyImage(&shadowMaskImage, NULL);
        shadowMaskImage = VulkanImage{};
    }
    if (depthCopyImage.img) {
        if (depthCopyImage.inPool) vulkanRemoveImageFromPool(&depthCopyImage);
        vulkanDestroyImage(&depthCopyImage, NULL);
        depthCopyImage = VulkanImage{};
    }
    for (int i = 0; i < 2; i++) {
        if (cascadeDepthImages[i].img) {
            vulkanDestroyImage(&cascadeDepthImages[i], NULL);
            cascadeDepthImages[i] = VulkanImage{};
        }
    }
    if (dummyCascadeDepth.img) {
        vulkanDestroyImage(&dummyCascadeDepth, NULL);
        dummyCascadeDepth = VulkanImage{};
    }
    cascadeDepthCount = 0;
    contextsWidth  = 0;
    contextsHeight = 0;
}

static char hsEnsureContexts(u32 width, u32 height) {
    if (contextsReady && rayHitImage.img && shadowMaskImage.img &&
        workQueueBuf.buf && workQueueCountBuf.buf &&
        contextsWidth == width && contextsHeight == height) {
        return 1;
    }

    hsDestroyResources();

    /* Backend interface (created once, shared by both contexts). */
    if (!ffxBackendReady) {
        ffxScratchSize = ffxGetScratchMemorySizeVK(vulkan.physicalDevice, 2);
        ffxScratch     = calloc(1, ffxScratchSize);
        if (!ffxScratch) {
            utils::error("vulkanShadowDenoisePass: failed to allocate %zu bytes of FFX scratch",
                         ffxScratchSize);
            return 0;
        }
        VkDeviceContext deviceContext = {
            .vkDevice         = vulkan.device,
            .vkPhysicalDevice = vulkan.physicalDevice,
            .vkDeviceProcAddr = vkGetDeviceProcAddr,
        };
        FfxDevice    device        = ffxGetDeviceVK(&deviceContext);
        FfxErrorCode backendResult =
            ffxGetInterfaceVK(&ffxInterface, device, ffxScratch, ffxScratchSize, 2);
        if (backendResult != FFX_OK) {
            utils::error("vulkanShadowDenoisePass: ffxGetInterfaceVK failed: %d", backendResult);
            free(ffxScratch);
            ffxScratch     = NULL;
            ffxScratchSize = 0;
            return 0;
        }
        ffxBackendReady = 1;
    }

    const u32 xTiles    = (width + HS_TILE_X - 1) / HS_TILE_X;
    const u32 yTiles    = (height + HS_TILE_Y - 1) / HS_TILE_Y;
    const u32 tileCount = xTiles * yTiles;

    /* Tile-resolution ray-hit UAV (one R32 texel per 8x4 tile). */
    rayHitImage = vulkanCreateImage(.name   = "hs_rayhit",
                                    .format = VK_FORMAT_R32_UINT,
                                    .usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                    .width  = (int)xTiles,
                                    .height = (int)yTiles);
    if (!rayHitImage.img) return 0;

    /* Work queue (dead in the raster path — allocated but never dispatched). */
    workQueueBuf = vulkanCreateGpuBuffer("hs_workqueue", (u64)tileCount * 4 * sizeof(u32),
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!workQueueBuf.buf) {
        hsDestroyResources();
        return 0;
    }
    workQueueCountBuf = vulkanCreateGpuBuffer("hs_workqueue_count", 3 * sizeof(u32),
                                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!workQueueCountBuf.buf) {
        hsDestroyResources();
        return 0;
    }

    /* Full-resolution shadow mask (denoiser output; .r = lit fraction). */
    shadowMaskImage = vulkanCreateImage(.name   = "hs_shadowmask",
                                        .format = VK_FORMAT_R8G8B8A8_UNORM,
                                        .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                        .width  = (int)width,
                                        .height = (int)height);
    if (!shadowMaskImage.img) {
        hsDestroyResources();
        return 0;
    }
    vulkanAddImageToPool(&shadowMaskImage);

    /* R32 depth copy (the FFX denoiser's internal history copy rejects a
     * D32->R32 image copy; the hybridshadows sample solves this with a small
     * compute "copy depth" pass, which we mirror here). */
    depthCopyImage = vulkanCreateImage(.name   = "hs_depth_copy",
                                      .format = VK_FORMAT_R32_SFLOAT,
                                      .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                      .width  = (int)width,
                                      .height = (int)height);
    if (!depthCopyImage.img) {
        hsDestroyResources();
        return 0;
    }
    vulkanAddImageToPool(&depthCopyImage);

    if (!depthCopyPipeReady) {
        depthCopyPipe      = vulkanCreatePipe(.name = "hs_depth_copy",
                                              .comp  = "shaders/pass/shadow_denoise/spv/hs_depth_copy.comp.spv");
        depthCopyPipeReady = 1;
    }

    /* Single-layer 2D D32 images for the active cascades (the FFX backend
     * cannot sample a layer of the CSM 2D array directly). */
    ShadowCascadeData cascadeTmp;
    vulkanShadowPassGetCascadeData(&cascadeTmp);
    const int cascadeCount = cascadeTmp.cascadeCount;
    cascadeDepthCount      = cascadeCount > 2 ? 2 : cascadeCount;
    for (int i = 0; i < cascadeDepthCount; i++) {
        cascadeDepthImages[i] = vulkanCreateImage(.name   = "hs_cascade_depth",
                                                  .format = VK_FORMAT_D32_SFLOAT,
                                                  .usage  = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                            VK_IMAGE_USAGE_SAMPLED_BIT,
                                                  .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
                                                  .width  = 2048,
                                                  .height = 2048);
        if (!cascadeDepthImages[i].img) {
            hsDestroyResources();
            return 0;
        }
    }

    /* 1x1 dummy depth image for the inactive cascade slots. */
    dummyCascadeDepth = vulkanCreateImage(.name   = "hs_dummy_cascade_depth",
                                         .format = VK_FORMAT_D32_SFLOAT,
                                         .usage  = VK_IMAGE_USAGE_SAMPLED_BIT,
                                         .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
                                         .width  = 1,
                                         .height = 1);
    if (!dummyCascadeDepth.img) {
        hsDestroyResources();
        return 0;
    }

    /* Initialize the UAV images to GENERAL (and the depth images to a known
     * layout) so the FFX backend's first barrier is valid. */
    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanTransition(cmd, &rayHitImage, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    vulkanTransition(cmd, &shadowMaskImage, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    vulkanTransition(cmd, &depthCopyImage, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    vulkanTransition(cmd, &dummyCascadeDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    for (int i = 0; i < cascadeDepthCount; i++) {
        vulkanTransition(cmd, &cascadeDepthImages[i], VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    }
    vulkanTransientEnd(cmd, 1);

    /* FFX contexts. */
    FfxClassifierContextDescription cd = {};
    cd.flags = FFX_CLASSIFIER_SHADOW | FFX_CLASSIFIER_CLASSIFY_BY_CASCADES;
    /* NOT FFX_CLASSIFIER_ENABLE_DEPTH_INVERTED — the engine's depth is [0,1]. */
    cd.resolution.width  = width;
    cd.resolution.height = height;
    cd.backendInterface  = ffxInterface;
    FfxErrorCode cr = ffxClassifierContextCreate(&classifierContext, &cd);
    if (cr != FFX_OK) {
        utils::error("vulkanShadowDenoisePass: ffxClassifierContextCreate failed: %d", cr);
        hsDestroyResources();
        return 0;
    }

    FfxDenoiserContextDescription dd = {};
    dd.flags = FFX_DENOISER_SHADOWS;
    /* NOT FFX_DENOISER_ENABLE_DEPTH_INVERTED. */
    dd.windowSize.width  = width;
    dd.windowSize.height = height;
    dd.backendInterface  = ffxInterface;
    FfxErrorCode dr = ffxDenoiserContextCreate(&denoiserContext, &dd);
    if (dr != FFX_OK) {
        utils::error("vulkanShadowDenoisePass: ffxDenoiserContextCreate failed: %d", dr);
        ffxClassifierContextDestroy(&classifierContext);
        hsDestroyResources();
        return 0;
    }

    contextsReady  = 1;
    contextsWidth  = width;
    contextsHeight = height;
    frameIndex     = 0;
    utils::info("vulkanShadowDenoisePass: created FFX classifier+denoiser for %ux%u (%u tiles)",
                width, height, tileCount);
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

static FfxResource wrapBufferResource(VulkanBuffer* buffer,
                                      u64           size,
                                      FfxResourceUsage usage,
                                      FfxResourceStates state,
                                      const wchar_t* name) {
    VkBufferCreateInfo createInfo = {};
    createInfo.sType             = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    createInfo.size              = size;
    createInfo.usage             = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    createInfo.queueFamilyIndexCount = 0;
    createInfo.sharingMode       = VK_SHARING_MODE_EXCLUSIVE;
    FfxResourceDescription desc  = ffxGetBufferResourceDescriptionVK(buffer->buf, createInfo, usage);
    return ffxGetResourceVK(buffer->buf, desc, name, state);
}

/* ── Dispatch ─────────────────────────────────────────────────────── */
static void hsDispatch(VulkanCommand* cmd,
                       VulkanImage*   depth,
                       VulkanImage*   worldNormal,
                       VulkanImage*   velocity,
                       Camera*        camera,
                       Transform*     cameraTransform) {
    const u32 width  = depth->extent.width;
    const u32 height = depth->extent.height;
    if (!hsEnsureContexts(width, height)) return;

    const u32 xTiles    = (width + HS_TILE_X - 1) / HS_TILE_X;
    const u32 yTiles    = (height + HS_TILE_Y - 1) / HS_TILE_Y;
    const u32 tileCount = xTiles * yTiles;

    ShadowCascadeData cascade;
    vulkanShadowPassGetCascadeData(&cascade);
    if (cascade.cascadeCount < 1) return;

    /* The classifier + denoiser read the depth / normal / shadow maps in
     * SHADER_READ_ONLY; make sure they're staged for compute reads. */
    vulkanTransition(cmd, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, worldNormal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    if (velocity) vulkanTransition(cmd, velocity, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    /* Copy each active cascade's depth (a layer of the CSM 2D array) into a
     * single-layer 2D image the FFX backend can wrap.  vulkanTransition always
     * covers layer 0, so transition the specific cascade layer manually. */
    for (int i = 0; i < cascadeDepthCount; i++) {
        VulkanImage* layer = vulkanShadowPassGetShadowMapLayer(i);
        if (!layer) continue;
        /* shadow_csm layer i: SHADER_READ_ONLY -> TRANSFER_SRC */
        VkImageMemoryBarrier2 srcBar = {};
        srcBar.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        srcBar.oldLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        srcBar.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        srcBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        srcBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        srcBar.srcAccessMask    = VK_ACCESS_2_SHADER_READ_BIT;
        srcBar.dstAccessMask    = VK_ACCESS_2_TRANSFER_READ_BIT;
        srcBar.srcStageMask     = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        srcBar.dstStageMask     = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        srcBar.image            = layer->img;
        srcBar.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        srcBar.subresourceRange.baseMipLevel   = 0;
        srcBar.subresourceRange.levelCount     = 1;
        srcBar.subresourceRange.baseArrayLayer = (u32)i;
        srcBar.subresourceRange.layerCount     = 1;
        VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        (void)srcStage;
        (void)dstStage;
        VkDependencyInfo dep = {};
        dep.sType             = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers    = &srcBar;
        vkCmdPipelineBarrier2(cmd->cmd, &dep);

        vulkanTransition(cmd, &cascadeDepthImages[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);
        VkImageCopy region = {};
        region.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        region.srcSubresource.mipLevel       = 0;
        region.srcSubresource.baseArrayLayer = (u32)i;
        region.srcSubresource.layerCount     = 1;
        region.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        region.dstSubresource.mipLevel       = 0;
        region.dstSubresource.baseArrayLayer = 0;
        region.dstSubresource.layerCount     = 1;
        region.extent = {2048, 2048, 1};
        vkCmdCopyImage(cmd->cmd, layer->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       cascadeDepthImages[i].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        vulkanTransition(cmd, &cascadeDepthImages[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        /* Leave the CSM layer in SHADER_READ_ONLY for the fragment shaders. */
        VkImageMemoryBarrier2 dstBar = {};
        dstBar.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        dstBar.oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        dstBar.newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        dstBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dstBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dstBar.srcAccessMask    = VK_ACCESS_2_TRANSFER_READ_BIT;
        dstBar.dstAccessMask    = VK_ACCESS_2_SHADER_READ_BIT;
        dstBar.srcStageMask     = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        dstBar.dstStageMask     = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        dstBar.image            = layer->img;
        dstBar.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        dstBar.subresourceRange.baseMipLevel   = 0;
        dstBar.subresourceRange.levelCount     = 1;
        dstBar.subresourceRange.baseArrayLayer = (u32)i;
        dstBar.subresourceRange.layerCount     = 1;
        VkDependencyInfo dep2 = {};
        dep2.sType             = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep2.imageMemoryBarrierCount = 1;
        dep2.pImageMemoryBarriers    = &dstBar;
        vkCmdPipelineBarrier2(cmd->cmd, &dep2);
    }

    /* Reset the work-queue counter to {0,1,1} each frame (the classifier
     * atomicAdds into data[0]; the queue itself is never dispatched in the
     * raster path, but the counter must stay bounded to avoid OOB writes). */
    u32 wqc[3] = {0, 1, 1};
    vkCmdUpdateBuffer(cmd->cmd, workQueueCountBuf.buf, 0, sizeof(wqc), wqc);
    VkBufferMemoryBarrier bmb = {};
    bmb.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bmb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bmb.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    bmb.buffer        = workQueueCountBuf.buf;
    bmb.offset        = 0;
    bmb.size          = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd->cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0,
                         0, NULL,   /* memory barriers */
                         1, &bmb,   /* buffer memory barriers */
                         0, NULL);  /* image memory barriers */

    /* ── Classifier (shadow mode, classify-by-cascades) ──────────── */
    FfxClassifierShadowDispatchDescription cdesc = {};
    cdesc.commandList = ffxGetCommandListVK(cmd->cmd);
    cdesc.depth       = wrapImageResource(depth, FFX_RESOURCE_USAGE_READ_ONLY,
                                          FFX_RESOURCE_STATE_COMPUTE_READ, L"hs_depth");
    cdesc.normals     = wrapImageResource(worldNormal, FFX_RESOURCE_USAGE_READ_ONLY,
                                          FFX_RESOURCE_STATE_COMPUTE_READ, L"hs_normal");
    for (int i = 0; i < (int)FFX_CLASSIFIER_MAX_SHADOW_MAP_TEXTURES_COUNT; i++) {
        if (i < cascadeDepthCount) {
            cdesc.shadowMaps[i] = wrapImageResource(&cascadeDepthImages[i], FFX_RESOURCE_USAGE_READ_ONLY,
                                                    FFX_RESOURCE_STATE_COMPUTE_READ,
                                                    L"hs_shadowmap");
        } else {
            /* Inactive cascade slots still need a valid (non-null) resource. */
            cdesc.shadowMaps[i] = wrapImageResource(&dummyCascadeDepth, FFX_RESOURCE_USAGE_READ_ONLY,
                                                    FFX_RESOURCE_STATE_COMPUTE_READ,
                                                    L"hs_dummy_shadowmap");
        }
    }
    cdesc.workQueue      = wrapBufferResource(&workQueueBuf, (u64)tileCount * 4 * sizeof(u32),
                                              FFX_RESOURCE_USAGE_UAV,
                                              FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"hs_workqueue");
    cdesc.workQueueCount = wrapBufferResource(&workQueueCountBuf, 3 * sizeof(u32),
                                              FFX_RESOURCE_USAGE_UAV,
                                              FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"hs_workqueue_count");
    cdesc.rayHitTexture  = wrapImageResource(&rayHitImage, FFX_RESOURCE_USAGE_UAV,
                                             FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"hs_rayhit");

    /* World normal is already in [-1,1] world space. */
    cdesc.normalsUnPackMul = 1.0f;
    cdesc.normalsUnPackAdd = 0.0f;

    /* Light direction (toward the scene). */
    glm_vec3_copy(cascade.lightDir, cdesc.lightDir);
    cdesc.sunSizeLightSpace = sunAngleDeg * (3.14159265f / 180.0f);
    cdesc.tileCutOff        = 0;
    cdesc.bRejectLitPixels  = true;
    cdesc.cascadeCount      = (u32)cascade.cascadeCount;
    cdesc.blockerOffset     = blockerOffset;
    cdesc.bUseCascadesForRayT = false;
    cdesc.cascadeSize       = (float)cascade.cascadeSize;

    /* Per-cascade scale/offset: the classifier computes
     *   shadowCoord = (LightView * worldPos) * CascadeScale + CascadeOffset
     * which must match the CSM's shadow-map UV (clip [-1,1] -> [0,1] UV).
     * lightProj is the per-cascade light orthographic projection; its
     * diagonal/translation give the scale/offset (x/y scaled by 0.5 for the
     * clip->UV conversion, z kept in [0,1] depth). */
    for (int i = 0; i < 4; i++) {
        if (i < cascade.cascadeCount) {
            /* cglm mat4 is column-major: m[col][row].  Diagonal = scale, 4th
             * column = translation. */
            const float (*pm)[4] = cascade.cascadeProj[i];
            cdesc.cascadeScale[i][0] = 0.5f * pm[0][0];
            cdesc.cascadeScale[i][1] = 0.5f * pm[1][1];
            cdesc.cascadeScale[i][2] = pm[2][2];
            cdesc.cascadeScale[i][3] = 0.0f;
            cdesc.cascadeOffset[i][0] = 0.5f * pm[3][0] + 0.5f;
            cdesc.cascadeOffset[i][1] = 0.5f * pm[3][1] + 0.5f;
            cdesc.cascadeOffset[i][2] = pm[3][2];
            cdesc.cascadeOffset[i][3] = 0.0f;
        } else {
            for (int k = 0; k < 4; k++) {
                cdesc.cascadeScale[i][k]  = 0.0f;
                cdesc.cascadeOffset[i][k] = 0.0f;
            }
        }
    }
    /* Matrices. */
    memcpy(cdesc.viewToWorld, camera->cameraUbo.invViewProjectionNoJitter, sizeof(cdesc.viewToWorld));
    memcpy(cdesc.lightView, cascade.cascadeLightView[0], sizeof(cdesc.lightView));
    mat4 invLightView;
    glm_mat4_inv(cascade.cascadeLightView[0], invLightView);
    memcpy(cdesc.inverseLightView, invLightView, sizeof(cdesc.inverseLightView));

    vulkanBeginProfile(cmd, &hsProfile, 0);
    FfxErrorCode result = ffxClassifierContextShadowDispatch(&classifierContext, &cdesc);
    if (result != FFX_OK) {
        utils::error("vulkanShadowDenoisePass: ffxClassifierContextShadowDispatch failed: %d", result);
        vulkanEndProfile(cmd, &hsProfile, 0);
        return;
    }

    /* ── Denoiser (shadow mode) ──────────────────────────────────── */
    /* Copy the scene depth (D32) into an R32 image: the FFX denoiser's
     * internal history copy rejects a D32->R32 image copy, so it must be fed
     * an R32 depth buffer (mirrors the hybridshadows sample's copy-depth). */
    vulkanTransition(cmd, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, &depthCopyImage, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    {
        struct {
            u32 depthIndex;
            u32 outIndex;
            u32 width;
            u32 height;
        } pc = {
            .depthIndex = (u32)depth->sampledPoolIndex,
            .outIndex   = (u32)depthCopyImage.storagePoolIndex,
            .width      = width,
            .height     = height,
        };
        vulkanBindPipe(cmd, &depthCopyPipe);
        vulkanPush(cmd, &depthCopyPipe, sizeof(pc), &pc);
        u32 groupsX = (width + 7) / 8;
        u32 groupsY = (height + 7) / 8;
        vulkanDispatch(cmd, &depthCopyPipe, groupsX, groupsY, 1);
    }

    FfxDenoiserShadowsDispatchDescription ddesc = {};
    ddesc.commandList = ffxGetCommandListVK(cmd->cmd);
    ddesc.hitMaskResults = wrapImageResource(&rayHitImage, FFX_RESOURCE_USAGE_READ_ONLY,
                                             FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"hs_hitmask");
    ddesc.depth = wrapImageResource(&depthCopyImage, FFX_RESOURCE_USAGE_READ_ONLY,
                                    FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"hs_depth");
    ddesc.velocity = velocity ? wrapImageResource(velocity, FFX_RESOURCE_USAGE_READ_ONLY,
                                                  FFX_RESOURCE_STATE_COMPUTE_READ, L"hs_velocity")
                              : FfxResource{};
    ddesc.normal = wrapImageResource(worldNormal, FFX_RESOURCE_USAGE_READ_ONLY,
                                     FFX_RESOURCE_STATE_COMPUTE_READ, L"hs_normal");
    ddesc.shadowMaskOutput = wrapImageResource(&shadowMaskImage, FFX_RESOURCE_USAGE_UAV,
                                               FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"hs_mask");

    ddesc.motionVectorScale[0] = 1.0f / (float)width;
    ddesc.motionVectorScale[1] = 1.0f / (float)height;
    ddesc.normalsUnpackMul    = 1.0f;
    ddesc.normalsUnpackAdd    = 0.0f;
    if (cameraTransform) {
        ddesc.eye[0] = cameraTransform->pos[0];
        ddesc.eye[1] = cameraTransform->pos[1];
        ddesc.eye[2] = cameraTransform->pos[2];
    }
    ddesc.frameIndex = frameIndex;
    memcpy(ddesc.projectionInverse, camera->cameraUbo.invProjection, sizeof(ddesc.projectionInverse));
    /* Reprojection: current (ndc,depth) -> previous clip space.
     * reproj = prevViewProjection * invViewProjection (projection is
     * constant frame-to-frame, so this equals proj * prevView * invViewProj). */
    mat4 reproj;
    glm_mat4_mul(camera->cameraUbo.prevViewProjectionNoJitter,
                 camera->cameraUbo.invViewProjectionNoJitter, reproj);
    memcpy(ddesc.reprojectionMatrix, reproj, sizeof(ddesc.reprojectionMatrix));
    memcpy(ddesc.viewProjectionInverse, camera->cameraUbo.invViewProjectionNoJitter,
           sizeof(ddesc.viewProjectionInverse));
    ddesc.depthSimilaritySigma = depthSigma;

    FfxErrorCode dresult = ffxDenoiserContextDispatchShadows(&denoiserContext, &ddesc);
    if (dresult != FFX_OK) {
        utils::error("vulkanShadowDenoisePass: ffxDenoiserContextDispatchShadows failed: %d", dresult);
        vulkanEndProfile(cmd, &hsProfile, 0);
        return;
    }
    vulkanEndProfile(cmd, &hsProfile, 0);

    if (dumpEnabled) {
        char path[512];
        snprintf(path, sizeof(path), "%s/hs_rayhit_%06u.jpg", getenv("ENGINE_HS_DUMP"), frameIndex);
        utils::info("vulkanShadowDenoisePass: dumping rayHit to %s", path);
        vulkanSaveImage(&rayHitImage, path);
        snprintf(path, sizeof(path), "%s/hs_mask_%06u.jpg", getenv("ENGINE_HS_DUMP"), frameIndex);
        utils::info("vulkanShadowDenoisePass: dumping shadow mask to %s", path);
        vulkanSaveImage(&shadowMaskImage, path);
    }

    /* The shadow mask is left in GENERAL (the FFX UAV state); GENERAL allows
     * SRV reads, so the fragment shaders can sample it directly. */
    /* The depth buffer is a depth attachment for downstream passes; the FFX
     * classifier/denoiser left it in SHADER_READ_ONLY, so restore it. */
    vulkanTransition(cmd, depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, 1);
    /* The velocity + world-normal buffers are color attachments for
     * downstream passes; the FFX denoiser left them in SHADER_READ_ONLY, so
     * restore them. */
    if (velocity) {
        vulkanTransition(cmd, velocity, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    }
    vulkanTransition(cmd, worldNormal, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    frameIndex++;
}

/* ── System lifecycle ─────────────────────────────────────────────── */
VulkanShadowDenoisePass::VulkanShadowDenoisePass() : System("shadow_denoise") {}

void VulkanShadowDenoisePass::added() {
    utils::info("vulkanShadowDenoisePass: added() called, ENGINE_HYBRID_SHADOWS=%s ENGINE_HS_DUMP=%s",
                getenv("ENGINE_HYBRID_SHADOWS") ? getenv("ENGINE_HYBRID_SHADOWS") : "(null)",
                getenv("ENGINE_HS_DUMP") ? getenv("ENGINE_HS_DUMP") : "(null)");
    const char* env = getenv("ENGINE_HYBRID_SHADOWS");
    if (env && *env && atoi(env)) hsEnabled = 1;
    const char* sunEnv = getenv("ENGINE_HS_SUN_ANGLE");
    if (sunEnv && *sunEnv) sunAngleDeg = (float)atof(sunEnv);
    const char* blockerEnv = getenv("ENGINE_HS_BLOCKER_OFFSET");
    if (blockerEnv && *blockerEnv) blockerOffset = (float)atof(blockerEnv);
    const char* sigmaEnv = getenv("ENGINE_HS_DEPTH_SIGMA");
    if (sigmaEnv && *sigmaEnv) depthSigma = (float)atof(sigmaEnv);
    const char* dumpEnv = getenv("ENGINE_HS_DUMP");
    if (dumpEnv && *dumpEnv) dumpEnabled = 1;
    dumpChecked = 1;
    hsEnvChecked = 1;

    utils::signalSubscribe("swapchainCreated", swapchainCreated);
    hsProfile      = vulkanCreateProfile("hybrid_shadows");
    hsProfileReady = 1;
    if (hsEnabled) {
        utils::info("vulkanShadowDenoisePass: enabled (sunAngle=%.1f blockerOffset=%.5f)",
                    sunAngleDeg, blockerOffset);
    }
}

void VulkanShadowDenoisePass::removed() {
    hsDestroyResources();
    if (depthCopyPipeReady) {
        vulkanDestroyPipe(&depthCopyPipe);
        depthCopyPipe      = VulkanPipe{};
        depthCopyPipeReady = 0;
    }
    if (ffxScratch) {
        free(ffxScratch);
        ffxScratch     = NULL;
        ffxScratchSize = 0;
    }
    ffxInterface    = FfxInterface{};
    ffxBackendReady = 0;
    if (hsProfileReady) {
        vulkanDestroyProfile(&hsProfile);
        hsProfile      = VulkanProfile{};
        hsProfileReady = 0;
    }
}

void VulkanShadowDenoisePass::preUpdate() {
    if (vulkan.skipFrame) return;
    if (hsProfileReady) vulkanResetProfile(vulkan.currentCmd, &hsProfile, 0);
}

void VulkanShadowDenoisePass::update() {
    /* Publish the mask index for the lit fragment shaders (0 = PCF
     * fallback). The shadow pass zero-clears the UBO each frame, so this
     * has to re-apply the index every frame (and clear it when disabled). */
    struct VulkanImage* mask = vulkanShadowDenoisePassGetMask();
    vulkanResourceSetShadowMaskImageIndex(hsEnabled && mask ? (u32)mask->sampledPoolIndex : 0u);

    if (!hsEnabled || vulkan.skipFrame) return;

    VulkanImage* depth       = vulkanFrameResourcesGetDepth();
    VulkanImage* worldNormal = vulkanFrameResourcesGetWorldNormal();
    VulkanImage* velocity    = vulkanFrameResourcesGetVelocity();
    if (!depth || !worldNormal) return;

    Entity* camEntity = cameraGetEntity();
    if (!camEntity) return;
    Camera*    camera = getComponent(camEntity->scene, Camera, camEntity->id);
    Transform* camT   = getComponent(camEntity->scene, Transform, camEntity->id);
    if (!camera) return;

    VulkanCommand* cmd = vulkan.currentCmd;
    hsDispatch(cmd, depth, worldNormal, velocity, camera, camT);
}

static void swapchainCreated(void* _) {
    (void)_;
    hsDestroyResources();
}

/* ── Public API ───────────────────────────────────────────────────── */
void vulkanShadowDenoisePassSetEnabled(char enabled) {
    hsEnabled = enabled;
    utils::info("vulkanShadowDenoisePass: %s", enabled ? "enabled" : "disabled");
}

char vulkanShadowDenoisePassIsEnabled(void) {
    return hsEnabled;
}

float vulkanShadowDenoisePassGetSunAngle(void) {
    return sunAngleDeg;
}

void vulkanShadowDenoisePassSetSunAngle(float degrees) {
    /* A zero/near-zero sun disc degenerates the classifier's Poisson PCF
     * (single-tap "definitely lit" everywhere), so clamp to a small floor. */
    if (degrees < 0.1f) degrees = 0.1f;
    if (degrees > 10.0f) degrees = 10.0f;
    sunAngleDeg = degrees;
    utils::info("vulkanShadowDenoisePass: sun angle = %.2f deg", sunAngleDeg);
}

VulkanImage* vulkanShadowDenoisePassGetMask(void) {
    return shadowMaskImage.img ? &shadowMaskImage : NULL;
}
}  // namespace engine