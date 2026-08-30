#include "VulkanOcclusionPass.h"
#include "VulkanOcclusionPass.h"
#include "ecs/Ecs.h"
#include "ecs/system/scene/Scene.h"
#include "events/Events.h"
#include "renderer/Renderer.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/scene/VulkanScene.h"
#include "renderer/vulkan/scene/VulkanVisibleScenes.h"

namespace engine {
static void recreateDepthPipelines(void);

static double elapsedCPU;
static double elapsedGPU;

VulkanOcclusionPass vulkanOcclusionPass;

VulkanOcclusionPass::VulkanOcclusionPass() : System("occlusion") {}

#define MAX_HIZ_MIPS 16

// Early HiZ image (single, not double-buffered)
static VulkanImage earlyHiZImage;
static VkImageView earlyMipViews[MAX_HIZ_MIPS];
static int         earlyMipSampledPoolIndex[MAX_HIZ_MIPS];
static int         earlyMipStoragePoolIndex[MAX_HIZ_MIPS];
static int         earlyMipCount;

// Compute pipes
static VulkanPipe copyDepthPipe;
static VulkanPipe downsamplePipe;

// Phase 2 culling pipe
static VulkanPipe phase2CullingPipe;

// Phase 2 depth rendering pipes
static VulkanPipe phase2DepthPipe;
static VulkanPipe phase2DepthDSPipe;
static VulkanPipe statsPipe;

typedef struct HiZPushConstants {
    u32 srcIndex;
    u32 dstIndex;
    u32 width;
    u32 height;
} HiZPushConstants;

// Phase 2 culling push constants (must match shader)
typedef struct Phase2CullingPC {
    u64 drawInstanceBufferAddress;
    u64 visibilityBufferAddress;
    u64 transformBufferAddress;
    u64 indirectBufferAddress;
    u64 culledBufferAddress;
    u64 drawCountBufferAddress;
    u64 dsIndirectBufferAddress;
    u64 dsCulledBufferAddress;
    u64 dsDrawCountBufferAddress;
    u32 maxDrawInstances;
    u32 hizTextureIndex;
} Phase2CullingPC;

// Depth push constants (same layout as depth pass)
typedef struct DepthPushConstants {
    u64 transformBufferAddress;
    u64 prevTransformBufferAddress;
    u64 drawInstanceBufferAddress;
    u64 culledBufferAddress;
    u64 jointMatrixBufferAddress;
    u64 entitySkinMapBufferAddress;
    u64 prevJointMatrixBufferAddress;
} DepthPushConstants;

typedef struct StatsPushConstants {
    u64 drawInstanceBufferAddress;
    u64 culledBufferAddress;
    u64 countBufferAddress;
    u64 statsBufferAddress;
    u32 maxDrawInstances;
} StatsPushConstants;

// Vertex input description for SceneVertex (48 bytes)
static VkVertexInputBindingDescription sceneVertexBinding = {
    .binding   = 0,
    .stride    = sizeof(SceneVertex),
    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
};

static VkVertexInputAttributeDescription sceneVertexAttrs[] = {
    {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,    .offset = 0},
    {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,    .offset = 12},
    {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 24},
    {.location = 3, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,       .offset = 40},
    {.location = 4, .binding = 0, .format = VK_FORMAT_R32_UINT, .offset = 48},  // joints
    {.location = 5, .binding = 0, .format = VK_FORMAT_R32_UINT, .offset = 52},  // weights
};

static u32 calculateMipCount(u32 width, u32 height) {
    u32 maxDim = width > height ? width : height;
    u32 levels = 1;
    while (maxDim > 1) {
        maxDim >>= 1;
        levels++;
    }
    if (levels > MAX_HIZ_MIPS) levels = MAX_HIZ_MIPS;
    return levels;
}

static void createEarlyMipViews(void) {
    VulkanImage* img = &earlyHiZImage;
    for (int i = 0; i < img->mipLevels; i++) {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image    = img->img;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format   = (VkFormat)img->format;
        viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel   = i;
        viewInfo.subresourceRange.levelCount     = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount     = 1;
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_R;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_G;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_B;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_A;
        vkCreateImageView(vulkan.device, &viewInfo, NULL,
                          &earlyMipViews[i]);
        earlyMipSampledPoolIndex[i] =
            vulkanAddImageViewToPool(earlyMipViews[i]);
        earlyMipStoragePoolIndex[i] =
            vulkanAddStorageImageViewToPool(earlyMipViews[i]);
    }
}

static void destroyEarlyHiZ(void) {
    for (int i = 0; i < MAX_HIZ_MIPS; i++) {
        if (earlyMipViews[i]) {
            vulkanRemoveImageViewFromPool(earlyMipSampledPoolIndex[i]);
            vulkanRemoveStorageImageViewFromPool(earlyMipStoragePoolIndex[i]);
            vkDestroyImageView(vulkan.device, earlyMipViews[i], NULL);
            earlyMipViews[i]            = VK_NULL_HANDLE;
            earlyMipSampledPoolIndex[i] = 0;
            earlyMipStoragePoolIndex[i] = 0;
        }
    }
    if (earlyHiZImage.img) {
        vulkanDestroyImage(&earlyHiZImage, NULL);
        earlyHiZImage = VulkanImage{};
    }
    earlyMipCount = 0;
}

static void swapchainCreated(void*) {
    recreateDepthPipelines();
    destroyEarlyHiZ();
}

void VulkanOcclusionPass::added() {
    utils::signalSubscribe("swapchainCreated", swapchainCreated);

    copyDepthPipe = vulkanCreatePipe(
        .name = "phase2_hiz_copy_depth",
        .comp = "shaders/pass/hiz/spv/hiz_copy_depth.comp.spv");

    downsamplePipe = vulkanCreatePipe(
        .name = "phase2_hiz_downsample",
        .comp = "shaders/pass/hiz/spv/hiz_downsample.comp.spv");

    phase2CullingPipe = vulkanCreatePipe(
        .name = "scene_culling_phase2",
        .comp = "shaders/pass/scene/spv/scene_culling_phase2.comp.spv");

    statsPipe = vulkanCreatePipe(
        .name = "scene_stats",
        .comp = "shaders/pass/scene/spv/scene_stats.comp.spv");

    recreateDepthPipelines();
}

static void recreateDepthPipelines(void) {
    if (phase2DepthPipe.pipe) {
        vulkanDestroyPipe(&phase2DepthPipe);
        vulkanDestroyPipe(&phase2DepthDSPipe);
    }

    phase2DepthPipe = vulkanCreatePipe(
        .name               = "phase2_depth",
        .vs                 = "shaders/pass/scene/spv/scene_depth.vert.spv",
        .fs                 = "shaders/pass/scene/spv/scene_depth.frag.spv",
        .colorFormat1       = VK_FORMAT_R16G16_SFLOAT,
        .colorFormat2       = VK_FORMAT_R16G16_SNORM,
        .depthFormat        = VK_FORMAT_D32_SFLOAT,
        .vertexAttributes   = sceneVertexAttrs,
        .vertexAttributeCount = 6,
        .vertexBindings     = &sceneVertexBinding,
        .vertexBindingCount = 1);

    phase2DepthDSPipe = vulkanCreatePipe(
        .name               = "phase2_depth_ds",
        .vs                 = "shaders/pass/scene/spv/scene_depth.vert.spv",
        .fs                 = "shaders/pass/scene/spv/scene_depth.frag.spv",
        .colorFormat1       = VK_FORMAT_R16G16_SFLOAT,
        .colorFormat2       = VK_FORMAT_R16G16_SNORM,
        .depthFormat        = VK_FORMAT_D32_SFLOAT,
        .noCull             = 1,
        .vertexAttributes   = sceneVertexAttrs,
        .vertexAttributeCount = 6,
        .vertexBindings     = &sceneVertexBinding,
        .vertexBindingCount = 1);
}

void VulkanOcclusionPass::preUpdate() {
    if (vulkan.skipFrame) return;

    VulkanImage* depthImg = vulkanFrameResourcesGetDepth();
    if (!depthImg) return;

    if (!earlyHiZImage.img) {
        u32 w = depthImg->extent.width;
        u32 h = depthImg->extent.height;
        earlyMipCount = calculateMipCount(w, h);

        earlyHiZImage = vulkanCreateImage(
            .name      = "early_hiz",
            .format    = VK_FORMAT_R32G32_SFLOAT,
            .usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            .aspect    = VK_IMAGE_ASPECT_COLOR_BIT,
            .width     = static_cast<int>(w),
            .height    = static_cast<int>(h),
            .mipLevels = earlyMipCount);
        createEarlyMipViews();
    }

    vulkanResetProfile(vulkan.currentCmd, &copyDepthPipe.profile, 0);
}

void VulkanOcclusionPass::update() {
    if (vulkan.skipFrame) return;

    VulkanImage* depthImg        = vulkanFrameResourcesGetDepth();
    VulkanImage* velocityImg     = vulkanFrameResourcesGetVelocity();
    VulkanImage* viewNormalImg   = vulkanFrameResourcesGetViewNormal();
    if (!depthImg || !earlyHiZImage.img || !velocityImg) return;

    VulkanCommand* cmd = vulkan.currentCmd;
    u32 fi             = renderer.flightIndex;
    u32 w              = depthImg->extent.width;
    u32 h              = depthImg->extent.height;
    VkImageLayout reusedMipOldLayout =
        earlyHiZImage.layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_IMAGE_LAYOUT_UNDEFINED
            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    vulkanBeginProfile(cmd, &copyDepthPipe.profile, 0);

    // ========== STEP A: Build early HiZ from Phase 1 depth ==========

    // Transition depth to shader read
    {
        VkImageMemoryBarrier barrier = {};
        barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.image         = depthImg->img;
        barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        barrier.subresourceRange.baseMipLevel   = 0;
        barrier.subresourceRange.levelCount     = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = 1;
        vkCmdPipelineBarrier(cmd->cmd,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, NULL, 0, NULL, 1, &barrier);
    }

    // Transition early HiZ mip 0 to GENERAL
    {
        VkImageMemoryBarrier barrier = {};
        barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = reusedMipOldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                    ? VK_ACCESS_SHADER_READ_BIT
                                    : 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.oldLayout     = reusedMipOldLayout;
        barrier.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
        barrier.image         = earlyHiZImage.img;
        barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel   = 0;
        barrier.subresourceRange.levelCount     = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = 1;
        vkCmdPipelineBarrier(cmd->cmd,
                             reusedMipOldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                 ? (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
                                 : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, NULL, 0, NULL, 1, &barrier);
    }

    // Copy depth → early HiZ mip 0
    vulkanBindPipe(cmd, &copyDepthPipe);
    HiZPushConstants pc = {
        .srcIndex = (u32)depthImg->sampledPoolIndex,
        .dstIndex = (u32)earlyMipStoragePoolIndex[0],
        .width    = w,
        .height   = h,
    };
    vulkanPush(cmd, &copyDepthPipe, sizeof(pc), &pc);
    u32 groupsX = (w + 7) / 8;
    u32 groupsY = (h + 7) / 8;
    vulkanDispatch(cmd, &copyDepthPipe, groupsX, groupsY, 1);

    // Downsample mip chain
    u32 srcW = w;
    u32 srcH = h;
    for (int mip = 1; mip < earlyMipCount; mip++) {
        u32 dstW = srcW > 1 ? srcW >> 1 : 1;
        u32 dstH = srcH > 1 ? srcH >> 1 : 1;

        // Source mip write → read
        {
            VkImageMemoryBarrier barrier = {};
            barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
            barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.image         = earlyHiZImage.img;
            barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel   = mip - 1;
            barrier.subresourceRange.levelCount     = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount     = 1;
            vkCmdPipelineBarrier(cmd->cmd,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 0, NULL, 0, NULL, 1, &barrier);
        }
        // Dest mip to GENERAL
        {
            VkImageMemoryBarrier barrier = {};
            barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = reusedMipOldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                        ? VK_ACCESS_SHADER_READ_BIT
                                        : 0;
            barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.oldLayout     = reusedMipOldLayout;
            barrier.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
            barrier.image         = earlyHiZImage.img;
            barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel   = mip;
            barrier.subresourceRange.levelCount     = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount     = 1;
            vkCmdPipelineBarrier(cmd->cmd,
                                 reusedMipOldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                     ? (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
                                     : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 0, NULL, 0, NULL, 1, &barrier);
        }

        vulkanBindPipe(cmd, &downsamplePipe);
        HiZPushConstants downPC = {
            .srcIndex = (u32)earlyMipSampledPoolIndex[mip - 1],
            .dstIndex = (u32)earlyMipStoragePoolIndex[mip],
            .width    = srcW,
            .height   = srcH,
        };
        vulkanPush(cmd, &downsamplePipe, sizeof(downPC), &downPC);
        groupsX = (dstW + 7) / 8;
        groupsY = (dstH + 7) / 8;
        vulkanDispatch(cmd, &downsamplePipe, groupsX, groupsY, 1);

        srcW = dstW;
        srcH = dstH;
    }

    // Final barrier: last mip readable
    {
        VkImageMemoryBarrier barrier = {};
        barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.image         = earlyHiZImage.img;
        barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel   = earlyMipCount - 1;
        barrier.subresourceRange.levelCount     = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = 1;
        vkCmdPipelineBarrier(cmd->cmd,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, NULL, 0, NULL, 1, &barrier);
    }

    // Transition depth back to attachment
    {
        VkImageMemoryBarrier barrier = {};
        barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        barrier.image         = depthImg->img;
        barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        barrier.subresourceRange.baseMipLevel   = 0;
        barrier.subresourceRange.levelCount     = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = 1;
        vkCmdPipelineBarrier(cmd->cmd,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             0, 0, NULL, 0, NULL, 1, &barrier);
    }

    earlyHiZImage.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // ========== STEP B: Phase 2 culling ==========

    u32 visibleSceneCount = 0;
    const Scene** visibleScenes = vulkanGetVisibleScenes(&visibleSceneCount);

    // Reset Phase 2 draw counts
    for (u32 si = 0; si < visibleSceneCount; si++) {
        const Scene* scene = visibleScenes[si];
        if (!scene->backendScene) continue;
        VulkanScene* vs  = static_cast<VulkanScene*>(scene->backendScene);
        if (!vs->totalDraws) continue;

        vkCmdFillBuffer(cmd->cmd,
                        vs->phase2DrawCountBuffer[fi].buf,
                        0, sizeof(u32), 0);
        vkCmdFillBuffer(cmd->cmd,
                        vs->phase2DsDrawCountBuffer[fi].buf,
                        0, sizeof(u32), 0);
    }

    // Barrier: clear → compute
    {
        VkMemoryBarrier clearBarrier = {};
        clearBarrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        clearBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        clearBarrier.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd->cmd,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &clearBarrier, 0, NULL, 0, NULL);
    }

    // Phase 2 culling dispatch
    vulkanBindPipe(cmd, &phase2CullingPipe);
    for (u32 si = 0; si < visibleSceneCount; si++) {
        const Scene* scene = visibleScenes[si];
        if (!scene->backendScene) continue;
        VulkanScene* vs  = static_cast<VulkanScene*>(scene->backendScene);
        if (!vs->totalDraws) continue;

        Phase2CullingPC p2pc = {
            .drawInstanceBufferAddress = vs->drawInstanceBuffer.address,
            .visibilityBufferAddress   = vs->visibilityBuffer[fi].address,
            .transformBufferAddress    = vs->transformBuffer[fi].address,
            .indirectBufferAddress     = vs->phase2IndirectBuffer[fi].address,
            .culledBufferAddress       = vs->phase2CulledBuffer[fi].address,
            .drawCountBufferAddress    = vs->phase2DrawCountBuffer[fi].address,
            .dsIndirectBufferAddress   = vs->phase2DsIndirectBuffer[fi].address,
            .dsCulledBufferAddress     = vs->phase2DsCulledBuffer[fi].address,
            .dsDrawCountBufferAddress  = vs->phase2DsDrawCountBuffer[fi].address,
            .maxDrawInstances          = vs->totalDraws,
            .hizTextureIndex           = (u32)earlyHiZImage.sampledPoolIndex,
        };
        vulkanPush(cmd, &phase2CullingPipe, sizeof(Phase2CullingPC), &p2pc);
        u32 groups = (vs->totalDraws + 63) / 64;
        vulkanDispatch(cmd, &phase2CullingPipe, groups, 1, 1);
    }

    // Barrier: compute → indirect draw
    {
        VkMemoryBarrier computeBarrier = {};
        computeBarrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        computeBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        computeBarrier.dstAccessMask =
            VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd->cmd,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                                 VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                             0, 1, &computeBarrier, 0, NULL, 0, NULL);
    }

    // ========== STEP C: Phase 2 depth rendering (no clear) ==========
    vulkanBeginRender(.cmd    = cmd,
                      .pipe    = &phase2DepthPipe,
                      .color1  = velocityImg,
                      .color2  = viewNormalImg,
                      .depth   = depthImg);

    vulkanViewport(cmd, 0, h, w, -((i32)h));
    vulkanScissor(cmd, 0, 0, w, h);

    // Phase 2: single-sided depth draws
    vulkanBindPipe(cmd, &phase2DepthPipe);
    for (u32 si = 0; si < visibleSceneCount; si++) {
        const Scene* scene = visibleScenes[si];
        if (!scene->backendScene) continue;
        VulkanScene* vs  = static_cast<VulkanScene*>(scene->backendScene);
        if (!vs->totalDraws) continue;

        vulkanBindVertex(cmd, &vs->vertexBuffer, 0, NULL, 0, NULL, 0);
        vulkanBindIndex(cmd, &vs->indexBuffer, 0, VK_INDEX_TYPE_UINT32);

        DepthPushConstants dpc = {
            .transformBufferAddress     = vs->transformBuffer[fi].address,
            .prevTransformBufferAddress = vs->prevTransformBuffer[fi].address,
            .drawInstanceBufferAddress  = vs->drawInstanceBuffer.address,
            .culledBufferAddress        = vs->phase2CulledBuffer[fi].address,
            .jointMatrixBufferAddress   = vs->jointMatrixBuffer[fi].address,
            .entitySkinMapBufferAddress = vs->entitySkinMapBuffer[fi].address,
            .prevJointMatrixBufferAddress = vs->prevJointMatrixBuffer[fi].address,
        };
        vulkanPush(cmd, &phase2DepthPipe, sizeof(DepthPushConstants), &dpc);

        vkCmdDrawIndexedIndirectCount(cmd->cmd,
                                      vs->phase2IndirectBuffer[fi].buf, 0,
                                      vs->phase2DrawCountBuffer[fi].buf, 0,
                                      vs->totalDraws,
                                      sizeof(SceneDrawIndexedCommand));
    }

    // Phase 2: double-sided depth draws
    vulkanBindPipe(cmd, &phase2DepthDSPipe);
    for (u32 si = 0; si < visibleSceneCount; si++) {
        const Scene* scene = visibleScenes[si];
        if (!scene->backendScene) continue;
        VulkanScene* vs  = static_cast<VulkanScene*>(scene->backendScene);
        if (!vs->totalDraws) continue;

        vulkanBindVertex(cmd, &vs->vertexBuffer, 0, NULL, 0, NULL, 0);
        vulkanBindIndex(cmd, &vs->indexBuffer, 0, VK_INDEX_TYPE_UINT32);

        DepthPushConstants dpc = {
            .transformBufferAddress     = vs->transformBuffer[fi].address,
            .prevTransformBufferAddress = vs->prevTransformBuffer[fi].address,
            .drawInstanceBufferAddress  = vs->drawInstanceBuffer.address,
            .culledBufferAddress        = vs->phase2DsCulledBuffer[fi].address,
            .jointMatrixBufferAddress   = vs->jointMatrixBuffer[fi].address,
            .entitySkinMapBufferAddress = vs->entitySkinMapBuffer[fi].address,
            .prevJointMatrixBufferAddress = vs->prevJointMatrixBuffer[fi].address,
        };
        vulkanPush(cmd, &phase2DepthDSPipe, sizeof(DepthPushConstants), &dpc);

        vkCmdDrawIndexedIndirectCount(cmd->cmd,
                                      vs->phase2DsIndirectBuffer[fi].buf, 0,
                                      vs->phase2DsDrawCountBuffer[fi].buf, 0,
                                      vs->totalDraws,
                                      sizeof(SceneDrawIndexedCommand));
    }

    vulkanEndRender(cmd);

    // Depth barrier
    {
        VkMemoryBarrier depthBarrier = {};
        depthBarrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        depthBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depthBarrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        vkCmdPipelineBarrier(cmd->cmd,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                             0, 1, &depthBarrier, 0, NULL, 0, NULL);
    }

    // ========== STEP D: Stats accumulation (only when stats GUI is visible) ==========
    if (ecs.showStats) {
        // Zero stats readback buffers
        for (u32 si = 0; si < visibleSceneCount; si++) {
            const Scene* scene = visibleScenes[si];
            if (!scene->backendScene) continue;
            VulkanScene* vs  = static_cast<VulkanScene*>(scene->backendScene);
            if (!vs->totalDraws) continue;
            vkCmdFillBuffer(cmd->cmd, vs->statsReadbackBuffer[fi].buf, 0, sizeof(u32) * 2, 0);
        }

        // Barrier: fill → compute
        VkMemoryBarrier statsFillBarrier = {};
        statsFillBarrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        statsFillBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        statsFillBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd->cmd,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &statsFillBarrier, 0, NULL, 0, NULL);

        // Need compute visibility of culling results
        VkMemoryBarrier cullingResultsBarrier = {};
        cullingResultsBarrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        cullingResultsBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        cullingResultsBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd->cmd,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &cullingResultsBarrier, 0, NULL, 0, NULL);

        // Dispatch stats accumulation: 5 streams per scene
        // (opaque, ds, trans, phase2 opaque, phase2 ds)
        vulkanBindPipe(cmd, &statsPipe);
        for (u32 si = 0; si < visibleSceneCount; si++) {
            const Scene* scene = visibleScenes[si];
            if (!scene->backendScene) continue;
            VulkanScene* vs  = static_cast<VulkanScene*>(scene->backendScene);
            if (!vs->totalDraws) continue;

            u32 groups = (vs->totalDraws + 63) / 64;

            // Opaque single-sided
            StatsPushConstants spc = {
                .drawInstanceBufferAddress = vs->drawInstanceBuffer.address,
                .culledBufferAddress      = vs->culledBuffer[fi].address,
                .countBufferAddress       = vs->drawCountBuffer[fi].address,
                .statsBufferAddress       = vs->statsReadbackBuffer[fi].address,
                .maxDrawInstances         = vs->totalDraws,
            };
            vulkanPush(cmd, &statsPipe, sizeof(spc), &spc);
            vulkanDispatch(cmd, &statsPipe, groups, 1, 1);

            // Double-sided opaque
            spc.culledBufferAddress = vs->dsCulledBuffer[fi].address;
            spc.countBufferAddress  = vs->dsDrawCountBuffer[fi].address;
            vulkanPush(cmd, &statsPipe, sizeof(spc), &spc);
            vulkanDispatch(cmd, &statsPipe, groups, 1, 1);

            // Transparent
            spc.culledBufferAddress = vs->transCulledBuffer[fi].address;
            spc.countBufferAddress  = vs->transDrawCountBuffer[fi].address;
            vulkanPush(cmd, &statsPipe, sizeof(spc), &spc);
            vulkanDispatch(cmd, &statsPipe, groups, 1, 1);

            // Phase 2 opaque single-sided
            spc.culledBufferAddress = vs->phase2CulledBuffer[fi].address;
            spc.countBufferAddress  = vs->phase2DrawCountBuffer[fi].address;
            vulkanPush(cmd, &statsPipe, sizeof(spc), &spc);
            vulkanDispatch(cmd, &statsPipe, groups, 1, 1);

            // Phase 2 opaque double-sided
            spc.culledBufferAddress = vs->phase2DsCulledBuffer[fi].address;
            spc.countBufferAddress  = vs->phase2DsDrawCountBuffer[fi].address;
            vulkanPush(cmd, &statsPipe, sizeof(spc), &spc);
            vulkanDispatch(cmd, &statsPipe, groups, 1, 1);
        }

        // Barrier: compute write → host read
        VkMemoryBarrier statsReadBarrier = {};
        statsReadBarrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        statsReadBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        statsReadBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd->cmd,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &statsReadBarrier, 0, NULL, 0, NULL);
    }

    vulkanEndProfile(cmd, &copyDepthPipe.profile, 0);

    elapsedGPU = copyDepthPipe.profile.elapsed;
}

void VulkanOcclusionPass::postUpdate() {
    vulkanOcclusionPass.cpuElapsed = elapsedCPU;
    vulkanOcclusionPass.gpuElapsed = elapsedGPU;

    // Read back stats from previous frame (safe: GPU is done with it)
    if (ecs.showStats) {
        u32 totalDrawCalls     = 0;
        u32 totalTriangleCount = 0;

        u32 readbackFi = renderer.flightIndex;

        u32 visibleSceneCount = 0;
        const Scene** visibleScenes = vulkanGetVisibleScenes(&visibleSceneCount);
        for (u32 si = 0; si < visibleSceneCount; si++) {
            const Scene* scene = visibleScenes[si];
            if (!scene->backendScene) continue;
            VulkanScene* vs  = static_cast<VulkanScene*>(scene->backendScene);
            if (!vs->totalDraws) continue;

            u32* data = (u32*)vs->statsReadbackBuffer[readbackFi].vmaInfo.pMappedData;
            totalDrawCalls     += data[0];
            totalTriangleCount += data[1];
        }

        renderer.drawCalls     = totalDrawCalls;
        renderer.triangleCount = totalTriangleCount;
        renderer.instanceCount = totalDrawCalls;  // each draw = 1 instance
    }
}

void VulkanOcclusionPass::removed() {
    destroyEarlyHiZ();
    vulkanDestroyPipe(&copyDepthPipe);
    vulkanDestroyPipe(&downsamplePipe);
    vulkanDestroyPipe(&phase2CullingPipe);
    vulkanDestroyPipe(&phase2DepthPipe);
    vulkanDestroyPipe(&phase2DepthDSPipe);
    vulkanDestroyPipe(&statsPipe);
}
}  // namespace engine
