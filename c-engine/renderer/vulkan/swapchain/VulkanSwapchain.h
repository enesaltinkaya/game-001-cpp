#pragma once

#include "renderer/vulkan/resources/VulkanImage.h"
struct VulkanImage;

typedef struct VulkanSwapchain {
    VkFormat swapchainImageFormat;

    struct VulkanImage* currentSwapchainImage;
    u32 imageCount;

    double cpuElapsed;
    char vsync;
} VulkanSwapchain;

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
