#include "Utils.h"
#include "../Vulkan.h"
#include <float.h>
#include "../barrier/VulkanBarrier.h"
#include "../command/VulkanCommand.h"
#include "../resources/VulkanBuffer.h"
#include "../resources/VulkanResourceManager.h"
#include "ecs/system/window/WindowSystem.h"
#include "futuretask/FutureTask.h"
#include "logger/Logger.h"
#include "timer/Timer.h"

namespace engine {
static const char* vulkanToStringMessageSeverity(
    VkDebugUtilsMessageSeverityFlagBitsEXT s);
static const char* vulkanToStringMessageType(VkDebugUtilsMessageTypeFlagsEXT s);

unsigned int vulkanValidationLog(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* _) {
    if (messageSeverity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        utils::debug("%llu %s", utils::timer.frameCounter, pCallbackData->pMessage);
        return VK_FALSE;
    }

    const char* severity = vulkanToStringMessageSeverity(messageSeverity);
    const char* type     = vulkanToStringMessageType(messageType);

    // Only genuine validation ERRORS are fatal: performance-type notes (e.g.
    // unused vertex attribute locations) fire at pipeline creation on every
    // run and are informational only. Warnings are logged, not aborted.
    if (messageSeverity != VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        utils::warn("%s - %s: %s [frame %llu]", severity, type, pCallbackData->pMessage, utils::timer.frameCounter);
        return VK_FALSE;
    }

    utils::terminate("---------\n%s - %s\n%s\nframeCounter: %llu\n",
              severity,
              type,
              pCallbackData->pMessage,
              utils::timer.frameCounter);
    return VK_FALSE;
}

static const char* vulkanToStringMessageSeverity(
    VkDebugUtilsMessageSeverityFlagBitsEXT s) {
    switch (s) {
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
            return "VERBOSE";
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
            return "ERROR";
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
            return "WARNING";
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
            return "INFO";
        default:
            return "UNKNOWN";
    }
}

static const char* vulkanToStringMessageType(
    VkDebugUtilsMessageTypeFlagsEXT s) {
    if (s == 7) {
        return "General | Validation | Performance";
    }
    if (s == 6) {
        return "Validation | Performance";
    }
    if (s == 5) {
        return "General | Performance";
    }
    if (s == 4 /*VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT*/) {
        return "Performance";
    }
    if (s == 3) {
        return "General | Validation";
    }
    if (s == 2 /*VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT*/) {
        return "Validation";
    }
    if (s == 1 /*VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT*/) {
        return "General";
    }
    return "Unknown";
}

void vulkanUtilsSetName(u64 objectHandle, VkObjectType type, const char* name) {
    VkDebugUtilsObjectNameInfoEXT nameInfo = {};
    nameInfo.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    nameInfo.objectType   = type;
    nameInfo.objectHandle = objectHandle;
    nameInfo.pObjectName  = name;
    vkSetDebugUtilsObjectNameEXT(vulkan.device, &nameInfo);
}

VkFormat vulkanFindSupportedFormat(const VkFormat* candidates,
                                   int count,
                                   VkImageTiling tiling,
                                   VkFormatFeatureFlags features) {
    for (i32 i = 0, s = count; i < s; i++) {
        VkFormat format = candidates[i];
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(vulkan.physicalDevice,
                                            format,
                                            &props);

        if (tiling == VK_IMAGE_TILING_LINEAR &&
            (props.linearTilingFeatures & features) == features) {
            return format;
        }
        if (tiling == VK_IMAGE_TILING_OPTIMAL &&
            (props.optimalTilingFeatures & features) == features) {
            return format;
        }
    }

    utils::terminate("Failed to find supported depth format!");
    return VK_FORMAT_UNDEFINED;
}

// Phase 1 of a swapchain screenshot: record the readback copy into the
// caller's ACTIVE command buffer (the frame's flight buffer, still open).
// Must run before vulkanSwapchainEnd/present — once present releases the
// image, any layout transition or copy on it from a separate submit is a
// spec violation (use of a not-acquired presentable image). The image is
// handed back to PRESENT_SRC so vulkanSwapchainEnd's own transition is a
// no-op.
void vulkanScreenshotRecord(VulkanImage* swapImg, VulkanBuffer* outReadback) {
    *outReadback = VulkanBuffer{};
    if (!swapImg || !swapImg->img) {
        utils::warn("vulkanScreenshotRecord: no swapchain image available");
        return;
    }

    u32 w = swapImg->extent.width;
    u32 h = swapImg->extent.height;
    u64 bufSize = (u64)w * h * 4;

    *outReadback = vulkanCreateReadbackBuffer("screenshot", bufSize, 0);
    VulkanCommand* cmd = vulkan.currentCmd;

    vulkanTransition(cmd, swapImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1);
    vulkanCopy(.cmd      = cmd,
               .source.img = swapImg,
               .target.buf = outReadback);
    vulkanBarrier(cmd, DEVICE_WRITE_TO_HOST_READ);
    vulkanTransition(cmd, swapImg, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, 1);
}

// Phase 2 of a swapchain screenshot: write the recorded readback (BGRA8) to
// disk. Only call after the flight fence has signalled.
void vulkanScreenshotWriteJpg(const VulkanBuffer* readback, const VulkanImage* swapImg,
                              const char* path) {
    if (!readback || !readback->buf) {
        utils::warn("vulkanScreenshotWriteJpg: no readback buffer");
        return;
    }

    u32 w = swapImg->extent.width;
    u32 h = swapImg->extent.height;

    // Swizzle BGRA -> RGBA in-place
    u8* pixels = (u8*)readback->vmaInfo.pMappedData;
    for (u64 i = 0; i < (u64)w * h * 4; i += 4) {
        u8 tmp      = pixels[i];
        pixels[i]   = pixels[i + 2];
        pixels[i + 2] = tmp;
    }

    if (stbi_write_jpg(path, (int)w, (int)h, 4, pixels, 80)) {
        utils::info("vulkanScreenshot: saved %s (%ux%u)", path, w, h);
    } else {
        utils::warn("vulkanScreenshot: failed to write %s", path);
    }
}

static bool isFloatFormat(VkFormat fmt) {
    switch (fmt) {
        case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_R32G32_SFLOAT:
        case VK_FORMAT_R32G32B32_SFLOAT:
        case VK_FORMAT_R32G32B32A32_SFLOAT:
        case VK_FORMAT_R16_SFLOAT:
        case VK_FORMAT_R16G16_SFLOAT:
        case VK_FORMAT_R16G16B16_SFLOAT:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_D32_SFLOAT:
            return true;
        default:
            return false;
    }
}

static int formatChannelCount(VkFormat fmt) {
    switch (fmt) {
        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_R16_SFLOAT:
        case VK_FORMAT_D32_SFLOAT:
            return 1;
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R32G32_SFLOAT:
        case VK_FORMAT_R16G16_SFLOAT:
            return 2;
        case VK_FORMAT_R8G8B8_UNORM:
        case VK_FORMAT_R32G32B32_SFLOAT:
        case VK_FORMAT_R16G16B16_SFLOAT:
            return 3;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_R32G32B32A32_SFLOAT:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return 4;
        default:
            return 4;
    }
}

void vulkanSaveImage(VulkanImage* img, const char* path) {
    if (!img || !img->img) {
        utils::warn("vulkanSaveImage: null image");
        return;
    }

    u32 w = img->extent.width;
    u32 h = img->extent.height;
    int channels = formatChannelCount(img->format);
    u64 pixelSize = isFloatFormat(img->format) ? (u64)channels * sizeof(float) : (u64)channels;
    u64 bufSize = (u64)w * h * pixelSize;

    VulkanBuffer readback = vulkanCreateReadbackBuffer("saveImage", bufSize, 0);

    VkImageLayout prevLayout = img->layout;

    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanTransition(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1);
    vulkanCopy(.cmd = cmd,
               .source.img = img,
               .target.buf = &readback);
    vulkanBarrier(cmd, DEVICE_WRITE_TO_HOST_READ);
    vulkanTransition(cmd, img, prevLayout, 0, 1);
    vulkanTransientEnd(cmd, 1);

    void* pixels = readback.vmaInfo.pMappedData;

    int ok;
    if (isFloatFormat(img->format)) {
        // Float image: find min/max per channel, remap to 0-255 PNG
        float* src = (float*)pixels;
        u64 numPixels = (u64)w * h;

        float minV[4] = {FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX};
        float maxV[4] = {-FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (u64 i = 0; i < numPixels; i++) {
            for (int c = 0; c < channels; c++) {
                float v = src[i * channels + c];
                if (v < minV[c]) minV[c] = v;
                if (v > maxV[c]) maxV[c] = v;
            }
        }

        utils::info("vulkanSaveImage: float range ch0=[%.4f,%.4f] ch1=[%.4f,%.4f] ch2=[%.4f,%.4f] ch3=[%.4f,%.4f]",
             minV[0], maxV[0], minV[1], maxV[1], minV[2], maxV[2], minV[3], maxV[3]);

        // Remap each channel: [min, max] -> [0, 255]
        u8* jpg = static_cast<u8*>(malloc(numPixels * channels));
        for (u64 i = 0; i < numPixels; i++) {
            for (int c = 0; c < channels; c++) {
                float v = src[i * channels + c];
                float range = maxV[c] - minV[c];
                float norm = range > 1e-6f ? (v - minV[c]) / range : 0.0f;
                jpg[i * channels + c] = (u8)(norm * 255.0f + 0.5f);
            }
        }
        ok = stbi_write_jpg(path, (int)w, (int)h, channels, jpg, 80);
        free(jpg);
    } else {
        // BGRA swizzle for swapchain-like formats
        if (img->format == VK_FORMAT_B8G8R8A8_UNORM) {
            u8* p = (u8*)pixels;
            for (u64 i = 0; i < bufSize; i += 4) {
                u8 tmp  = p[i];
                p[i]    = p[i + 2];
                p[i + 2] = tmp;
            }
        }
        ok = stbi_write_jpg(path, (int)w, (int)h, channels, pixels, 80);
    }

    if (ok) {
        utils::info("vulkanSaveImage: saved %s (%ux%u, fmt=%d, ch=%d)", path, w, h, img->format, channels);
    } else {
        utils::warn("vulkanSaveImage: failed to write %s", path);
    }

    vulkanDestroyBuffer(&readback, nullptr);
}

#ifndef NDEBUG
#ifdef __linux__
#include "renderdoc_app.h"
#include <dlfcn.h>
static RENDERDOC_API_1_1_2* rdoc_api = nullptr;

void* initRenderDocAPI(void) {
    if (rdoc_api) {
        return rdoc_api;
    }

    void* module = nullptr;
    module       = dlopen("librenderdoc.so", RTLD_NOLOAD | RTLD_NOW);
    if (module) {
        pRENDERDOC_GetAPI RENDERDOC_GetAPI =
            (pRENDERDOC_GetAPI)dlsym(module, "RENDERDOC_GetAPI");
        if (RENDERDOC_GetAPI) {
            int ret = RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_1_2,
                                       (void**)&rdoc_api);
            if (ret == 1) {
                printf("RenderDoc API initialized!\n");
            } else {
                printf("Failed to get RenderDoc API.\n");
            }
        }
    } else {
        printf("RenderDoc not present.\n");
    }
    return rdoc_api;
}

static void doCapture(void* pRenderDoc) {
    RENDERDOC_API_1_1_2* renderDoc  = static_cast<RENDERDOC_API_1_1_2*>(pRenderDoc);
    renderDoc->TriggerCapture();
}

void captureFrameRenderDoc(void) {
    RENDERDOC_API_1_1_2* renderDoc  = static_cast<RENDERDOC_API_1_1_2*>(initRenderDocAPI());
    if (renderDoc) {
        utils::futureTaskAdd(1000, doCapture, renderDoc);
    }
}
#endif
#endif
}  // namespace engine
