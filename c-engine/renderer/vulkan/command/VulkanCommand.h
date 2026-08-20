#pragma once

#include <atomic>

#include "renderer/vulkan/resources/VulkanBuffer.h"
struct VulkanImage;
struct VulkanPipe;

struct VulkanCommand {
    VkCommandBuffer cmd;
    VkFence fence;
    VkCommandPool pool;  // associated pool if it is transient
    bool transient;
    std::atomic<bool> submitted;
};

struct VulkanSubmitInfo {
    VkSemaphore wait = VK_NULL_HANDLE;
    VkSemaphore signal = VK_NULL_HANDLE;
    struct VulkanCommand* cmd = nullptr;
    VkShaderStageFlags stageFlags = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
};

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

#define vulkanSubmit(...) r_vulkanSubmit(VulkanSubmitInfo{__VA_ARGS__})
void r_vulkanSubmit(VulkanSubmitInfo info);
void vulkanPresent(VkSwapchainKHR* pSwapchains, u32* pImageIndices, VkSemaphore* pWaitSemaphore);
void vulkanWaitIdle(const char* reason);
