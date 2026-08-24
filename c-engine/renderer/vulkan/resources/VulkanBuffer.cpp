#include "VulkanBuffer.h"
#include "VulkanImage.h"
#include "Utils.h"
#include "../Vulkan.h"
#include "../command/VulkanCommand.h"
#include "../utils/VulkanUtils.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"

namespace engine {
static void copyDataToBuffer(VulkanCopyInfo info);
static void copyDataToImage(VulkanCopyInfo info);
static void copyBufferToBuffer(VulkanCopyInfo info);
static void copyBufferToImage(VulkanCopyInfo info);
static void copyImageToBuffer(VulkanCopyInfo info);
static void copyImageToImage(VulkanCopyInfo info);

// The VMA allocator is not internally thread-safe.  Buffer creation happens on
// pool threads (scene loads, azgaar props uploads) while destruction happens on
// the main thread (vulkanCleanupGarbage), so every VMA allocator call is
// serialized here.
static utils::Thread vmaLock = {.mutex = PTHREAD_MUTEX_INITIALIZER};

void vulkanVmaLock(void) {
    utils::threadLock(&vmaLock);
}

void vulkanVmaUnlock(void) {
    utils::threadUnlock(&vmaLock);
}

static VulkanBuffer createBuffer(const char* name,
                                 u64 size,
VkBufferUsageFlags usage,
                                  int gpu,
                                  bool readBack) {
    assert(name && "need buf name");
    VulkanBuffer buf = {};
    utils::threadInitMutex(&buf.lock);

    VkBufferCreateInfo bufInfo = {};
    bufInfo.sType              = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size               = size;
    bufInfo.usage              = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    bufInfo.sharingMode        = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo createInfo = {};
    if (gpu == 2) {
        createInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        // HOST_ACCESS_RANDOM_WRITE implies HOST_COHERENT - GPU sees CPU writes immediately
        // without manual flush/invalidate. Required for per-frame vertex buffer updates.
        createInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                            VMA_ALLOCATION_CREATE_MAPPED_BIT;
    } else if (gpu) {
        createInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    } else if (readBack) {
        createInfo.usage = VMA_MEMORY_USAGE_AUTO;
        createInfo.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    } else {
        // Use CPU_ONLY to ensure coherent memory for direct pointer writes.
        // This is needed because GPU accesses this buffer via buffer device address
        // and needs to see CPU writes without manual flushes.
        // On integrated GPUs, this will use shared memory (fast).
        // On discrete GPUs, this will use system memory (slower but coherent).
        createInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;
        createInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                            VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }

    vulkanVmaLock();
    vmaCreateBuffer(vulkan.vmaAllocator, &bufInfo, &createInfo, &buf.buf, &buf.vma, &buf.vmaInfo);
    VmaVirtualBlockCreateInfo virtualInfo = {};
    virtualInfo.size                      = buf.vmaInfo.size;
    vmaCreateVirtualBlock(&virtualInfo, &buf.virtualBlock);
    vulkanVmaUnlock();

    buf.size = size;

    VkBufferDeviceAddressInfo pInfo = {};
    pInfo.sType                     = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    pInfo.buffer                    = buf.buf;
    buf.address                     = vkGetBufferDeviceAddress(vulkan.device, &pInfo);

    if (utils::isDebug()) {
        // Local format (not strtmp): this path runs on pool threads.
        char debugName[128];
        snprintf(debugName, sizeof(debugName), "buf %s", name);
        vulkanUtilsSetName(reinterpret_cast<u64>(buf.buf), VK_OBJECT_TYPE_BUFFER, debugName);
    }
    return buf;
}

VulkanBuffer vulkanCreateGpuBuffer(const char* name, u64 size, VkBufferUsageFlags usage) {
    return createBuffer(name, size, usage, 1, 0);
}

VulkanBuffer vulkanCreateCpuBuffer(const char* name, u64 size, VkBufferUsageFlags usage) {
    // Use CPU_ONLY to ensure coherent memory for direct pointer writes
    // This guarantees HOST_COHERENT_BIT which is needed for GPU to see CPU writes
    // without manual flushes. The GPU will access this via buffer device address.
    return createBuffer(name, size, usage, 0, 0);
}

VulkanBuffer vulkanCreateCpuToGpuBuffer(const char* name, u64 size, VkBufferUsageFlags usage) {
    return createBuffer(name, size, usage, 2, 0);
}

VulkanBuffer vulkanCreateReadbackBuffer(const char* name, u64 size, VkBufferUsageFlags extraUsage) {
    return createBuffer(name, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT | extraUsage, 0, 1);
}

void vulkanDestroyBuffer(VulkanBuffer* buffer, VkFence fence) {
    // if (!fence || vkGetFenceStatus(vulkan.device, fence) == VK_SUCCESS) {
    //     vmaDestroyVirtualBlock(buffer->virtualBlock);
    //     vmaDestroyBuffer(vulkan.vmaAllocator, buffer->buf, buffer->vma);
    // } else {
    addBufferGarbage(buffer, fence, nullptr);
    // }
    // The destruction is deferred (garbage list) and the entry above holds a
    // copy of every field it needs.  Invalidate the caller's struct so a
    // stale `if (buf.buf)` guard cannot destroy the same buffer a second
    // time (double free of the VMA virtual block on world re-entry).
    *buffer = VulkanBuffer{};
}

VulkanVirtualBuf vulkanBufferAllocateVirtual(VulkanBuffer* buf, u32 size, u32 align) {
    assert(buf->virtualBlock && "did you create the buffer with virtual flag on?");
    VulkanVirtualBuf virtualBuf = {};
    vulkanVmaLock();
    utils::threadLock(&buf->lock);
    u32 alignUp                              = (size + align - 1) & ~(align - 1);
    size                                     = alignUp;
    virtualBuf.size                          = size;
    virtualBuf.buffer                        = buf;
    VmaVirtualAllocationCreateInfo allocInfo = {};
    allocInfo.size                           = size;
    allocInfo.alignment                      = align;
    allocInfo.flags                          = VMA_VIRTUAL_ALLOCATION_CREATE_STRATEGY_MIN_TIME_BIT;
    VkResult result                          = vmaVirtualAllocate(virtualBuf.buffer->virtualBlock,
                                                                  &allocInfo,
                                                                  &virtualBuf.virtualAllocation,
                                                                  &virtualBuf.offset);
    assert(!result);
    utils::threadUnlock(&buf->lock);
    vulkanVmaUnlock();
    return virtualBuf;
}

void vulkanBufferDestroyVirtual(VulkanVirtualBuf* virtualBuf) {
    if (!virtualBuf->buffer) {
        return;
    }
    vulkanVmaLock();
    utils::threadLock(&virtualBuf->buffer->lock);
    vmaVirtualFree(virtualBuf->buffer->virtualBlock, virtualBuf->virtualAllocation);
    utils::threadUnlock(&virtualBuf->buffer->lock);
    vulkanVmaUnlock();
}

void r_vulkanCopy(VulkanCopyInfo copyInfo) {
    assert(!(copyInfo.target.buf && copyInfo.target.img) && "multiple targets!");
    assert(!(copyInfo.source.buf && copyInfo.source.img) && "multiple sources!");
    assert(!(copyInfo.source.buf && copyInfo.source.data) && "multiple sources!");
    assert(!(copyInfo.source.img && copyInfo.source.data) && "multiple sources!");
    assert(!(!copyInfo.cmd && copyInfo.target.buf && !copyInfo.target.buf->vmaInfo.pMappedData) &&
           "need commandbuffer for un-mapped copy!");
    assert(!(copyInfo.cmd && copyInfo.source.data && copyInfo.target.buf &&
             copyInfo.target.buf->vmaInfo.pMappedData) &&
           "dont need commandbuffer for mapped copy!");

    if (copyInfo.source.data && copyInfo.target.buf && copyInfo.target.buf->vmaInfo.pMappedData) {
memcpy(static_cast<char*>(copyInfo.target.buf->vmaInfo.pMappedData) + copyInfo.target.bufferOffset,
       static_cast<char*>(copyInfo.source.data) + copyInfo.source.offset,
       copyInfo.size);
        return;
    }

    assert(copyInfo.cmd && "need a cmd");

    if (copyInfo.source.data && copyInfo.target.buf) {
        copyDataToBuffer(copyInfo);
    }
    if (copyInfo.source.data && copyInfo.target.img) {
        copyDataToImage(copyInfo);
    }
    if (copyInfo.source.buf && copyInfo.target.buf) {
        copyBufferToBuffer(copyInfo);
    }
    if (copyInfo.source.buf && copyInfo.target.img) {
        copyBufferToImage(copyInfo);
    }
    if (copyInfo.source.img && copyInfo.target.buf) {
        copyImageToBuffer(copyInfo);
    }
    if (copyInfo.source.img && copyInfo.target.img) {
        copyImageToImage(copyInfo);
    }
}

void copyDataToBuffer(VulkanCopyInfo info) {
    char* source          = static_cast<char*>(info.source.data);
    VulkanBuffer* target = info.target.buf;

    VulkanBuffer staging = vulkanCreateStagingBuffer(info.size);
    memcpy(static_cast<char*>(staging.vmaInfo.pMappedData), source + info.source.offset, info.size);
    VkBufferCopy copyRegion = {
        0,
        info.target.bufferOffset,
        info.size,
    };
    vkCmdCopyBuffer(info.cmd->cmd, staging.buf, target->buf, 1, &copyRegion);
    addBufferGarbage(&staging, info.cmd->fence, &info.cmd->submitted);
}

void copyDataToImage(VulkanCopyInfo copyInfo) {
    char* source         = static_cast<char*>(copyInfo.source.data);
    VulkanImage* target = copyInfo.target.img;

    // if (copyInfo.staging) {
    //     memcpy((char*)copyInfo.staging->buffer->vmaInfo.pMappedData + copyInfo.staging->offset,
    //     source + copyInfo.source.offset, copyInfo.size);

    //     if (copyInfo.target.imageExtent[0] == 0 && copyInfo.target.imageExtent[1] == 0 &&
    //     copyInfo.target.imageExtent[2] == 0) {
    //         copyInfo.target.imageExtent[0] = target->extent.width;
    //         copyInfo.target.imageExtent[1] = target->extent.height;
    //         copyInfo.target.imageExtent[2] = target->extent.depth;
    //     }

    //     VkBufferImageCopy region = {
    //         .bufferOffset                    = copyInfo.staging->offset,
    //         .imageSubresource.aspectMask     = target->aspect,
    //         .imageSubresource.mipLevel       = 0,
    //         .imageSubresource.baseArrayLayer = copyInfo.target.layer,
    //         .imageSubresource.layerCount     = 1,
    //         .bufferRowLength                 = copyInfo.target.bufferRowLength,
    //         .imageExtent                     = {copyInfo.target.imageExtent[0],
    //         copyInfo.target.imageExtent[1], copyInfo.target.imageExtent[2]}, .imageOffset =
    //         {copyInfo.target.imageOffset[0], copyInfo.target.imageOffset[1],
    //         copyInfo.target.imageOffset[2]},
    //     };
    //     vkCmdCopyBufferToImage(copyInfo.cmd->cmd, copyInfo.staging->buffer->buf, target->img,
    //     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    //     // copyInfo.cmd->virtualBufsToClean.push_back(*copyInfo.staging);

    // } else {
    VulkanBuffer staging = vulkanCreateStagingBuffer(copyInfo.size);
    memcpy(static_cast<char*>(staging.vmaInfo.pMappedData), source + copyInfo.source.offset, copyInfo.size);

    if (copyInfo.target.imageExtent[0] == 0 && copyInfo.target.imageExtent[1] == 0 &&
        copyInfo.target.imageExtent[2] == 0) {
        copyInfo.target.imageExtent[0] = target->extent.width;
        copyInfo.target.imageExtent[1] = target->extent.height;
        copyInfo.target.imageExtent[2] = target->extent.depth;
    }

    VkBufferImageCopy region = {
        .bufferOffset                    = 0,
        .imageSubresource.aspectMask     = target->aspect,
        .imageSubresource.mipLevel       = 0,
        .imageSubresource.baseArrayLayer = copyInfo.target.layer,
        .imageSubresource.layerCount     = 1,
        .bufferRowLength                 = copyInfo.target.bufferRowLength,
        .bufferImageHeight               = 0,
        .imageExtent                     = {static_cast<uint32_t>(copyInfo.target.imageExtent[0]),
                                            static_cast<uint32_t>(copyInfo.target.imageExtent[1]),
                                            static_cast<uint32_t>(copyInfo.target.imageExtent[2])},
        .imageOffset                     = {copyInfo.target.imageOffset[0],
                                            copyInfo.target.imageOffset[1],
                                            copyInfo.target.imageOffset[2]},
    };
    vkCmdCopyBufferToImage(copyInfo.cmd->cmd,
                           staging.buf,
                           target->img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1,
                           &region);
    addBufferGarbage(&staging, copyInfo.cmd->fence, &copyInfo.cmd->submitted);
    // }
}

void copyBufferToBuffer(VulkanCopyInfo info) {
    VulkanBuffer* source    = info.source.buf;
    VulkanBuffer* target    = info.target.buf;
    VkBufferCopy copyRegion = {info.source.offset, info.target.bufferOffset, info.size};
    vkCmdCopyBuffer(info.cmd->cmd, source->buf, target->buf, 1, &copyRegion);
}

void copyBufferToImage(VulkanCopyInfo info) {
    VulkanBuffer* source = info.source.buf;
    VulkanImage* target  = info.target.img;

    VkBufferImageCopy region           = {};
    region.bufferOffset                = info.source.offset;
    region.imageSubresource.aspectMask = target->aspect;
    region.imageSubresource.layerCount = target->layers;
    region.imageExtent                 = target->extent;
    vkCmdCopyBufferToImage(info.cmd->cmd,
                           source->buf,
                           target->img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1,
                           &region);
}

void copyImageToBuffer(VulkanCopyInfo info) {
    VulkanImage* source  = info.source.img;
    VulkanBuffer* target = info.target.buf;

    VkBufferImageCopy region           = {};
    region.bufferOffset                = info.target.bufferOffset;
    region.imageSubresource.aspectMask = source->aspect;
    region.imageSubresource.layerCount = source->layers;
    region.imageExtent                 = source->extent;
    vkCmdCopyImageToBuffer(info.cmd->cmd,
                           source->img,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           target->buf,
                           1,
                           &region);
}

void copyImageToImage(VulkanCopyInfo info) {
    VulkanImage* source = info.source.img;
    VulkanImage* target = info.target.img;

    VkImageCopy region    = {};
    region.srcSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent         = info.source.img->extent;

    vkCmdCopyImage(info.cmd->cmd,
                   source->img,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   target->img,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1,
                   &region);
}

VulkanBuffer vulkanCreateStagingBuffer(u64 size) {
    return vulkanCreateCpuBuffer("staging", size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
}
}  // namespace engine
