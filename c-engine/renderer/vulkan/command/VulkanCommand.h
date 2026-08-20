#pragma once

#include "renderer/vulkan/resources/VulkanBuffer.h"
struct VulkanImage;
struct VulkanPipe;

typedef struct VulkanCommand {
    VkCommandBuffer cmd;
    VkFence fence;
    VkCommandPool pool;  // associated pool if it is transient
    char transient;
    _Atomic bool submitted;
} VulkanCommand;

typedef struct VulkanSubmitInfo {
    VkSemaphore wait;
    VkSemaphore signal;
    struct VulkanCommand* cmd;
    VkShaderStageFlags stageFlags;
} VulkanSubmitInfo;

void vulkanBegin(VulkanCommand* cmd);
void vulkanEnd(VulkanCommand* cmd);
void vulkanFenceWait(VulkanCommand* cmd);
void vulkanReset(VulkanCommand* cmd);

void vulkanLabelBeginColor(VulkanCommand* cmd,
                           const char* name,
                           float r,
                           float g,
                           float b,
                           float a);
void vulkanLabelBegin(VulkanCommand* cmd, const char* name);
void vulkanLabelEnd(VulkanCommand* cmd);

#define vulkanSubmit(...) \
    r_vulkanSubmit(       \
        (VulkanSubmitInfo){.stageFlags = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, __VA_ARGS__})
void r_vulkanSubmit(VulkanSubmitInfo info);
void vulkanPresent(VkSwapchainKHR* pSwapchains, u32* pImageIndices, VkSemaphore* pWaitSemaphore);
void vulkanWaitIdle(const char* reason);
