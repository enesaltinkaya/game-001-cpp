#include "VulkanHiZPass.h"
#include "events/Events.h"
#include "renderer/Renderer.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"

static void added(void);
static void preUpdate(void);
static void update(void);
static void postUpdate(void);
static void removed(void);

static double elapsedCPU;
static double elapsedGPU;

System vulkanHiZPass = {
    .name                = "hiz",
    .added               = added,
    .removed             = removed,
    .preUpdate           = preUpdate,
    .update              = update,
    .postUpdate          = postUpdate,
    .cpuElapsedLastFrame = 0.0,
    .cpuElapsed          = 0.0,
    .gpuElapsed          = 0.0,
    .priority            = 0,
};

#define MAX_HIZ_MIPS 16

static VulkanPipe copyDepthPipe;
static VulkanPipe downsamplePipe;

// Double-buffered Hi-Z images: [0] and [1] alternate each frame
static VulkanImage hizImages[2];
static int         currentHiZIndex;

// Per-mip image views for each Hi-Z image
static VkImageView mipViews[2][MAX_HIZ_MIPS];
// Per-mip pool indices (registered in global bindless pool)
static int         mipSampledPoolIndex[2][MAX_HIZ_MIPS];
static int         mipStoragePoolIndex[2][MAX_HIZ_MIPS];
static int         mipCount;

typedef struct HiZPushConstants {
    u32 srcIndex;   // sampled pool index of source image
    u32 dstIndex;   // storage pool index of destination image
    u32 width;
    u32 height;
} HiZPushConstants;

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

static void createMipViews(int imageIndex) {
    VulkanImage* img = &hizImages[imageIndex];
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
                          &mipViews[imageIndex][i]);

        // Register per-mip views in global bindless pool
        mipSampledPoolIndex[imageIndex][i] =
            vulkanAddImageViewToPool(mipViews[imageIndex][i]);
        mipStoragePoolIndex[imageIndex][i] =
            vulkanAddStorageImageViewToPool(mipViews[imageIndex][i]);
    }
}

static void destroyMipViews(int imageIndex) {
    for (int i = 0; i < MAX_HIZ_MIPS; i++) {
        if (mipViews[imageIndex][i]) {
            vulkanRemoveImageViewFromPool(mipSampledPoolIndex[imageIndex][i]);
            vulkanRemoveStorageImageViewFromPool(
                mipStoragePoolIndex[imageIndex][i]);
            vkDestroyImageView(vulkan.device, mipViews[imageIndex][i], NULL);
            mipViews[imageIndex][i]            = VK_NULL_HANDLE;
            mipSampledPoolIndex[imageIndex][i] = 0;
            mipStoragePoolIndex[imageIndex][i] = 0;
        }
    }
}

static void destroyHiZ(int imageIndex) {
    destroyMipViews(imageIndex);
    if (hizImages[imageIndex].img) {
        vulkanDestroyImage(&hizImages[imageIndex], NULL);
        hizImages[imageIndex] = VulkanImage{0};
    }
}

static void createHiZ(int imageIndex, u32 width, u32 height, int mips) {
    hizImages[imageIndex] = vulkanCreateImage(
        .name      = imageIndex == 0 ? "hiz_0" : "hiz_1",
        .format    = VK_FORMAT_R32G32_SFLOAT,
        .usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        .aspect    = VK_IMAGE_ASPECT_COLOR_BIT,
        .width     = static_cast<int>(width),
        .height    = static_cast<int>(height),
        .mipLevels = mips);
    createMipViews(imageIndex);
}

static void swapchainCreated(void*) {
    for (int i = 0; i < 2; i++) {
        destroyHiZ(i);
    }
    mipCount        = 0;
    currentHiZIndex = 0;
}

static void added(void) {
    signalSubscribe("swapchainCreated", swapchainCreated);

    copyDepthPipe = vulkanCreatePipe(
        .name = "hiz_copy_depth",
        .comp = "shaders/pass/hiz/spv/hiz_copy_depth.comp.spv");

    downsamplePipe = vulkanCreatePipe(
        .name = "hiz_downsample",
        .comp = "shaders/pass/hiz/spv/hiz_downsample.comp.spv");
}

static void preUpdate(void) {
    if (vulkan.skipFrame) {
        return;
    }

    VulkanImage* depthImg = vulkanFrameResourcesGetDepth();
    if (!depthImg) return;

    if (!hizImages[0].img) {
        u32 w = depthImg->extent.width;
        u32 h = depthImg->extent.height;
        int newMipCount = calculateMipCount(w, h);

        for (int i = 0; i < 2; i++) {
            createHiZ(i, w, h, newMipCount);
        }
        mipCount        = newMipCount;
        currentHiZIndex = 0;
    }

    vulkanResetProfile(vulkan.currentCmd, &copyDepthPipe.profile, 0);
    vulkanResetProfile(vulkan.currentCmd, &downsamplePipe.profile, 0);
}

static void update(void) {
    if (vulkan.skipFrame) return;

    VulkanImage* depthImg = vulkanFrameResourcesGetDepth();
    if (!depthImg || !hizImages[0].img) return;

    VulkanCommand* cmd = vulkan.currentCmd;
    u32 w              = depthImg->extent.width;
    u32 h              = depthImg->extent.height;

    int curIdx = currentHiZIndex;
    VkImageLayout reusedMipOldLayout =
        hizImages[curIdx].layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_IMAGE_LAYOUT_UNDEFINED
            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    vulkanBeginProfile(cmd, &copyDepthPipe.profile, 0);

    // === STEP 1: Transition depth from attachment to shader read ===
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

    // Transition Hi-Z mip 0 to GENERAL for storage write.
    {
        VkImageMemoryBarrier barrier = {};
        barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = reusedMipOldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                    ? VK_ACCESS_SHADER_READ_BIT
                                    : 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.oldLayout     = reusedMipOldLayout;
        barrier.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
        barrier.image         = hizImages[curIdx].img;
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

    // === STEP 2: Copy depth → Hi-Z mip 0 ===
    vulkanBindPipe(cmd, &copyDepthPipe);

    HiZPushConstants pc = {
        .srcIndex = (u32)depthImg->sampledPoolIndex,
        .dstIndex = (u32)mipStoragePoolIndex[curIdx][0],
        .width    = w,
        .height   = h,
    };
    vulkanPush(cmd, &copyDepthPipe, sizeof(pc), &pc);

    u32 groupsX = (w + 7) / 8;
    u32 groupsY = (h + 7) / 8;
    vulkanDispatch(cmd, &copyDepthPipe, groupsX, groupsY, 1);

    vulkanEndProfile(cmd, &copyDepthPipe.profile, 0);

    // === STEP 3: Downsample mip chain ===
    vulkanBeginProfile(cmd, &downsamplePipe.profile, 0);

    u32 srcW = w;
    u32 srcH = h;

    if (isDebug()) {
        vulkanLabelBeginColor(cmd, "hi-z downsample", 1.0f, 1.0f, 0.0f, 1.0f);
    }

    for (int mip = 1; mip < mipCount; mip++) {
        u32 dstW = srcW > 1 ? srcW >> 1 : 1;
        u32 dstH = srcH > 1 ? srcH >> 1 : 1;

        // Barrier: source mip (mip-1) write → read
        {
            VkImageMemoryBarrier barrier = {};
            barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
            barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.image         = hizImages[curIdx].img;
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

        // Barrier: dest mip to GENERAL for writing.
        {
            VkImageMemoryBarrier barrier = {};
            barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = reusedMipOldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                        ? VK_ACCESS_SHADER_READ_BIT
                                        : 0;
            barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.oldLayout     = reusedMipOldLayout;
            barrier.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
            barrier.image         = hizImages[curIdx].img;
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
            .srcIndex = (u32)mipSampledPoolIndex[curIdx][mip - 1],
            .dstIndex = (u32)mipStoragePoolIndex[curIdx][mip],
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

    if (isDebug()) {
        vulkanLabelEnd(cmd);
    }

    // Final barrier: make the last mip readable
    {
        VkImageMemoryBarrier barrier = {};
        barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.image         = hizImages[curIdx].img;
        barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel   = mipCount - 1;
        barrier.subresourceRange.levelCount     = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = 1;
        vkCmdPipelineBarrier(cmd->cmd,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, NULL, 0, NULL, 1, &barrier);
    }

    // Transition depth back to depth attachment
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

    vulkanEndProfile(cmd, &downsamplePipe.profile, 0);

    // Track layout
    hizImages[curIdx].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Swap: next frame will write to the other index
    currentHiZIndex = 1 - currentHiZIndex;

    elapsedGPU = copyDepthPipe.profile.elapsed + downsamplePipe.profile.elapsed;
}

static void postUpdate(void) {
    vulkanHiZPass.cpuElapsed = elapsedCPU;
    vulkanHiZPass.gpuElapsed = elapsedGPU;
}

static void removed(void) {
    for (int i = 0; i < 2; i++) {
        destroyHiZ(i);
    }

    vulkanDestroyPipe(&copyDepthPipe);
    vulkanDestroyPipe(&downsamplePipe);
}

// --- Public API ---

VulkanImage* vulkanHiZGetCurrentImage(void) {
    int justCompleted = 1 - currentHiZIndex;
    return hizImages[justCompleted].img ? &hizImages[justCompleted] : NULL;
}

VulkanImage* vulkanHiZGetPreviousImage(void) {
    // Previous frame's Hi-Z is at currentHiZIndex (the one NOT just written)
    int prevIdx = currentHiZIndex;
    if (!hizImages[prevIdx].img ||
        hizImages[prevIdx].layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        return NULL;
    }
    return &hizImages[prevIdx];
}

int vulkanHiZGetMipCount(void) {
    return mipCount;
}

u32 vulkanHiZGetMipSampledIndex(int mip) {
    int justCompleted = 1 - currentHiZIndex;
    if (mip < 0 || mip >= mipCount) return 0;
    return (u32)mipSampledPoolIndex[justCompleted][mip];
}

u32 vulkanHiZGetMipStorageIndex(int mip) {
    int justCompleted = 1 - currentHiZIndex;
    if (mip < 0 || mip >= mipCount) return 0;
    return (u32)mipStoragePoolIndex[justCompleted][mip];
}
