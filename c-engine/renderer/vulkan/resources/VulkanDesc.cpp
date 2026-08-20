#include "VulkanDesc.h"
#include "Utils.h"
#include "VulkanImage.h"
#include "../Vulkan.h"
#include "../utils/VulkanUtils.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/resources/VulkanBuffer.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"

struct VulkanDesc r_vulkanCreateDesc(struct VulkanDescInfo info) {
    assert(info.name);
    struct VulkanDesc desc = {};

    Array(VkDescriptorPoolSize) poolSizes = {};
    // VkDescriptorPoolSize poolSize         = {VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, 256};  //
    // min inline uniform size, similar to min push constant being 128 arrayPut(poolSizes,
    // poolSize);

    if (info.samplers) {
        VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_SAMPLER, static_cast<uint32_t>(info.samplers)};
        arrayPut(poolSizes, poolSize);
    }

    if (info.sampledImages) {
        VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, static_cast<uint32_t>(info.sampledImages)};
        arrayPut(poolSizes, poolSize);
    }

    if (info.sampledCubeImages) {
        VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, static_cast<uint32_t>(info.sampledCubeImages)};
        arrayPut(poolSizes, poolSize);
    }

    if (info.sampledImageLayered) {
        VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                         static_cast<uint32_t>(info.sampledImageLayered)};
        arrayPut(poolSizes, poolSize);
    }

    if (info.combinedImageSamplers) {
        VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                         static_cast<uint32_t>(info.combinedImageSamplers)};
        arrayPut(poolSizes, poolSize);
    }

    if (info.storageImages) {
        VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, static_cast<uint32_t>(info.storageImages)};
        arrayPut(poolSizes, poolSize);
    }

    if (info.ssbos) {
        VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<uint32_t>(info.ssbos)};
        arrayPut(poolSizes, poolSize);
    }

    if (info.ubos) {
        VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, static_cast<uint32_t>(info.ubos)};
        arrayPut(poolSizes, poolSize);
    }

    VkDescriptorPoolCreateInfo poolCreateInfo = {};
    poolCreateInfo.sType                      = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCreateInfo.flags                      = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolCreateInfo.maxSets                    = 1;
    poolCreateInfo.pPoolSizes                 = poolSizes;
    poolCreateInfo.poolSizeCount              = arraySize(poolSizes);
    vkCreateDescriptorPool(vulkan.device, &poolCreateInfo, nullptr, &desc.pool);
    arrayFree(poolSizes);

    Array(VkDescriptorSetLayoutBinding) bindings = {};
    Array(VkDescriptorBindingFlags) bindingFlags = {};
    VkDescriptorBindingFlags flags =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT  // empty/unbound slots in the array
        | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;  // allowed to call
                                                                  // vkUpdateDescriptorSets on a
                                                                  // descriptor set that is
                                                                  // currently in use

    // | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT             // change what's in the slots on
    // the fly first binding is always inline uniform block VkDescriptorSetLayoutBinding binding =
    // {0}; binding.binding                      = arraySize(bindings); binding.descriptorCount =
    // 256; binding.descriptorType               = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK;
    // binding.stageFlags                   = VK_SHADER_STAGE_ALL_GRAPHICS |
    // VK_SHADER_STAGE_COMPUTE_BIT; arrayPut(bindings, binding); arrayPut(bindingFlags, flags);

    if (info.samplers) {
        VkDescriptorSetLayoutBinding binding = {};
        binding.binding                      = arraySize(bindings);
        binding.descriptorCount              = info.samplers;
        binding.descriptorType               = VK_DESCRIPTOR_TYPE_SAMPLER;
        binding.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;

        arrayPut(bindings, binding);
        arrayPut(bindingFlags, flags);
    }

    if (info.sampledImages) {  // array of textures;  layout(set = 0, binding = SLOT_IMAGE) uniform
                               // texture2D textures[MAX_IMAGES];
        VkDescriptorSetLayoutBinding binding = {};
        binding.binding                      = arraySize(bindings);
        binding.descriptorCount              = info.sampledImages;
        binding.descriptorType               = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        binding.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
        arrayPut(bindings, binding);
        arrayPut(bindingFlags, flags);
    }

    if (info.sampledCubeImages) {  // layout(set = 0, binding = SLOT_CUBE_IMAGE) uniform textureCube
                                   // cubeTextures[MAX_IMAGES];
        VkDescriptorSetLayoutBinding binding = {};
        binding.binding                      = arraySize(bindings);
        binding.descriptorCount              = info.sampledCubeImages;
        binding.descriptorType               = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        binding.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
        arrayPut(bindings, binding);
        arrayPut(bindingFlags, flags);
    }

    if (info.sampledImageLayered) {  // texture array
        int firstBinding = arraySize(bindings);
        for (i32 i = 0, si = info.sampledImageLayered; i < si; i++) {
            VkDescriptorSetLayoutBinding binding = {};
            binding.binding                      = firstBinding + i;
            binding.descriptorCount              = 1;
            binding.descriptorType               = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            binding.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
            arrayPut(bindings, binding);
            arrayPut(bindingFlags, flags);
        }
    }

    if (info.combinedImageSamplers) {
        VkDescriptorSetLayoutBinding binding = {};
        binding.binding                      = arraySize(bindings);
        binding.descriptorCount              = info.combinedImageSamplers;
        binding.descriptorType               = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
        arrayPut(bindings, binding);
        arrayPut(bindingFlags, flags);
    }

    if (info.storageImages) {
        VkDescriptorSetLayoutBinding binding = {};
        binding.binding                      = arraySize(bindings);
        binding.descriptorCount              = info.storageImages;
        binding.descriptorType               = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        binding.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
        arrayPut(bindings, binding);
        arrayPut(bindingFlags, flags);

        // int firstBinding = arraySize(bindings);
        // for (i32 i = 0, si = info.storageImages; i < si; i++) {
        //     VkDescriptorSetLayoutBinding binding = {};
        //     binding.binding                      = firstBinding + i;
        //     binding.descriptorCount              = 1;
        //     binding.descriptorType               = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        //     binding.stageFlags                   = VK_SHADER_STAGE_ALL_GRAPHICS |
        //     VK_SHADER_STAGE_COMPUTE_BIT; arrayPut(bindings, binding); arrayPut(bindingFlags,
        //     flags);
        // }
    }

    if (info.ssbos) {
        int firstBinding = arraySize(bindings);
        for (i32 i = 0, si = info.ssbos; i < si; i++) {
            VkDescriptorSetLayoutBinding binding = {};
            binding.binding                      = firstBinding + i;
            binding.descriptorCount              = 1;
            binding.descriptorType               = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            binding.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
            arrayPut(bindings, binding);
            arrayPut(bindingFlags, flags);
        }
    }

    if (info.ubos) {
        int firstBinding = arraySize(bindings);
        for (i32 i = 0, si = info.ubos; i < si; i++) {
            VkDescriptorSetLayoutBinding binding = {};
            binding.binding                      = firstBinding + i;
            binding.descriptorCount              = 1;
            binding.descriptorType               = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            binding.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
            arrayPut(bindings, binding);
            arrayPut(bindingFlags, flags);
        }
    }

    VkDescriptorSetLayoutBindingFlagsCreateInfo layoutBindingFlags = {};
    layoutBindingFlags.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    layoutBindingFlags.bindingCount  = arraySize(bindingFlags);
    layoutBindingFlags.pBindingFlags = bindingFlags;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.pNext        = &layoutBindingFlags;
    layoutInfo.pBindings    = bindings;
    layoutInfo.bindingCount = arraySize(bindings);
    vkCreateDescriptorSetLayout(vulkan.device, &layoutInfo, nullptr, &desc.layout);
    arrayFree(bindings);
    arrayFree(bindingFlags);

    VkDescriptorSetAllocateInfo descAllocInfo = {};
    descAllocInfo.sType                       = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descAllocInfo.descriptorPool              = desc.pool;
    descAllocInfo.pSetLayouts                 = &desc.layout;
    descAllocInfo.descriptorSetCount          = 1;
    vkAllocateDescriptorSets(vulkan.device, &descAllocInfo, &desc.set);

    if (isDebug()) {
        vulkanUtilsSetName((uint64_t)desc.pool,
                           VK_OBJECT_TYPE_DESCRIPTOR_POOL,
                           strtmp("%s%s", "descPool ", info.name));
        vulkanUtilsSetName((uint64_t)desc.set,
                           VK_OBJECT_TYPE_DESCRIPTOR_SET,
                           strtmp("%s%s", "descSet ", info.name));
        vulkanUtilsSetName((uint64_t)desc.layout,
                           VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
                           strtmp("%s%s", "descLayout ", info.name));
    }

    return desc;
}

void vulkanDestroyDesc(struct VulkanDesc* desc) {
    vkDestroyDescriptorSetLayout(vulkan.device, desc->layout, nullptr);
    vkDestroyDescriptorPool(vulkan.device, desc->pool, nullptr);
}

// dstArrayElement becomes size, if writing INLINE_UNIFORM
void vulkanUpdateDesc(struct VulkanDesc* desc,
                      enum VulkanDescType type,
                      void* resource,
                      int dstBinding,
                      int dstArrayElement) {
    threadLock(&desc->lock);
    VkDescriptorImageInfo descriptorImageInfo                 = {};
    VkDescriptorBufferInfo descriptorBufferInfo               = {};
    VkWriteDescriptorSetInlineUniformBlock inlineUniformWrite = {};

    VkWriteDescriptorSet writeDescriptor = {};
    writeDescriptor.sType                = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writeDescriptor.descriptorCount      = 1;
    struct VulkanBuffer* buf;

    if (type == VULKAN_BINDING_SAMPLER) {
        descriptorImageInfo.sampler  = static_cast<VkSampler>(resource);
        writeDescriptor.pImageInfo  = &descriptorImageInfo;
    } else if (type == VULKAN_BINDING_SAMPLED_IMAGE) {
        struct VulkanImage* img          = static_cast<struct VulkanImage*>(resource);
        descriptorImageInfo.imageView   = img->view;
        descriptorImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writeDescriptor.pImageInfo      = &descriptorImageInfo;
    } else if (type == VULKAN_BINDING_COMBINED_IMAGE_SAMPLER) {
        struct VulkanImage* img          = static_cast<struct VulkanImage*>(resource);
        descriptorImageInfo.imageView   = img->view;
        descriptorImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        descriptorImageInfo.sampler     = vulkanGetLinearSampler();
        writeDescriptor.pImageInfo      = &descriptorImageInfo;
    } else if (type == VULKAN_BINDING_STORAGE_IMAGE) {
        struct VulkanImage* img          = static_cast<struct VulkanImage*>(resource);
        descriptorImageInfo.imageView   = img->view;
        descriptorImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        writeDescriptor.pImageInfo      = &descriptorImageInfo;
    } else if (type == VULKAN_BINDING_INLINE_UNIFORM_BLOCK) {
        inlineUniformWrite.sType    = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK;
        inlineUniformWrite.pNext    = nullptr;
        inlineUniformWrite.dataSize = dstArrayElement;
        inlineUniformWrite.pData    = resource;

        writeDescriptor.pNext           = &inlineUniformWrite;
        writeDescriptor.descriptorCount = dstArrayElement;
    } else {
        buf                          = static_cast<struct VulkanBuffer*>(resource);
        descriptorBufferInfo.buffer = buf->buf;
        descriptorBufferInfo.range  = buf->size;
        writeDescriptor.pBufferInfo = &descriptorBufferInfo;
    }

    writeDescriptor.dstSet          = desc->set;
    writeDescriptor.descriptorType  = static_cast<VkDescriptorType>(type);
    writeDescriptor.dstBinding      = dstBinding;
    writeDescriptor.dstArrayElement = dstArrayElement;
    vkUpdateDescriptorSets(vulkan.device, 1, &writeDescriptor, 0, 0);
    threadUnlock(&desc->lock);
}

void vulkanUpdateDescInline(struct VulkanDesc* desc, void* data, int size) {
    VkWriteDescriptorSetInlineUniformBlock inlineUniformWrite = {};
    inlineUniformWrite.sType    = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK;
    inlineUniformWrite.pNext    = nullptr;
    inlineUniformWrite.dataSize = size;
    inlineUniformWrite.pData    = data;

    VkWriteDescriptorSet writeDescriptor = {};
    writeDescriptor.sType                = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writeDescriptor.pNext                = &inlineUniformWrite;
    writeDescriptor.dstSet               = desc->set;
    writeDescriptor.dstBinding           = 0;
    writeDescriptor.descriptorType       = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK;
    writeDescriptor.descriptorCount      = size;
    writeDescriptor.pBufferInfo          = nullptr;
    writeDescriptor.pImageInfo           = nullptr;
    writeDescriptor.pTexelBufferView     = nullptr;
    vkUpdateDescriptorSets(vulkan.device, 1, &writeDescriptor, 0, nullptr);
}

void vulkanBindDesc(struct VulkanCommand* cmd,
                    struct VulkanPipe* pipe,
                    struct VulkanDesc* desc,
                    int firstSet) {
    vkCmdBindDescriptorSets(
        cmd->cmd,
        pipe->isCompute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipe->layout,
        firstSet,
        1,
        &desc->set,
        0,
        nullptr);
}
