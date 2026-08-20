#pragma once
#include "renderer/vulkan/Vulkan.h"
#include "thread/Thread.h"

typedef struct VulkanBuffer {
    struct Thread lock;
    VmaAllocationInfo vmaInfo;
    VkBuffer buf;
    VmaAllocation vma;
    VmaVirtualBlock virtualBlock;
    u64 address;
    u64 size;
} VulkanBuffer;

typedef struct VulkanVirtualBuf {
    VulkanBuffer* buffer;
    VkDeviceSize offset;
    VkDeviceSize size;
    VmaVirtualAllocation virtualAllocation;
} VulkanVirtualBuf;

typedef struct VulkanCommand VulkanCommand;
typedef struct VulkanImage VulkanImage;

typedef struct VulkanCopyInfo {
    VulkanCommand* cmd;
    // VulkanVirtualBuf* staging;

    struct {
        VulkanBuffer* buf;
        VulkanImage* img;
        void* data;
        u32 offset;
    } source;

    struct {
        VulkanBuffer* buf;
        u32 bufferOffset;
        VulkanImage* img;
        u32 layer;
        ivec3 imageExtent;
        ivec3 imageOffset;
        u32 bufferRowLength;
    } target;

    u32 size;
} VulkanCopyInfo;

VulkanBuffer vulkanCreateCpuBuffer(const char* name, u64 size, VkBufferUsageFlags usage);
VulkanBuffer vulkanCreateGpuBuffer(const char* name, u64 size, VkBufferUsageFlags usage);
VulkanBuffer vulkanCreateCpuToGpuBuffer(const char* name, u64 size, VkBufferUsageFlags usage);
VulkanBuffer vulkanCreateReadbackBuffer(const char* name, u64 size, VkBufferUsageFlags extraUsage);
VulkanBuffer vulkanCreateStagingBuffer(u64 size);

void vulkanDestroyBuffer(VulkanBuffer* buffer, VkFence fence);

#define vulkanCopy(...) r_vulkanCopy((VulkanCopyInfo){__VA_ARGS__})
void r_vulkanCopy(VulkanCopyInfo info);

VulkanVirtualBuf vulkanBufferAllocateVirtual(VulkanBuffer* buf, u32 size, u32 align);
void vulkanBufferDestroyVirtual(VulkanVirtualBuf* virtualBuf);

// The VMA allocator is not internally thread-safe: buffer creation runs on pool
// threads (scene loads, props uploads) while destruction runs on the main
// thread (vulkanCleanupGarbage).  Hold this across any direct vma* call made
// outside VulkanBuffer.c.
void vulkanVmaLock(void);
void vulkanVmaUnlock(void);
