#pragma once

#include "../Vulkan.h"

/**
 * VK_CHECK — assert on success, otherwise log and (in debug) trap.
 *
 * Usage:
 *     VK_CHECK(vkSomeCall(...), "vkSomeCall description");
 */
#ifdef NDEBUG
#define VK_CHECK(result, desc)                                               \
    do {                                                                     \
        VkResult _r = (result);                                              \
        if (_r != VK_SUCCESS) {                                              \
            utils::error("VK_CHECK failed: %s → %d", desc, _r);                     \
        }                                                                    \
    } while (0)
#else
#define VK_CHECK(result, desc)                                               \
    do {                                                                     \
        VkResult _r = (result);                                              \
        if (_r != VK_SUCCESS) {                                              \
            utils::terminate("VK_CHECK failed: %s → %d", desc, _r);                     \
        }                                                                    \
    } while (0)
#endif

/**
 * VK_CHECK_WARN — log a warning on failure but do not trap.
 *
 * Use for operations that may legitimately fail (e.g.
 * vkAcquireNextImageKHR → VK_NOT_READY, vkQueuePresentKHR errors).
 */
#define VK_CHECK_WARN(result, desc)                                          \
    do {                                                                     \
        VkResult _r = (result);                                              \
        if (_r != VK_SUCCESS && _r != VK_SUBOPTIMAL_KHR &&                   \
            _r != VK_TIMEOUT && _r != VK_NOT_READY) {                        \
            utils::warn("VK_CHECK_WARN: %s → %d", desc, _r);                        \
        }                                                                    \
    } while (0)

/**
 * Check for device loss after a queue operation.
 *
 * Returns 1 if the device is now lost (driver crashed), 0 otherwise.
 * On device loss, calls the deviceLost handler if registered.
 */
namespace engine {
typedef void (*VulkanDeviceLostHandler)(void);
void vulkanSetDeviceLostHandler(VulkanDeviceLostHandler handler);

/**
 * vulkanCheckQueueError — check the result of a queue submit / present and
 * handle device loss.
 *
 * Returns 0 on success, 1 if device is lost (caller should skip the frame
 * and trigger recovery).
 */
int vulkanCheckQueueError(VkResult result, const char* op);
}  // namespace engine
