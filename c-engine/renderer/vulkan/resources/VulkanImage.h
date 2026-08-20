#pragma once

typedef struct VulkanImage {
    String name;
    VmaAllocation vma;
    VkImage img;
    VkImageView view;  // still needed for texture array, eg. shadow cascades
    VkImageView* views;
    VkImageAspectFlags aspect;
    VkImageLayout layout;
    VkExtent3D extent;
    VkFormat format;
    VkImageUsageFlags usage;
    VkImageViewType viewType;
    VkSampleCountFlagBits samples;

    int layers;
    int mipLevels;
    int sampledPoolIndex;
    int storagePoolIndex;
    char inPool;
    char onHeap;
} VulkanImage;

typedef struct VulkanImageInfo {
    const char* name;
    VkFormat format;
    VkImageUsageFlags usage;
    VkImageAspectFlags aspect;
    VkImageType type;
    VkImageCreateFlags flags;
    VkImageViewType viewType;
    int width;
    int height;
    int dedicated;
    int layers;
    int mipLevels;
    int samples;
    char isArray;
    char noPool;
    char onHeap;
} VulkanImageInfo;

#define vulkanCreateImage(...)                                                     \
    r_vulkanCreateImg((struct VulkanImageInfo){                                    \
        .format    = VK_FORMAT_R8G8B8A8_UNORM,                                     \
        .usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, \
        .aspect    = VK_IMAGE_ASPECT_COLOR_BIT,                                    \
        .layers    = 1,                                                            \
        .mipLevels = 1,                                                            \
        .samples   = VK_SAMPLE_COUNT_1_BIT,                                        \
        .viewType  = VK_IMAGE_VIEW_TYPE_2D,                                        \
        __VA_ARGS__})

typedef struct VulkanCommand VulkanCommand;
typedef struct Texture Texture;

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
                                 
void vulkanLoadTexture(Texture* texture, char nonColor, char genMips);
