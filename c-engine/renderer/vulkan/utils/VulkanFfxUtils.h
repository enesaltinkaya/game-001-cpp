#pragma once
#include "renderer/vulkan/resources/VulkanBuffer.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <FidelityFX/host/ffx_interface.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#pragma GCC diagnostic pop

namespace engine {
/* The FFX backend needs the VkImageCreateInfo it was created with, but
 * VulkanImage does not store the create flags/type, so synthesize them here:
 *  - 3D images: r_vulkanCreateImg stores the 3D size in extent and leaves
 *    layers set, so extent.depth > 1 means 3D (arrayLayers is forced to 1).
 *  - cube views: the FFX backend keys FFX_RESOURCE_TYPE_TEXTURE_CUBE (→
 *    VK_IMAGE_VIEW_TYPE_CUBE) on VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; without
 *    the flag a 6-layer array is wrapped as a plain 2D array and textureCube
 *    bindings break (plan pitfall #14 — the env-cube map in Step 6 relies on
 *    this). */
static inline VkImageCreateInfo vulkanFfxMakeImageCreateInfo(VulkanImage* image) {
    VkImageCreateInfo info = {};
    info.sType             = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.format            = image->format;
    info.mipLevels         = (u32)image->mipLevels;
    info.samples           = image->samples;
    info.tiling            = VK_IMAGE_TILING_OPTIMAL;
    info.usage             = image->usage;
    info.flags             = (image->viewType == VK_IMAGE_VIEW_TYPE_CUBE) ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT
                                                                           : 0;
    info.sharingMode       = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    if (image->extent.depth > 1) {
        info.imageType     = VK_IMAGE_TYPE_3D;
        info.extent        = image->extent;
        info.arrayLayers   = 1;
    } else {
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.extent        = {image->extent.width, image->extent.height, 1};
        info.arrayLayers   = (u32)image->layers;
    }
    return info;
}

static inline FfxResource vulkanFfxWrapImageResource(VulkanImage* image,
                                                     FfxResourceUsage usage,
                                                     FfxResourceStates state,
                                                     const wchar_t* name) {
    VkImageCreateInfo createInfo = vulkanFfxMakeImageCreateInfo(image);
    FfxResourceDescription desc = ffxGetImageResourceDescriptionVK(image->img, createInfo, usage);
    return ffxGetResourceVK(image->img, desc, name, state);
}

/* VulkanBuffer does not store its create-time usage, so the caller passes it:
 * the FFX VK backend derives FFX_RESOURCE_USAGE_UAV from
 * VK_BUFFER_USAGE_STORAGE_BUFFER_BIT — FFX binds every buffer SRV as a shader
 * storage buffer, so vertex/index data must be created with STORAGE usage
 * (plan pitfall #13). */
static inline FfxResource vulkanFfxWrapBufferResource(VulkanBuffer* buffer,
                                                      VkBufferUsageFlags usage,
                                                      FfxResourceStates state,
                                                      const wchar_t* name,
                                                      FfxResourceUsage additionalUsages = FFX_RESOURCE_USAGE_READ_ONLY) {
    VkBufferCreateInfo createInfo = {};
    createInfo.size               = (VkDeviceSize)buffer->size;
    createInfo.usage              = usage;
    FfxResourceDescription desc   = ffxGetBufferResourceDescriptionVK(buffer->buf, createInfo, additionalUsages);
    return ffxGetResourceVK(buffer->buf, desc, name, state);
}
}  // namespace engine