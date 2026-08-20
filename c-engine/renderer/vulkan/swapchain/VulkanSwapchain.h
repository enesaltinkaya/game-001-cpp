#pragma once

#include "renderer/vulkan/resources/VulkanImage.h"
namespace engine {
struct VulkanImage;

struct VulkanSwapchain {
    VkFormat swapchainImageFormat;

    struct VulkanImage* currentSwapchainImage;
    u32 imageCount;

    double cpuElapsed;
    bool vsync;
};

extern struct VulkanSwapchain vulkanSwapchain;

void vulkanSwapchainInit(void);
void vulkanSwapchainDestroy(void);
void vulkanSwapchainBegin(void);
void vulkanSwapchainEnd(void);
// Wait on the flight fence of the frame just submitted by vulkanSwapchainEnd
// (flightIndex only advances on the next postUpdate), so callers can
// synchronously consume readbacks recorded into that frame's command buffer.
void vulkanSwapchainWaitCurrentFlight(void);
void vulkanSwapchainRecreate(void);
}  // namespace engine
