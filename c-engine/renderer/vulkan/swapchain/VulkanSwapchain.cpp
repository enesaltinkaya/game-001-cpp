#include "VulkanSwapchain.h"
#include "../Vulkan.h"
#include "../command/VulkanCommand.h"
#include "../resources/VulkanImage.h"
#include "../utils/VulkanError.h"
#include "../utils/VulkanUtils.h"
#include "Utils.h"
#include "ecs/system/window/WindowSystem.h"
#include "events/Events.h"
#include "logger/Logger.h"
#include "renderer/Renderer.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "settings/Settings.h"

struct FlightItem {
    VkCommandPool pool;
    struct VulkanCommand cmd;
    VkSemaphore acquired;
};

static Array(struct FlightItem) flightItems;

static void createSwapchain(void);
static void recreateSwapchain(void);

static void createPerFlightItems(void);

struct VulkanSwapchain vulkanSwapchain;
static VkSwapchainKHR swapchain;
static Array(struct VulkanImage) swapchainImages;
static Array(VkSemaphore) submitSemaphores;
static u32 swapchainImageIndex = 0;
static double swapchainElapsed;

static void resized(void*) {
    vulkan.skipFrame = 1;
}

void vulkanSwapchainInit(void) {
    createSwapchain();
    createPerFlightItems();
    signalSubscribe("windowResized", resized);
}

void vulkanSwapchainDestroy(void) {
    vulkan.currentCmd = nullptr;
    for (i32 i = 0, si = FRAMES_IN_FLIGHT; i < si; i++) {
        struct FlightItem* item = &flightItems[i];
        vkDestroyCommandPool(vulkan.device, item->pool, nullptr);
        vkDestroyFence(vulkan.device, item->cmd.fence, nullptr);
        vkDestroySemaphore(vulkan.device, item->acquired, nullptr);

        item->cmd.cmd = nullptr;
    }
    arrayFree(flightItems);

    foreach (VkSemaphore semaphore, submitSemaphores) {
        vkDestroySemaphore(vulkan.device, semaphore, nullptr);
    }
    arrayFree(submitSemaphores);

    foreachptr(struct VulkanImage * img, swapchainImages) {
        vkDestroyImageView(vulkan.device, img->view, nullptr);
    }

    vkDestroySwapchainKHR(vulkan.device, swapchain, nullptr);
    arrayFree(swapchainImages);
}

void vulkanSwapchainBegin(void) {
    if (vulkan.skipFrame) {
        return;
    }

    double elapsed = elapsedBegin();

    struct FlightItem* item = &flightItems[renderer.flightIndex];
    vulkan.currentCmd       = &item->cmd;

    {
        static int hitchOn = -1;
        if (hitchOn < 0) hitchOn = getenv("ENGINE_HITCH_DEBUG") != nullptr;
        double tw0 = nanos();
        vulkanFenceWait(&item->cmd);
        double twms = (nanos() - tw0) / 1e6;
        if (hitchOn && twms > 8.0) info("HITCH: flight fence wait %.1f ms", twms);
    }
    vulkanReset(&item->cmd);

    VkResult result = vkAcquireNextImageKHR(vulkan.device,
                                            swapchain,
                                            UINT64_MAX,
item->acquired,
                                             nullptr,
                                             &swapchainImageIndex);

    if (vulkanCheckQueueError(result, "vkAcquireNextImageKHR")) {
        vulkan.skipFrame = 1;
    }

    vulkanSwapchain.currentSwapchainImage = &swapchainImages[swapchainImageIndex];

    VK_CHECK(vkResetCommandPool(vulkan.device, item->pool, 0),
             "vkResetCommandPool");
    vulkanBegin(&item->cmd);
    if (!vulkan.skipFrame) {
        vulkanTransition(vulkan.currentCmd,
                         vulkanSwapchain.currentSwapchainImage,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         0,
                         1);
    }
    swapchainElapsed = elapsedEnd(elapsed);
}


void vulkanSwapchainEnd(void) {
    if (vulkan.skipFrame) {
        recreateSwapchain();
        return;
    }

    double elapsed = elapsedBegin();

    struct FlightItem* item = &flightItems[renderer.flightIndex];
    vulkanTransition(&item->cmd,
                     vulkanSwapchain.currentSwapchainImage,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     0,
                     1);
    vulkanEnd(&item->cmd);

    vulkanSubmit(.wait   = item->acquired,
                 .signal = submitSemaphores[swapchainImageIndex],
                 .cmd    = &item->cmd);

    vulkanPresent(&swapchain, &swapchainImageIndex, &submitSemaphores[swapchainImageIndex]);

    swapchainElapsed += elapsedEnd(elapsed);
    vulkanSwapchain.cpuElapsed = swapchainElapsed;
}

void vulkanSwapchainWaitCurrentFlight(void) {
    struct FlightItem* item = &flightItems[renderer.flightIndex];
    VK_CHECK(vkWaitForFences(vulkan.device, 1, &item->cmd.fence, VK_TRUE, UINT64_MAX),
             "vkWaitForFences (screenshot flight)");
}

void createSwapchain(void) {
    VkSurfaceCapabilitiesKHR capabilities = {};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vulkan.physicalDevice, vulkan.surface, &capabilities);

    u32 requestedImageCount = capabilities.minImageCount + 1;
    if (requestedImageCount < capabilities.minImageCount) {
        requestedImageCount = capabilities.minImageCount;
    }
    if (capabilities.maxImageCount && requestedImageCount > capabilities.maxImageCount) {
        requestedImageCount = capabilities.maxImageCount;
    }

    windowSystemUpdateDimensions();

    /* Use the Vulkan surface's currentExtent when the compositor dictates the
     * size (i.e. not the special 0xFFFFFFFF value).  This avoids creating a
     * swapchain at a size that doesn't match the surface, which would cause
     * immediate VK_ERROR_OUT_OF_DATE / VK_SUBOPTIMAL and flickering. */
    if (capabilities.currentExtent.width != 0xFFFFFFFF) {
        window.width  = (int)capabilities.currentExtent.width;
        window.height = (int)capabilities.currentExtent.height;
        window.ratio  = (float)window.width / (float)window.height;
    } else {
        /* Clamp to surface min/max when the compositor lets us choose. */
        if ((u32)window.width < capabilities.minImageExtent.width)
            window.width = (int)capabilities.minImageExtent.width;
        if ((u32)window.width > capabilities.maxImageExtent.width)
            window.width = (int)capabilities.maxImageExtent.width;
        if ((u32)window.height < capabilities.minImageExtent.height)
            window.height = (int)capabilities.minImageExtent.height;
        if ((u32)window.height > capabilities.maxImageExtent.height)
            window.height = (int)capabilities.maxImageExtent.height;
        window.ratio = (float)window.width / (float)window.height;
    }

    rendererUpdateRenderDimensions();

    vulkanSwapchain.swapchainImageFormat = VK_FORMAT_B8G8R8A8_SRGB;
    vulkanSwapchain.vsync                = settingsGetBool("vsync");

    VkPresentModeKHR presentMode = {};
    if (vulkanSwapchain.vsync) {
        presentMode = VK_PRESENT_MODE_FIFO_KHR;
    } else {
        presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }

    static const char* presentModes[4] = {"IMMEDIATE", "MAILBOX", "FIFO", "FIFO_RELAXED"};

    VkFormat requesteFormat = (VkFormat)vulkanSwapchain.swapchainImageFormat;

    u32 count = {};
    vkGetPhysicalDeviceSurfaceFormatsKHR(vulkan.physicalDevice, vulkan.surface, &count, 0);
    Array(VkSurfaceFormatKHR) vkSurfaceFormats = {};
    arraySetSize(vkSurfaceFormats, count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(vulkan.physicalDevice,
                                         vulkan.surface,
                                         &count,
                                         vkSurfaceFormats);

    vkGetPhysicalDeviceSurfacePresentModesKHR(vulkan.physicalDevice, vulkan.surface, &count, 0);
    Array(VkPresentModeKHR) pPresentModes = {};
    arraySetSize(pPresentModes, count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(vulkan.physicalDevice,
                                              vulkan.surface,
                                              &count,
                                              pPresentModes);

    VkSwapchainKHR old = swapchain;

    VkSwapchainCreateInfoKHR createInfo = {};
    createInfo.sType                    = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface                  = vulkan.surface;
    createInfo.minImageCount            = requestedImageCount;
    createInfo.imageFormat              = static_cast<VkFormat>(vulkanSwapchain.swapchainImageFormat);
    createInfo.imageColorSpace          = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    createInfo.imageExtent      = VkExtent2D{static_cast<uint32_t>(window.width), static_cast<uint32_t>(window.height)};
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    createInfo.imageSharingMode      = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.queueFamilyIndexCount = 0;
    createInfo.pQueueFamilyIndices   = 0;
    createInfo.preTransform          = capabilities.currentTransform;
    createInfo.compositeAlpha        = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode           = presentMode;
    createInfo.clipped               = VK_TRUE;
    createInfo.oldSwapchain          = old;

    VkResult scResult =
        vkCreateSwapchainKHR(vulkan.device, &createInfo, nullptr, &swapchain);
    if (scResult != VK_SUCCESS) {
        terminate("vulkanSwapchain: failed to create swapchain!");
    }

    if (old) {
        vkDestroySwapchainKHR(vulkan.device, old, nullptr);
    }

    static VkImage imagesKHR[10] = {};
    vkGetSwapchainImagesKHR(vulkan.device, swapchain, &vulkanSwapchain.imageCount, nullptr);
    vkGetSwapchainImagesKHR(vulkan.device, swapchain, &vulkanSwapchain.imageCount, imagesKHR);

    foreachptr(struct VulkanImage * img, swapchainImages) {
        vkDestroyImageView(vulkan.device, img->view, nullptr);
    }

    foreachptr(VkSemaphore * semaphore, submitSemaphores) {
        vkDestroySemaphore(vulkan.device, *semaphore, nullptr);
    }

    arraySetSize(swapchainImages, vulkanSwapchain.imageCount);
    arraySetSize(submitSemaphores, vulkanSwapchain.imageCount);

    foreachptr(VkSemaphore * semaphore, submitSemaphores) {
        VkSemaphoreCreateInfo info = {};
        info.sType                 = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(vulkan.device, &info, nullptr, semaphore),
                 "vkCreateSemaphore(submit)");
    }

    for (i32 i = 0, si = vulkanSwapchain.imageCount; i < si; i++) {
        struct VulkanImage* swapchainImage = &swapchainImages[i];
        *swapchainImage                    = VulkanImage{};
        swapchainImage->mipLevels          = 1;
        swapchainImage->layers             = 1;
        swapchainImage->img                = imagesKHR[i];
        swapchainImage->aspect             = VK_IMAGE_ASPECT_COLOR_BIT;
        swapchainImage->format             = vulkanSwapchain.swapchainImageFormat;
        swapchainImage->usage              = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        swapchainImage->extent =
            VkExtent3D{static_cast<uint32_t>(window.width), static_cast<uint32_t>(window.height), 1};
        swapchainImage->layout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkImageViewCreateInfo createInfo           = {};
        createInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image                           = imagesKHR[i];
        createInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format                          = (VkFormat)vulkanSwapchain.swapchainImageFormat;
        createInfo.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.baseMipLevel   = 0;
        createInfo.subresourceRange.levelCount     = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount     = 1;
        vkCreateImageView(vulkan.device, &createInfo, nullptr, &swapchainImage->view);

        if (isDebug()) {
            vulkanUtilsSetName((uint64_t)swapchainImage->img,
                               VK_OBJECT_TYPE_IMAGE,
                               strtmp("%s: %d", "swapchain image ", i));
            vulkanUtilsSetName((uint64_t)swapchainImage->view,
                               VK_OBJECT_TYPE_IMAGE_VIEW,
                               strtmp("%s: %d", "swapchain view ", i));
        }

    }

    VkFormat swapchainFormat = (VkFormat)vulkanSwapchain.swapchainImageFormat;
    if (requesteFormat != swapchainFormat) {
        warn("-----------------------");
        warn("vulkanSwapchain: Swapchain format is different from requested");
        warn("vulkanSwapchain: requestedFormat: %d", requesteFormat);
        warn("vulkanSwapchain: swapchainFormat: %d", swapchainFormat);
        warn("-----------------------");
    }

    debug("vulkanSwapchain: image requested/given: %d/%d size: %dx%d mode: %s",
          requestedImageCount,
          vulkanSwapchain.imageCount,
          window.width,
          window.height,
          presentModes[presentMode]);

    arrayFree(pPresentModes);
    arrayFree(vkSurfaceFormats);

    signalEmit("swapchainCreated", nullptr);
}

void createPerFlightItems(void) {
    for (i32 i = 0, si = arraySize(flightItems); i < si; i++) {
        struct FlightItem* item = &flightItems[i];
        vkDestroyFence(vulkan.device, item->cmd.fence, nullptr);
        vkDestroyCommandPool(vulkan.device, item->pool, nullptr);
        vkDestroySemaphore(vulkan.device, item->acquired, nullptr);
    }

    arraySetSize(flightItems, FRAMES_IN_FLIGHT);
    memset(flightItems, 0, sizeof(struct FlightItem) * FRAMES_IN_FLIGHT);

    for (i32 i = 0, si = FRAMES_IN_FLIGHT; i < si; i++) {
        struct FlightItem* item = &flightItems[i];
        item->cmd.transient     = 0;

        VkCommandPoolCreateInfo poolInfo = {};
        poolInfo.sType                   = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags                   = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex        = vulkan.graphicsFamilyIndex;
        VK_CHECK(vkCreateCommandPool(vulkan.device, &poolInfo, nullptr, &item->pool),
                 "vkCreateCommandPool");

        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType                       = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool                 = flightItems[i].pool;
        allocInfo.level                       = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount          = 1;
        VK_CHECK(vkAllocateCommandBuffers(vulkan.device, &allocInfo, &item->cmd.cmd),
                 "vkAllocateCommandBuffers");

        if (isDebug()) {
            vulkanUtilsSetName((u64)item->cmd.cmd,
                               VK_OBJECT_TYPE_COMMAND_BUFFER,
                               strtmp("%s %d", "flight cmd:", i));
        }

        VkFenceCreateInfo fenceCreateInfo = {};
        fenceCreateInfo.sType             = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceCreateInfo.flags             = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(vulkan.device, &fenceCreateInfo, nullptr, &item->cmd.fence),
                 "vkCreateFence");

        VkSemaphoreCreateInfo info = {};
        info.sType                 = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(vulkan.device, &info, nullptr, &item->acquired),
                 "vkCreateSemaphore(acquired)");
    }
}

void vulkanSwapchainRecreate(void) {
    vulkan.skipFrame = 1;
    recreateSwapchain();
}

static double recreateStart = 0;
static int tempWidth, tempHeight;

static void endSkipFrame(void* _) {
    if (tempWidth != window.width || tempHeight != window.height) {
        recreateStart = 0;
        return;
    }

    vulkanWaitIdle("resolution changed");

    createSwapchain();
    createPerFlightItems();

    vulkan.skipFrame = 0;
    recreateStart    = 0;
}

void recreateSwapchain(void) {
    if (recreateStart == 0) {
        recreateStart = millies();
        tempWidth     = window.width;
        tempHeight    = window.height;
    }

    if (millies() > recreateStart + 400) {
        futureTaskAdd(0, endSkipFrame, nullptr);
    }
}
