#pragma once

#include "thread/Thread.h"

struct VulkanDesc {
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkDescriptorSetLayout layout;
    struct Thread lock;
};

struct VulkanCommand;
struct VulkanImage;
struct VulkanPipe;

enum VulkanDescType {
    VULKAN_BINDING_INLINE_UNIFORM_BLOCK   = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK,
    VULKAN_BINDING_SAMPLER                = VK_DESCRIPTOR_TYPE_SAMPLER,
    VULKAN_BINDING_SAMPLED_IMAGE          = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
    VULKAN_BINDING_COMBINED_IMAGE_SAMPLER = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    VULKAN_BINDING_STORAGE_IMAGE          = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    VULKAN_BINDING_SSBO                   = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    VULKAN_BINDING_UBO                    = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
};

struct VulkanDescInfo {
    const char* name = nullptr;
    int samplers = 0;
    int sampledImages = 0;
    int sampledCubeImages = 0;
    int sampledImageLayered = 0;
    int combinedImageSamplers = 0;
    int storageImages = 0;
    int ssbos = 0;
    int ubos = 0;
};

#define vulkanCreateDesc(...) r_vulkanCreateDesc(VulkanDescInfo{__VA_ARGS__})
struct VulkanDesc r_vulkanCreateDesc(struct VulkanDescInfo info);
void vulkanDestroyDesc(struct VulkanDesc* desc);

void vulkanUpdateDesc(struct VulkanDesc* desc, enum VulkanDescType type, void* resource, int dstBinding, int dstArrayElement);
void vulkanBindDesc(struct VulkanCommand* cmd, struct VulkanPipe* pipe, struct VulkanDesc* desc, int firstSet);
