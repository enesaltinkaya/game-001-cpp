#pragma once

#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/resources/VulkanBuffer.h"
#include "renderer/vulkan/resources/VulkanImage.h"

namespace engine {
typedef struct VulkanImage VulkanImage;

void vulkanUtilsSetName(u64 objectHandle, VkObjectType type, const char* name);
unsigned int vulkanValidationLog(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                              VkDebugUtilsMessageTypeFlagsEXT messageType,
                              const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                              void* _);
// Swapchain screenshot capture, split around vulkanSwapchainEnd: the readback
// copy must be recorded into the ACTIVE frame command buffer before present
// releases the image (a separate submit touching a released presentable image
// is a spec violation), then the CPU write-out runs after the flight fence
// has signalled.
void vulkanScreenshotRecord(VulkanImage* swapImg, VulkanBuffer* outReadback);
void vulkanScreenshotWriteJpg(const VulkanBuffer* readback, const VulkanImage* swapImg,
                              const char* path);
VkFormat vulkanFindSupportedFormat(const VkFormat* candidates,
                                   int count,
                                   VkImageTiling tiling,
                                   VkFormatFeatureFlags features);

// Save a VulkanImage to disk as HDR (.hdr) or JPG (.jpg).
// For float formats, .hdr is recommended.
// The image is transitioned to TRANSFER_SRC, copied to a readback buffer,
// and written to the given path.
void vulkanSaveImage(VulkanImage* img, const char* path);
// Raw readback of the image (unmodified bytes), for bit-pattern analysis.
void vulkanSaveImageRaw(VulkanImage* img, const char* path);

#ifndef NDEBUG
#ifdef __linux__
void* initRenderDocAPI(void);
void captureFrameRenderDoc(void);
void vulkanRenderDocCaptureNow(void); /* immediate TriggerCapture (no delay) */
#endif
#endif
}  // namespace engine
