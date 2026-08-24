#include "VulkanError.h"
#include <atomic>
#include "../command/VulkanCommand.h"
#include "../swapchain/VulkanSwapchain.h"
#include "Utils.h"
#include "logger/Logger.h"

namespace engine {
static VulkanDeviceLostHandler deviceLostHandler = nullptr;
static std::atomic<char> deviceLostFlag{0};

void vulkanSetDeviceLostHandler(VulkanDeviceLostHandler handler) {
    deviceLostHandler = handler;
}

int vulkanCheckQueueError(VkResult result, const char* op) {
    if (result == VK_SUCCESS) {
        return 0;
    }

    // Normal cases that don't require recovery
    if (result == VK_SUBOPTIMAL_KHR) {
        utils::warn("vulkanCheckQueueError: %s → %d (suboptimal, continuing)", op, result);
        return 0;
    }
    if (result == VK_TIMEOUT) {
        return 0; // Will retry next frame
    }
    if (result == VK_NOT_READY) {
        return 0; // Will retry next frame
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        utils::warn("vulkanCheckQueueError: %s → %d (swapchain out of date)", op, result);
        vulkan.skipFrame = 1;
        vulkanSwapchainRecreate();
        return 0;
    }

    // Device lost — the driver crashed
    if (result == VK_ERROR_DEVICE_LOST) {
        if (deviceLostFlag.exchange(1)) {
            // Already handled once
            return 1;
        }
        utils::error("VK_ERROR_DEVICE_LOST detected during %s!", op);
        utils::error("GPU driver has crashed. Attempting graceful shutdown.");
        if (deviceLostHandler) {
            deviceLostHandler();
        }
        // Trigger engine shutdown
        extern volatile char engineRunning;
        engineRunning = 0;
        return 1;
    }

    // Other errors — may indicate driver instability
    utils::error("vulkanCheckQueueError: %s → %d (unexpected)", op, result);
    return 1;
}
}  // namespace engine
