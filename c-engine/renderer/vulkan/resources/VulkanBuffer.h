#pragma once
#include "renderer/vulkan/Vulkan.h"
#include "thread/Thread.h"

namespace engine {
struct VulkanBuffer {
    struct utils::Thread lock;
    VmaAllocationInfo vmaInfo;
    VkBuffer buf;
    VmaAllocation vma;
    VmaVirtualBlock virtualBlock;
    u64 address;
    u64 size;
};

struct VulkanVirtualBuf {
    VulkanBuffer* buffer;
    VkDeviceSize offset;
    VkDeviceSize size;
    VmaVirtualAllocation virtualAllocation;
};

struct VulkanCommand;
struct VulkanImage;

struct VulkanCopyInfo {
    VulkanCommand* cmd = nullptr;
    // VulkanVirtualBuf* staging;

    struct {
        VulkanBuffer* buf = nullptr;
        VulkanImage* img = nullptr;
        void* data = nullptr;
        u32 offset = 0;
    } source;

    struct {
        VulkanBuffer* buf = nullptr;
        u32 bufferOffset = 0;
        VulkanImage* img = nullptr;
        u32 layer = 0;
        ivec3 imageExtent{};
        ivec3 imageOffset{};
        u32 bufferRowLength = 0;
    } target;

    u32 size = 0;
};

VulkanBuffer vulkanCreateCpuBuffer(const char* name, u64 size, VkBufferUsageFlags usage);
VulkanBuffer vulkanCreateGpuBuffer(const char* name, u64 size, VkBufferUsageFlags usage);
VulkanBuffer vulkanCreateCpuToGpuBuffer(const char* name, u64 size, VkBufferUsageFlags usage);
VulkanBuffer vulkanCreateReadbackBuffer(const char* name, u64 size, VkBufferUsageFlags extraUsage);
VulkanBuffer vulkanCreateStagingBuffer(u64 size);

void vulkanDestroyBuffer(VulkanBuffer* buffer, VkFence fence);

#define vulkanCopy(...) engine::r_vulkanCopy(engine::VulkanCopyInfo{__VA_ARGS__})
void r_vulkanCopy(VulkanCopyInfo info);

VulkanVirtualBuf vulkanBufferAllocateVirtual(VulkanBuffer* buf, u32 size, u32 align);
void vulkanBufferDestroyVirtual(VulkanVirtualBuf* virtualBuf);

// The VMA allocator is not internally thread-safe: buffer creation runs on pool
// threads (scene loads, props uploads) while destruction runs on the main
// thread (vulkanCleanupGarbage).  Hold this across any direct vma* call made
// outside VulkanBuffer.c.
void vulkanVmaLock(void);
void vulkanVmaUnlock(void);
}  // namespace engine
