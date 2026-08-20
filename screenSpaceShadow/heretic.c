/* h3r2tic Screen Space Shadows for Vulkan/C
 * Much simpler than Bend Studio's approach - no complex dispatch calculations!
 * Just standard compute shader with raymarching
 */

#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

typedef struct H3r2tic_CameraParams {
    float view_to_clip[16];
    float clip_to_view[16];
    float world_to_view[16];
    float view_to_world[16];
    float camera_pos[3];
    float rcp_near_plane_distance;  // 1.0 / near_plane
    float sun_direction_ws[3];
    float _pad0;
} H3r2tic_CameraParams;

typedef struct H3r2tic_PushConstants {
    float depth_tex_size[2];
    float depth_thickness;
    uint32_t linear_steps;
    float jitter_offset;
    uint32_t march_behind_surfaces;
    uint32_t use_sloppy_march;
    float _padding;
} H3r2tic_PushConstants;

typedef struct H3r2tic_SSS_Context {
    VkDevice device;
    VkPipelineLayout pipeline_layout;
    VkPipeline compute_pipeline;
    VkDescriptorSetLayout descriptor_set_layout;
    VkDescriptorPool descriptor_pool;

    VkSampler bilinear_sampler;

    // Per-frame resources
    VkBuffer camera_buffer;
    VkDeviceMemory camera_memory;
    VkDescriptorSet descriptor_set;
} H3r2tic_SSS_Context;

// Initialize the raymarching screen space shadow system
VkResult h3r2tic_sss_init(struct H3r2tic_SSS_Context* ctx, VkDevice device, VkPhysicalDevice physical_device, const uint32_t* shader_code, size_t shader_size) {
    ctx->device = device;
    VkResult result;

    // Create bilinear sampler
    VkSamplerCreateInfo sampler_info = {.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                        .magFilter    = VK_FILTER_LINEAR,
                                        .minFilter    = VK_FILTER_LINEAR,
                                        .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};

    result = vkCreateSampler(device, &sampler_info, NULL, &ctx->bilinear_sampler);
    if (result != VK_SUCCESS) {
        return result;
    }

    // Create descriptor set layout
    VkDescriptorSetLayoutBinding bindings[3] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};

    VkDescriptorSetLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = bindings};

    result = vkCreateDescriptorSetLayout(device, &layout_info, NULL, &ctx->descriptor_set_layout);
    if (result != VK_SUCCESS) {
        return result;
    }

    // Create pipeline layout with push constants
    VkPushConstantRange push_range = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(struct H3r2tic_PushConstants)};

    VkPipelineLayoutCreateInfo pipeline_layout_info = {.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                       .setLayoutCount         = 1,
                                                       .pSetLayouts            = &ctx->descriptor_set_layout,
                                                       .pushConstantRangeCount = 1,
                                                       .pPushConstantRanges    = &push_range};

    result = vkCreatePipelineLayout(device, &pipeline_layout_info, NULL, &ctx->pipeline_layout);
    if (result != VK_SUCCESS) {
        return result;
    }

    // Create shader module
    VkShaderModuleCreateInfo shader_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = shader_size, .pCode = shader_code};

    VkShaderModule shader_module;
    result = vkCreateShaderModule(device, &shader_info, NULL, &shader_module);
    if (result != VK_SUCCESS) {
        return result;
    }

    // Create compute pipeline
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader_module, .pName = "main"},
        .layout = ctx->pipeline_layout};

    result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &ctx->compute_pipeline);
    vkDestroyShaderModule(device, shader_module, NULL);
    if (result != VK_SUCCESS) {
        return result;
    }

    // Create descriptor pool
    VkDescriptorPoolSize pool_sizes[3] = {{.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1},
                                          {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1},
                                          {.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1}};

    VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = pool_sizes};

    result = vkCreateDescriptorPool(device, &pool_info, NULL, &ctx->descriptor_pool);
    if (result != VK_SUCCESS) {
        return result;
    }

    // Create camera uniform buffer
    VkBufferCreateInfo buffer_info = {.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                      .size        = sizeof(struct H3r2tic_CameraParams),
                                      .usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                      .sharingMode = VK_SHARING_MODE_EXCLUSIVE};

    result = vkCreateBuffer(device, &buffer_info, NULL, &ctx->camera_buffer);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkMemoryRequirements mem_req;
    vkGetBufferMemoryRequirements(device, ctx->camera_buffer, &mem_req);

    // Find memory type (host visible)
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);

    uint32_t memory_type_index = UINT32_MAX;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((mem_req.memoryTypeBits & (1 << i)) && (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            memory_type_index = i;
            break;
        }
    }

    VkMemoryAllocateInfo alloc_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mem_req.size, .memoryTypeIndex = memory_type_index};

    result = vkAllocateMemory(device, &alloc_info, NULL, &ctx->camera_memory);
    if (result != VK_SUCCESS) {
        return result;
    }

    vkBindBufferMemory(device, ctx->camera_buffer, ctx->camera_memory, 0);

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo desc_alloc_info = {.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                   .descriptorPool     = ctx->descriptor_pool,
                                                   .descriptorSetCount = 1,
                                                   .pSetLayouts        = &ctx->descriptor_set_layout};

    result = vkAllocateDescriptorSets(device, &desc_alloc_info, &ctx->descriptor_set);
    return result;
}

// Simple interleaved gradient noise for jitter
static float interleaved_gradient_noise(int x, int y, int frame) {
    float f = (float)(frame % 64);
    return fmodf(52.9829189f * fmodf(0.06711056f * (float)x + 0.00583715f * (float)y + f, 1.0f), 1.0f);
}

// Render screen space shadows - MUCH SIMPLER than Bend Studio!
void h3r2tic_sss_render(struct H3r2tic_SSS_Context* ctx,
                        VkCommandBuffer cmd_buffer,
                        VkImageView depth_view,
                        VkImageView output_view,
                        const struct H3r2tic_CameraParams* camera_params,
                        uint32_t width,
                        uint32_t height,
                        uint32_t frame_number) {
    // Update camera buffer
    void* data;
    vkMapMemory(ctx->device, ctx->camera_memory, 0, sizeof(struct H3r2tic_CameraParams), 0, &data);
    memcpy(data, camera_params, sizeof(struct H3r2tic_CameraParams));
    vkUnmapMemory(ctx->device, ctx->camera_memory);

    // Update descriptor set
    VkDescriptorBufferInfo buffer_desc = {.buffer = ctx->camera_buffer, .offset = 0, .range = sizeof(struct H3r2tic_CameraParams)};

    VkDescriptorImageInfo depth_desc = {.sampler = ctx->bilinear_sampler, .imageView = depth_view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

    VkDescriptorImageInfo output_desc = {.imageView = output_view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};

    VkWriteDescriptorSet writes[3] = {{.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet          = ctx->descriptor_set,
                                       .dstBinding      = 0,
                                       .descriptorCount = 1,
                                       .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                       .pBufferInfo     = &buffer_desc},
                                      {.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet          = ctx->descriptor_set,
                                       .dstBinding      = 1,
                                       .descriptorCount = 1,
                                       .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       .pImageInfo      = &depth_desc},
                                      {.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet          = ctx->descriptor_set,
                                       .dstBinding      = 2,
                                       .descriptorCount = 1,
                                       .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       .pImageInfo      = &output_desc}};

    vkUpdateDescriptorSets(ctx->device, 3, writes, 0, NULL);

    // Setup push constants
    struct H3r2tic_PushConstants push = {
        .depth_tex_size        = {(float)width, (float)height},
        .depth_thickness       = 0.5f,  // Tune this! Thicker = longer shadows
        .linear_steps          = 4,     // More steps = better quality, slower
        .jitter_offset         = interleaved_gradient_noise(0, 0, frame_number),
        .march_behind_surfaces = 1,  // Allow rays to pass behind surfaces
        .use_sloppy_march      = 0   // Use smart bilinear+point sampling
    };

    // Bind pipeline
    vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->compute_pipeline);

    // Bind descriptor set
    vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->pipeline_layout, 0, 1, &ctx->descriptor_set, 0, NULL);

    // Push constants
    vkCmdPushConstants(cmd_buffer, ctx->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(struct H3r2tic_PushConstants), &push);

    // Single dispatch! (8x8 workgroup size in shader)
    uint32_t dispatch_x = (width + 7) / 8;
    uint32_t dispatch_y = (height + 7) / 8;
    vkCmdDispatch(cmd_buffer, dispatch_x, dispatch_y, 1);
}

void h3r2tic_sss_cleanup(struct H3r2tic_SSS_Context* ctx) {
    vkDestroyPipeline(ctx->device, ctx->compute_pipeline, NULL);
    vkDestroyPipelineLayout(ctx->device, ctx->pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(ctx->device, ctx->descriptor_set_layout, NULL);
    vkDestroyDescriptorPool(ctx->device, ctx->descriptor_pool, NULL);
    vkDestroySampler(ctx->device, ctx->bilinear_sampler, NULL);
    vkDestroyBuffer(ctx->device, ctx->camera_buffer, NULL);
    vkFreeMemory(ctx->device, ctx->camera_memory, NULL);
}
