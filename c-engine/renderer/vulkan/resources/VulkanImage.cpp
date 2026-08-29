#include "VulkanImage.h"
#include <algorithm>
#include <cmath>
#include "../command/VulkanCommand.h"
#include "../utils/VulkanUtils.h"
#include "Utils.h"

#include "renderer/texture/TextureManager.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "string/String.h"

namespace engine {
static bool hasStencilComponent(VkFormat format);
static VkAccessFlags2 accessFlagsForLayout(VkImageLayout layout);
static VkPipelineStageFlags2 stageFlagsForLayout(VkImageLayout layout);
static void copyImage(VulkanCommand* cmd,
                      VulkanImage* source,
                      VulkanImage* target,
                      VkImageAspectFlags aspectMask,
                      VkImageLayout targetFinalLayout);

VulkanImage r_vulkanCreateImg(VulkanImageInfo info) {
    assert(info.name);
    if (info.layers > 1 && info.viewType == VK_IMAGE_VIEW_TYPE_2D) {
        if (info.type == VK_IMAGE_TYPE_3D) {
            // 3D: layers carries the volume depth — one 3D view covers it.
            info.viewType = VK_IMAGE_VIEW_TYPE_3D;
        } else {
            info.isArray  = 1;
            info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        }
    }

    VulkanImage image = {};
    image.layers      = info.layers;
    image.aspect      = info.aspect;
    image.layout      = VK_IMAGE_LAYOUT_UNDEFINED;
    image.mipLevels   = info.mipLevels;
    image.format      = info.format;
    image.extent      = VkExtent3D{static_cast<uint32_t>(info.width), static_cast<uint32_t>(info.height), 1};
    image.usage       = info.usage;
    image.viewType    = info.viewType;
    image.samples     = static_cast<VkSampleCountFlagBits>(info.samples);
    image.onHeap      = info.onHeap;

    VkImageCreateInfo imageCreateInfo = {};
    imageCreateInfo.sType             = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateInfo.flags             = info.flags;
    imageCreateInfo.format            = static_cast<VkFormat>(info.format);
    imageCreateInfo.mipLevels         = info.mipLevels;
    imageCreateInfo.initialLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    imageCreateInfo.sharingMode       = VK_SHARING_MODE_EXCLUSIVE;
    imageCreateInfo.tiling            = VK_IMAGE_TILING_OPTIMAL;

    if (info.type == VK_IMAGE_TYPE_3D) {
        imageCreateInfo.imageType   = VK_IMAGE_TYPE_3D;
        imageCreateInfo.extent      = VkExtent3D{static_cast<uint32_t>(info.width), static_cast<uint32_t>(info.height), static_cast<uint32_t>(info.layers)};
        imageCreateInfo.arrayLayers = 1;  // Must be 1 for 3D images
        image.extent                = imageCreateInfo.extent;  // keep the struct field in sync (depth)
    } else {
        imageCreateInfo.imageType   = VK_IMAGE_TYPE_2D;
        imageCreateInfo.extent      = VkExtent3D{static_cast<uint32_t>(info.width), static_cast<uint32_t>(info.height), 1};
        imageCreateInfo.arrayLayers = info.layers;
    }
    imageCreateInfo.samples           = static_cast<VkSampleCountFlagBits>(info.samples);
    imageCreateInfo.usage             = info.usage;

    VmaAllocationCreateInfo vmaCreateInfo = {};
    if (info.dedicated) {
        vmaCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        vmaCreateInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    } else {
        vmaCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }
    vmaCreateImage(vulkan.vmaAllocator,
                   &imageCreateInfo,
                   &vmaCreateInfo,
&image.img,
                    &image.vma,
                    nullptr);
    if (!image.img) {
        utils::warn("r_vulkanCreateImg: VMA image allocation failed (name=%s, format=%d)",
             info.name, static_cast<int>(info.format));
        return VulkanImage{};
    }

    VkImageViewCreateInfo imageViewCreateInfo           = {};
    imageViewCreateInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    imageViewCreateInfo.image                           = image.img;
    imageViewCreateInfo.viewType                        = info.viewType;
    imageViewCreateInfo.format                          = static_cast<VkFormat>(info.format);
    imageViewCreateInfo.subresourceRange.levelCount     = info.mipLevels;
    imageViewCreateInfo.subresourceRange.layerCount     = info.type == VK_IMAGE_TYPE_3D ? 1 : info.layers;
    imageViewCreateInfo.subresourceRange.aspectMask     = info.aspect;
    imageViewCreateInfo.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
    imageViewCreateInfo.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
    imageViewCreateInfo.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
    imageViewCreateInfo.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
    imageViewCreateInfo.subresourceRange.baseMipLevel   = 0;
    imageViewCreateInfo.subresourceRange.baseArrayLayer = 0;
    vkCreateImageView(vulkan.device, &imageViewCreateInfo, nullptr, &image.view);

    /* 3D images have no per-layer concept — layers holds the extent depth,
     * one 3D view covers the whole volume. */
    i32 viewCount = (info.type == VK_IMAGE_TYPE_3D) ? 1 : info.layers;
    image.views.resize(viewCount);
    for (i32 i = 0; i < viewCount; i++) {
        VkImageViewCreateInfo perLayerViewCI = {};
        perLayerViewCI.sType                 = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        if (info.type == VK_IMAGE_TYPE_3D) {
            perLayerViewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
            perLayerViewCI.image    = image.img;
            perLayerViewCI.format   = static_cast<VkFormat>(info.format);
            perLayerViewCI.subresourceRange.baseMipLevel   = 0;
            perLayerViewCI.subresourceRange.levelCount     = info.mipLevels;
            perLayerViewCI.subresourceRange.baseArrayLayer = 0;
            perLayerViewCI.subresourceRange.layerCount     = 1;
            perLayerViewCI.subresourceRange.aspectMask     = info.aspect;
            /* For 3D images we can't create per-slice 2D views easily,
             * so we just replicate the main 3D view for each 'layer'. */
            perLayerViewCI.viewType = info.viewType;
            perLayerViewCI.image    = image.img;
        } else {
            perLayerViewCI.viewType              = VK_IMAGE_VIEW_TYPE_2D;
            perLayerViewCI.image                 = image.img;
            perLayerViewCI.format                = static_cast<VkFormat>(info.format);
            perLayerViewCI.components.r          = VK_COMPONENT_SWIZZLE_R;
            perLayerViewCI.components.g          = VK_COMPONENT_SWIZZLE_G;
            perLayerViewCI.components.b          = VK_COMPONENT_SWIZZLE_B;
            perLayerViewCI.components.a          = VK_COMPONENT_SWIZZLE_A;
            perLayerViewCI.subresourceRange.baseMipLevel   = 0;
            perLayerViewCI.subresourceRange.baseArrayLayer = i;
            perLayerViewCI.subresourceRange.levelCount     = info.mipLevels;
            perLayerViewCI.subresourceRange.layerCount     = 1;
            perLayerViewCI.subresourceRange.aspectMask     = info.aspect;
        }
        vkCreateImageView(vulkan.device, &perLayerViewCI, nullptr, &image.views[i]);
        if (utils::isDebug()) {
            vulkanUtilsSetName(reinterpret_cast<uint64_t>(image.views[i]),
                               VK_OBJECT_TYPE_IMAGE_VIEW,
                               utils::strtmp("view %s arr: %d", info.name, i));
        }
    }

    if (!info.noPool) {
        vulkanAddImageToPool(&image);
    }

    utils::stringPrintf(&image.name, info.name);

    if (utils::isDebug()) {
        vulkanUtilsSetName(reinterpret_cast<uint64_t>(image.view),
                           VK_OBJECT_TYPE_IMAGE_VIEW,
                           utils::strtmp("%s%s", "view ", info.name));
        vulkanUtilsSetName(
            reinterpret_cast<uint64_t>(image.img),
            VK_OBJECT_TYPE_IMAGE,
            utils::strtmp("%s%s poolIndex: %d", "image ", info.name, image.sampledPoolIndex));
    }

    return image;
}

void vulkanDestroyImage(VulkanImage* image, VkFence fence) {
    vulkanRemoveImageFromPool(image);
    addImageGarbage(image, fence, nullptr);
    utils::stringDestroy(&image->name);
    if (image->onHeap) delete image;
}

void vulkanTransition(VulkanCommand* cmd,
                      VulkanImage* img,
                      VkImageLayout newLayout,
                      int baseLayer,
                      int _) {
    (void)baseLayer;
    if (img->layout == newLayout) {
        return;
    }
    VkImageLayout oldLayout = img->layout;

    VkImageMemoryBarrier2 barrier = {};
    barrier.sType                 = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    // barrier.srcStageMask          = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    // barrier.dstStageMask          = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    // barrier.srcAccessMask         = VK_ACCESS_2_MEMORY_READ_BIT |
    // VK_ACCESS_2_MEMORY_WRITE_BIT; barrier.dstAccessMask         =
    // VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

    barrier.srcStageMask  = stageFlagsForLayout(oldLayout);
    barrier.srcAccessMask = accessFlagsForLayout(oldLayout);
    barrier.dstStageMask  = stageFlagsForLayout(newLayout);
    barrier.dstAccessMask = accessFlagsForLayout(newLayout);

    barrier.oldLayout           = static_cast<VkImageLayout>(oldLayout);
    barrier.newLayout           = static_cast<VkImageLayout>(newLayout);
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = img->img;

    barrier.subresourceRange.baseMipLevel   = 0;
    barrier.subresourceRange.levelCount     = img->mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    /* For 3D images, arrayLayers is always 1 (depth is in extent.depth). */
    barrier.subresourceRange.layerCount     = img->viewType == VK_IMAGE_VIEW_TYPE_3D ? 1 : img->layers;
    barrier.subresourceRange.aspectMask     = img->aspect;

    if (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
        oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (hasStencilComponent(img->format)) {
            barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }
    }

    VkDependencyInfo dependencyInfo        = {};
    dependencyInfo.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependencyInfo.imageMemoryBarrierCount = 1;
    dependencyInfo.pImageMemoryBarriers    = &barrier;

    vkCmdPipelineBarrier2(cmd->cmd, &dependencyInfo);
    img->layout = newLayout;
}

bool hasStencilComponent(VkFormat format) {
    return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT;
}

// Helper function to deduce access masks from layout
VkAccessFlags2 accessFlagsForLayout(VkImageLayout layout) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            return 0;
        case VK_IMAGE_LAYOUT_GENERAL:
            return VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            return VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            return VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            return VK_ACCESS_2_TRANSFER_READ_BIT;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            return VK_ACCESS_2_TRANSFER_WRITE_BIT;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            return VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
            return 0;  // Present has a special relationship with semaphores
        default:
            return VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;  // Fallback
    }
}

// Helper function to deduce pipeline stages from layout
VkPipelineStageFlags2 stageFlagsForLayout(VkImageLayout layout) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            return VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        case VK_IMAGE_LAYOUT_GENERAL:
            return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;  // Can be used anywhere
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            return VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            return VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                   VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            return VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            // Could be any shader stage, fragment is most common.
            // For a general utility, ALL_GRAPHICS or COMPUTE is safer.
            return VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
            return VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        default:
            return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;  // Fallback
    }
}

void vulkanImgGenerateMips(VulkanCommand* cmd, VulkanImage* img) {
    VkImageMemoryBarrier barrier            = {};
    barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image                           = img->img;
    barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount     = 1;
    barrier.subresourceRange.levelCount     = 1;

    int32_t mipWidth  = img->extent.width;
    int32_t mipHeight = img->extent.height;

    for (int i = 1; i < img->mipLevels; i++) {
        // Transition previous mip to TRANSFER_SRC_OPTIMAL
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout                     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout                     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask                 = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask                 = VK_ACCESS_TRANSFER_READ_BIT;

        vkCmdPipelineBarrier(cmd->cmd,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,
0,
                              nullptr,
                              0,
                              nullptr,
                             1,
                             &barrier);

        // Blit from (i-1) → i
        VkImageBlit blit = {
            .srcSubresource = {},
            .srcOffsets[0]  = {0, 0, 0},
            .srcOffsets[1]  = {mipWidth, mipHeight, 1},
            .dstSubresource = {},
            .dstOffsets[0]  = {0, 0, 0},
            .dstOffsets[1]  = {mipWidth > 1 ? mipWidth / 2 : 1,
                               mipHeight > 1 ? mipHeight / 2 : 1,
                               1},
        };
        blit.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel       = i - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount     = 1;

        blit.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel       = i;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount     = 1;

        vkCmdBlitImage(cmd->cmd,
                       img->img,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       img->img,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1,
                       &blit,
                       VK_FILTER_LINEAR);

        // Transition mip to SHADER_READ_ONLY_OPTIMAL
        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd->cmd,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0,
0,
                              nullptr,
                              0,
                              nullptr,
                             1,
                             &barrier);

        if (mipWidth > 1) {
            mipWidth /= 2;
        }
        if (mipHeight > 1) {
            mipHeight /= 2;
        }
    }

    // Transition last mip level to SHADER_READ_ONLY_OPTIMAL
    barrier.subresourceRange.baseMipLevel = img->mipLevels - 1;
    barrier.oldLayout                     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout                     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask                 = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask                 = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd->cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0,
0,
                          nullptr,
                          0,
                          nullptr,
                          1,
                          &barrier);
    img->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void vulkanClearColorImage(VulkanCommand* cmd, VulkanImage* img, VkClearColorValue color) {
    VkImageLayout prevLayout = img->layout;
    vulkanTransition(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);
    VkImageSubresourceRange range = {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,
    };
    vkCmdClearColorImage(cmd->cmd,
                         img->img,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &color,
                         1,
                         &range);
    vulkanTransition(cmd, img, prevLayout, 0, 1);
}

void vulkanBlit(VulkanCommand* cmd, VulkanImage* source, VulkanImage* target) {
    VkImageBlit blitRegion               = {};
    blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blitRegion.srcSubresource.layerCount = 1;
    blitRegion.srcOffsets[0]             = VkOffset3D{0, 0, 0};
    blitRegion.srcOffsets[1] =
        VkOffset3D{static_cast<int32_t>(source->extent.width), static_cast<int32_t>(source->extent.height), static_cast<int32_t>(source->extent.depth)};
    blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blitRegion.dstSubresource.layerCount = 1;
    blitRegion.dstOffsets[0]             = VkOffset3D{0, 0, 0};
    blitRegion.dstOffsets[1] =
        VkOffset3D{static_cast<int32_t>(target->extent.width), static_cast<int32_t>(target->extent.height), static_cast<int32_t>(target->extent.depth)};

    vkCmdBlitImage(cmd->cmd,
                   source->img,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   target->img,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1,
                   &blitRegion,
                   VK_FILTER_LINEAR  // or VK_FILTER_NEAREST
    );
}

void vulkanCopyColorImage(VulkanCommand* cmd,
                          VulkanImage* source,
                          VulkanImage* target,
                          VkImageLayout targetFinalLayout) {
    copyImage(cmd, source, target, VK_IMAGE_ASPECT_COLOR_BIT, targetFinalLayout);
}

void vulkanCopyDepthImage(VulkanCommand* cmd,
                          VulkanImage* source,
                          VulkanImage* target,
                          VkImageLayout targetFinalLayout) {
    VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (hasStencilComponent(source->format) || hasStencilComponent(target->format)) {
        aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }

    copyImage(cmd, source, target, aspectMask, targetFinalLayout);
}

void vulkanCopyDepthToColorImage(VulkanCommand* cmd,
                                 VulkanImage* depthSource,
                                 VulkanImage* colorTarget,
                                 VkImageLayout targetFinalLayout) {
    assert(cmd);
    assert(depthSource && depthSource->img);
    assert(colorTarget && colorTarget->img);
    assert(depthSource != colorTarget);
    assert(depthSource->extent.width == colorTarget->extent.width);
    assert(depthSource->extent.height == colorTarget->extent.height);
    assert(depthSource->extent.depth == colorTarget->extent.depth);

    VkImageLayout sourceLayout = depthSource->layout;

    vulkanTransition(cmd, depthSource, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1);
    vulkanTransition(cmd, colorTarget, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);

    VkImageCopy region               = {};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    region.srcSubresource.layerCount = depthSource->layers;
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.dstSubresource.layerCount = colorTarget->layers;
    region.extent                    = depthSource->extent;

    vkCmdCopyImage(cmd->cmd,
                   depthSource->img,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   colorTarget->img,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1,
                   &region);

    vulkanTransition(cmd, depthSource, sourceLayout, 0, 1);
    vulkanTransition(cmd, colorTarget, targetFinalLayout, 0, 1);
}

static void copyImage(VulkanCommand* cmd,
                      VulkanImage* source,
                      VulkanImage* target,
                      VkImageAspectFlags aspectMask,
                      VkImageLayout targetFinalLayout) {
    assert(cmd);
    assert(source && source->img);
    assert(target && target->img);
    assert(source != target);
    assert(source->extent.width == target->extent.width);
    assert(source->extent.height == target->extent.height);
    assert(source->extent.depth == target->extent.depth);

    VkImageLayout sourceLayout = source->layout;

    vulkanTransition(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1);
    vulkanTransition(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);

    VkImageCopy region               = {};
    region.srcSubresource.aspectMask = aspectMask;
    region.srcSubresource.layerCount = source->layers;
    region.dstSubresource.aspectMask = aspectMask;
    region.dstSubresource.layerCount = target->layers;
    region.extent                    = source->extent;

    vkCmdCopyImage(cmd->cmd,
                   source->img,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   target->img,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1,
                   &region);

    vulkanTransition(cmd, source, sourceLayout, 0, 1);
    vulkanTransition(cmd, target, targetFinalLayout, 0, 1);
}

// typedef Texture {
//     Image image;
//     String name;
//     void* backendImg;
//     int id;  // also poolIndex into global vulkan texture array
//     i32 wrap;
//     i32 filter;
//     i32 refCount;
// } Texture;

void vulkanLoadTexture(Texture* texture, bool nonColor, bool genMips) {
    VulkanImage* vulkanImage = new VulkanImage{};

    VkFormat format = VK_FORMAT_UNDEFINED;
    if (texture->image.vkFormat) {
        format = static_cast<VkFormat>(texture->image.vkFormat);
    } else {
        // Determine format based on image properties
        if (texture->image.depth == 1) {  // 8bit
            if (texture->image.channels == 1) {
                format = nonColor ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8_SRGB;
            } else if (texture->image.channels == 2) {
                format = nonColor ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_R8G8_SRGB;
            } else if (texture->image.channels == 3) {
                format = nonColor ? VK_FORMAT_R8G8B8_UNORM : VK_FORMAT_R8G8B8_SRGB;
            } else if (texture->image.channels == 4) {
                format = nonColor ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8G8B8A8_SRGB;
            }
        } else if (texture->image.depth == 2) {  // 16bit
            if (texture->image.channels == 1) {
                format = nonColor ? VK_FORMAT_R16_UNORM : VK_FORMAT_R16_SNORM;
            } else if (texture->image.channels == 2) {
                format = nonColor ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R16G16_SNORM;
            } else if (texture->image.channels == 3) {
                format = nonColor ? VK_FORMAT_R16G16B16_UNORM : VK_FORMAT_R16G16B16_SNORM;
            } else if (texture->image.channels == 4) {
                format = nonColor ? VK_FORMAT_R16G16B16A16_UNORM : VK_FORMAT_R16G16B16A16_SNORM;
            }
        }
    }

    // Check if image has pre-generated mipmaps
    bool hasMipData = texture->image.mips > 1;

    // Determine mip levels for the Vulkan image
    int mipLevels           = 1;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    if (hasMipData) {
        // Use pre-generated mipmaps from the image file
        mipLevels = texture->image.mips;
    } else if (genMips) {
        // Generate mipmaps at runtime
        mipLevels = static_cast<int>(std::log2(std::max(texture->image.width, texture->image.height)) + 1);
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // Need transfer src for mipmap
                                                   // generation
    }

*vulkanImage = vulkanCreateImage(.name      = texture->name.data,
                                  .format    = format,
                                  .usage     = usage,
                                  .width     = texture->image.width,
                                  .height    = texture->image.height,
                                  .mipLevels = mipLevels,
                                  .onHeap    = true);

    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanTransition(cmd, vulkanImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);

    if (hasMipData) {
        VulkanBuffer staging = vulkanCreateStagingBuffer(texture->image.size);
        memcpy(static_cast<char*>(staging.vmaInfo.pMappedData), texture->image.data, texture->image.size);

        std::vector<VkBufferImageCopy> regions = {};

        for (i32 i = 0; i < texture->image.mips; i++) {
            i32 mipWidth  = texture->image.width >> i;
            i32 mipHeight = texture->image.height >> i;

            // Skip mip levels with zero extent (Vulkan doesn't allow empty copies)
            if (mipWidth == 0 || mipHeight == 0) {
                break;
            }

            VkBufferImageCopy region           = {};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageSubresource.mipLevel   = i;
            region.imageExtent.width           = mipWidth;
            region.imageExtent.height          = mipHeight;
            region.imageExtent.depth           = 1;
            region.bufferOffset                = texture->image.mipSizes[i];
            regions.push_back(region);
        }
        vkCmdCopyBufferToImage(cmd->cmd,
                               staging.buf,
                               vulkanImage->img,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<i32>(regions.size()),
                               regions.data());
        addBufferGarbage(&staging, cmd->fence, &cmd->submitted);
        vulkanTransition(cmd, vulkanImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    } else {
        vulkanCopy(.cmd         = cmd,
                   .target.img  = vulkanImage,
                   .source.data = texture->image.data,
                   .size        = static_cast<u32>(texture->image.size));
        if (genMips) {
            vulkanImgGenerateMips(cmd, vulkanImage);
        } else {
            vulkanTransition(cmd, vulkanImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        }
    }

    vulkanTransientEnd(cmd, 1);

    utils::imageDestory(&texture->image);

    texture->backendImg = vulkanImage;
    texture->id         = vulkanImage->sampledPoolIndex;
}
}  // namespace engine
