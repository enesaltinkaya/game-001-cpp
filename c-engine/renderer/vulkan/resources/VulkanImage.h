#pragma once
#include <vector>

namespace engine {
struct VulkanImage {
    utils::String name = {};
    VmaAllocation vma = {};
    VkImage img = {};
    VkImageView view = {};  // still needed for texture array, eg. shadow cascades
    std::vector<VkImageView> views = {};
    VkImageAspectFlags aspect = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkExtent3D extent = {};
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage = 0;
    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
    VkSampleCountFlagBits samples = static_cast<VkSampleCountFlagBits>(0);

    int layers = 0;
    int mipLevels = 0;
    int sampledPoolIndex = 0;
    int storagePoolIndex = 0;
    bool inPool = false;
    bool onHeap = false;
};

struct VulkanImageInfo {
    const char* name = nullptr;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkImageCreateFlags flags = 0;
    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
    int width = 0;
    int height = 0;
    int dedicated = 0;
    int layers = 1;
    int mipLevels = 1;
    int samples = VK_SAMPLE_COUNT_1_BIT;
    bool isArray = false;
    bool noPool = false;
    bool onHeap = false;
};

#define vulkanCreateImage(...) engine::r_vulkanCreateImg(engine::VulkanImageInfo{__VA_ARGS__})

struct VulkanCommand;
struct Texture;

VulkanImage r_vulkanCreateImg(VulkanImageInfo info);
void vulkanDestroyImage(VulkanImage* img, VkFence fence);
void vulkanTransition(VulkanCommand* cmd,
                      VulkanImage* img,
                      VkImageLayout newLayout,
                      int baseLayer,
                      int layerCount);
void vulkanClearColorImage(VulkanCommand* cmd, VulkanImage* img, VkClearColorValue color);
void vulkanImgGenerateMips(VulkanCommand* cmd, VulkanImage* img);
void vulkanBlit(VulkanCommand* cmd, VulkanImage* source, VulkanImage* target);
void vulkanCopyColorImage(VulkanCommand* cmd,
                          VulkanImage* source,
                          VulkanImage* target,
                          VkImageLayout targetFinalLayout);
void vulkanCopyDepthImage(VulkanCommand* cmd,
                          VulkanImage* source,
                          VulkanImage* target,
                          VkImageLayout targetFinalLayout);
void vulkanCopyDepthToColorImage(VulkanCommand* cmd,
                                 VulkanImage* depthSource,
                                 VulkanImage* colorTarget,
                                 VkImageLayout targetFinalLayout);
                                 
void vulkanLoadTexture(Texture* texture, bool nonColor, bool genMips);
}  // namespace engine
