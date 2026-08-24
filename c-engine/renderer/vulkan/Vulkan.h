#pragma once

namespace engine {
struct VulkanCommand;

struct Vulkan {
    VkInstance instance;
    VkDevice device;
    VkPhysicalDevice physicalDevice;
    VmaAllocator vmaAllocator;
    VkQueue graphicsQueue;
    VkPhysicalDeviceProperties deviceProperties;
    VkPhysicalDeviceFeatures2 deviceFeatures2;
    VkSurfaceKHR surface;
    VkDebugUtilsMessengerEXT debugMessenger;
    struct VulkanCommand* currentCmd;  // current cmd in flight
    u32 graphicsFamilyIndex;
    bool skipFrame;
};

extern struct Vulkan vulkan;

struct Command;
struct VulkanImage;

void vulkanInit(void);
void vulkanDestroy(void);
void vulkanPostUpdate(void);

void vulkanSetVsync(bool vsync);

// Access pass profiling data (for stats GUI)
const struct VulkanProfile* vulkanGetPassProfiles(void);
size_t vulkanGetPassProfileCount(void);
}  // namespace engine
