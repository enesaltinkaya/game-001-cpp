#include "VulkanPipe.h"
#include "../utils/VulkanError.h"
#include "../utils/VulkanUtils.h"
#include "../command/VulkanCommand.h"
#include "../resources/VulkanResourceManager.h"
#include "../resources/VulkanImage.h"
#include "../Vulkan.h"

#include "Utils.h"
#include "datamanager/DataManager.h"
#include "renderer/vulkan/resources/VulkanBuffer.h"

namespace engine {
static VkShaderModule vulkanCreateShader(const char* path);
static void vulkanDestroyShader(VkShaderModule shader);
static void createGraphicsPipe(VulkanPipe* pipe, VulkanPipeInfo pipeInfo);
static void createComputePipe(VulkanPipe* pipe, VulkanPipeInfo pipeInfo);

VulkanPipe r_vulkanCreatePipe(VulkanPipeInfo pipeInfo) {
    assert(pipeInfo.name && "name me");
    VulkanPipe pipe = {};
    assert(pipeInfo.name);
    assert(!(pipeInfo.vs && !pipeInfo.fs) || !(!pipeInfo.vs && pipeInfo.fs));
    assert(!(pipeInfo.tsc && !pipeInfo.tes) || !(!pipeInfo.tsc && pipeInfo.tes));
    assert(!(pipeInfo.vs && pipeInfo.comp));

    strncpy(pipe.name, pipeInfo.name, 63);
    pipe.set1    = pipeInfo.set1;
    pipe.set2    = pipeInfo.set2;
    pipe.set3    = pipeInfo.set3;
    pipe.set4    = pipeInfo.set4;
    pipe.profile = vulkanCreateProfile(pipeInfo.name);

    if (pipeInfo.vs) {
        createGraphicsPipe(&pipe, pipeInfo);
    }
    if (pipeInfo.comp) {
        createComputePipe(&pipe, pipeInfo);
    }

    return pipe;
}

void vulkanDestroyPipe(VulkanPipe* pipe) {
    vkDestroyPipeline(vulkan.device, pipe->pipe, nullptr);
    vkDestroyPipelineLayout(vulkan.device, pipe->layout, nullptr);
    vulkanDestroyProfile(&pipe->profile);
}

void vulkanBindPipe(VulkanCommand* cmd, VulkanPipe* pipe) {
    VkPipelineBindPoint point = pipe->isCompute ? VK_PIPELINE_BIND_POINT_COMPUTE
                                                : VK_PIPELINE_BIND_POINT_GRAPHICS;
    VulkanDesc* globalSet0    = &vulkanResources.globalSet0[renderer.flightIndex];
    vkCmdBindDescriptorSets(cmd->cmd, point, pipe->layout, 0, 1, &globalSet0->set, 0, nullptr);
    vkCmdBindPipeline(cmd->cmd, point, pipe->pipe);
}

void vulkanPush(VulkanCommand* cmd, VulkanPipe* pipe, u32 size, void* pc) {
    assert(size <= 256 && "push constants must fit within 256 bytes");
    vkCmdPushConstants(cmd->cmd,
                       pipe->layout,
                       VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT,
                       0,
                       size,
                       pc);
}

////////////////////////////
////////////////////////////
void createComputePipe(VulkanPipe* pipe, VulkanPipeInfo info) {
    pipe->isCompute                         = 1;
    std::vector<VkDescriptorSetLayout> setLayouts = {};

    if (info.set0_reserved) {
        setLayouts.push_back(info.set0_reserved->layout);
    } else {
        setLayouts.push_back(vulkanResources.globalSet0[0].layout);
    }

    if (info.set1) {
        setLayouts.push_back(info.set1->layout);
    }
    if (info.set2) {
        setLayouts.push_back(info.set2->layout);
    }
    if (info.set3) {
        setLayouts.push_back(info.set3->layout);
    }
    if (info.set4) {
        setLayouts.push_back(info.set4->layout);
    }

    VkPushConstantRange pc = {};
    pc.offset              = 0;
    pc.size                = 256;
    pc.stageFlags          = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType                      = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount             = static_cast<i32>(setLayouts.size());
    layoutInfo.pSetLayouts                = setLayouts.data();
    layoutInfo.pushConstantRangeCount     = 1;
    layoutInfo.pPushConstantRanges        = &pc;
    vkCreatePipelineLayout(vulkan.device, &layoutInfo, nullptr, &pipe->layout);

    VkShaderModule module                       = vulkanCreateShader(info.comp);
    VkPipelineShaderStageCreateInfo shaderStage = {};
    shaderStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    shaderStage.module = module;
    shaderStage.pName  = "main";

    VkComputePipelineCreateInfo pipelineCreateInfo = {};
    pipelineCreateInfo.sType                       = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineCreateInfo.layout                      = pipe->layout;
    pipelineCreateInfo.flags                       = 0;
    pipelineCreateInfo.basePipelineIndex           = -1;
    pipelineCreateInfo.basePipelineHandle          = VK_NULL_HANDLE;
    pipelineCreateInfo.stage                       = shaderStage;

    VK_CHECK(
        vkCreateComputePipelines(vulkan.device, nullptr, 1, &pipelineCreateInfo, nullptr, &pipe->pipe),
        "vkCreateComputePipelines");

    if (utils::isDebug() && info.name) {
        vulkanUtilsSetName((u64)pipe->pipe,
                           VK_OBJECT_TYPE_PIPELINE,
                           utils::strtmp("%s%s", "pipeline ", info.name));
        vulkanUtilsSetName((u64)pipe->layout,
                           VK_OBJECT_TYPE_PIPELINE_LAYOUT,
                           utils::strtmp("%s%s", "pipelineLayout ", info.name));
    }

    vulkanDestroyShader(shaderStage.module);
}

void createGraphicsPipe(VulkanPipe* pipe, VulkanPipeInfo info) {
    std::vector<VkDescriptorSetLayout> setLayouts = {};
    setLayouts.push_back(vulkanResources.globalSet0[0].layout);

if (info.clearColor1Enabled) {
        pipe->clearColor1Enabled = true;
        pipe->clearColor1        = VkClearValue{.color = {{info.clearColor1[0],
                                                          info.clearColor1[1],
                                                          info.clearColor1[2],
                                                          info.clearColor1[3]}}};
    }

    if (info.clearColor2Enabled) {
        pipe->clearColor2Enabled = true;
        pipe->clearColor2        = VkClearValue{.color = {{info.clearColor2[0],
                                                          info.clearColor2[1],
                                                          info.clearColor2[2],
                                                          info.clearColor2[3]}}};
    }

    if (info.clearColor3Enabled) {
        pipe->clearColor3Enabled = true;
        pipe->clearColor3        = VkClearValue{.color = {{info.clearColor3[0],
                                                          info.clearColor3[1],
                                                          info.clearColor3[2],
                                                          info.clearColor3[3]}}};
    }

    if (info.clearDepthEnabled) {
        pipe->clearDepthEnabled = true;
        pipe->clearDepth = VkClearValue{.depthStencil = {info.clearDepth[0], static_cast<uint32_t>(info.clearDepth[1])}};
    }

    if (info.set1) {
        setLayouts.push_back(info.set1->layout);
    }
    if (info.set2) {
        setLayouts.push_back(info.set2->layout);
    }
    if (info.set3) {
        setLayouts.push_back(info.set3->layout);
    }
    if (info.set4) {
        setLayouts.push_back(info.set4->layout);
    }

    VkPushConstantRange pc = {};
    pc.offset              = 0;
    pc.size                = 256;
    pc.stageFlags          = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType                      = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount             = static_cast<i32>(setLayouts.size());
    layoutInfo.pSetLayouts                = setLayouts.data();
    layoutInfo.pushConstantRangeCount     = 1;
    layoutInfo.pPushConstantRanges        = &pc;
    vkCreatePipelineLayout(vulkan.device, &layoutInfo, nullptr, &pipe->layout);

    VkPipelineRenderingCreateInfo renderingInfo = {};
    renderingInfo.sType                         = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;

    VkFormat colorFormats[3]  = {};
    int colorAttachementCount = 0;

    if (info.colorFormat1) {
        colorFormats[0] = (VkFormat)info.colorFormat1;
        colorAttachementCount++;
    }
    if (info.colorFormat2) {
        colorFormats[1] = (VkFormat)info.colorFormat2;
        colorAttachementCount++;
    }
    if (info.colorFormat3) {
        colorFormats[2] = (VkFormat)info.colorFormat3;
        colorAttachementCount++;
    }

    renderingInfo.colorAttachmentCount    = colorAttachementCount;
    renderingInfo.pColorAttachmentFormats = colorFormats;

    if (info.depthFormat) {
        renderingInfo.depthAttachmentFormat = (VkFormat)info.depthFormat;
    }

    VkPipelineInputAssemblyStateCreateInfo assembly = {};
    assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    if (info.lineList) {
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    }
    if (info.tsc && info.tes) {
        assembly.topology               = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
        assembly.primitiveRestartEnable = VK_FALSE;
    }

    VkPipelineRasterizationStateCreateInfo rasterizationState = {};
    rasterizationState.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizationState.polygonMode = VK_POLYGON_MODE_FILL;
    if (info.noCull) {
        rasterizationState.cullMode = VK_CULL_MODE_NONE;
    } else if (info.cullFront) {
        rasterizationState.cullMode = VK_CULL_MODE_FRONT_BIT;
    } else {
        rasterizationState.cullMode = VK_CULL_MODE_BACK_BIT;
    }
    rasterizationState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    rasterizationState.flags                   = 0;
    rasterizationState.depthClampEnable        = info.depthClamp ? VK_TRUE : VK_FALSE;
    rasterizationState.lineWidth               = 1.0F;
    rasterizationState.depthBiasEnable         = info.depthBiasEnable ? VK_TRUE : VK_FALSE;
    rasterizationState.depthBiasConstantFactor = info.depthBiasConstantFactor;
    rasterizationState.depthBiasSlopeFactor    = info.depthBiasSlopeFactor;
    rasterizationState.depthBiasClamp          = info.depthBiasClamp;
    if (info.wireFrame) {
        rasterizationState.polygonMode = VK_POLYGON_MODE_LINE;
    }

    VkPipelineColorBlendAttachmentState attachementStates[3] = {};
    for (i32 i = 0, si = colorAttachementCount; i < si; i++) {
        attachementStates[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        // Only blend attachment 0 (sceneColor). G-buffer attachments (normals,
        // material) have vec2 outputs with undefined alpha — SRC_ALPHA blend
        // would zero them out.
        if (info.blendOit && i == 0) {
            // OIT accum: additive (ONE / ONE)
            attachementStates[i].blendEnable         = VK_TRUE;
            attachementStates[i].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            attachementStates[i].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            attachementStates[i].colorBlendOp        = VK_BLEND_OP_ADD;
            attachementStates[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            attachementStates[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            attachementStates[i].alphaBlendOp        = VK_BLEND_OP_ADD;
        } else if (info.blendOit && i == 1) {
            // OIT reveal: multiplicative (ZERO / ONE_MINUS_SRC_COLOR)
            attachementStates[i].blendEnable         = VK_TRUE;
            attachementStates[i].srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            attachementStates[i].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            attachementStates[i].colorBlendOp        = VK_BLEND_OP_ADD;
            attachementStates[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            attachementStates[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            attachementStates[i].alphaBlendOp        = VK_BLEND_OP_ADD;
        } else if (info.blendRoad && i == 0) {
            // Road "union" accumulation. Every road fragment is the same
            // constant color, so RGB REPLACE keeps it identical whether one
            // or several rectangles cover a pixel. Alpha takes the MAX so
            // overlapping junction rectangles don't add up (no darkening).
            attachementStates[i].blendEnable         = VK_TRUE;
            attachementStates[i].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            attachementStates[i].dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            attachementStates[i].colorBlendOp        = VK_BLEND_OP_ADD;
            attachementStates[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            attachementStates[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            attachementStates[i].alphaBlendOp        = VK_BLEND_OP_MAX;
        } else if (info.blend && i == 0) {
            attachementStates[i].blendEnable         = VK_TRUE;
            attachementStates[i].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            attachementStates[i].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            attachementStates[i].colorBlendOp        = VK_BLEND_OP_ADD;
            attachementStates[i].srcAlphaBlendFactor = info.blendPreserveAlpha ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE;
            attachementStates[i].dstAlphaBlendFactor = info.blendPreserveAlpha ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
            attachementStates[i].alphaBlendOp        = VK_BLEND_OP_ADD;
        } else {
            attachementStates[i].blendEnable = VK_FALSE;
        }
    }

    VkPipelineColorBlendStateCreateInfo blend = {};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = colorAttachementCount;
    blend.pAttachments    = attachementStates;

    VkPipelineMultisampleStateCreateInfo msaa = {};
    msaa.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineViewportStateCreateInfo viewport = {};
    viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount  = 1;
    viewport.flags         = 0;

    VkPipelineDepthStencilStateCreateInfo depth = {};
    depth.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable  = VK_FALSE;
    depth.depthWriteEnable = VK_FALSE;
    depth.depthCompareOp   = VK_COMPARE_OP_NEVER;
    depth.minDepthBounds   = 0.0f;
    depth.maxDepthBounds   = 1.0f;

    if (info.depthFormat) {
        depth.depthTestEnable = VK_TRUE;
        if (info.depthTestOnly && info.depthCompareOp) {
            /* Depth test with custom compare op but no depth write (e.g., OIT) */
            depth.depthWriteEnable = VK_FALSE;
            depth.depthCompareOp   = info.depthCompareOp;
        } else if (info.depthCompareOp) {
            /* Custom depth compare operation specified (e.g., for shadow mapping) */
            depth.depthWriteEnable = VK_TRUE;
            depth.depthCompareOp   = info.depthCompareOp;
        } else if (info.depthTestOnly) {
            depth.depthWriteEnable = VK_FALSE;
            depth.depthCompareOp   = VK_COMPARE_OP_EQUAL;
        } else {
            depth.depthWriteEnable = VK_TRUE;
            depth.depthCompareOp   = VK_COMPARE_OP_GREATER_OR_EQUAL;
        }
    }

    VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_VIEWPORT};
    VkPipelineDynamicStateCreateInfo dynamic = {};
    dynamic.sType                            = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.pDynamicStates                   = dynamicStates;
    dynamic.dynamicStateCount                = 2;
    dynamic.flags                            = 0;

    std::vector<VkPipelineShaderStageCreateInfo> shaders = {};
    if (info.vs) {
        VkShaderModule module                       = vulkanCreateShader(info.vs);
        VkPipelineShaderStageCreateInfo shaderStage = {};
        shaderStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
        shaderStage.module = module;
        shaderStage.pName  = "main";
        shaders.push_back(shaderStage);
    }

    if (info.fs) {
        VkShaderModule module                       = vulkanCreateShader(info.fs);
        VkPipelineShaderStageCreateInfo shaderStage = {};
        shaderStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStage.stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        shaderStage.module = module;
        shaderStage.pName  = "main";
        shaders.push_back(shaderStage);
    }

    if (info.tsc) {
        VkShaderModule module                       = vulkanCreateShader(info.tsc);
        VkPipelineShaderStageCreateInfo shaderStage = {};
        shaderStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStage.stage  = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        shaderStage.module = module;
        shaderStage.pName  = "main";
        shaders.push_back(shaderStage);
    }

    if (info.tes) {
        VkShaderModule module                       = vulkanCreateShader(info.tes);
        VkPipelineShaderStageCreateInfo shaderStage = {};
        shaderStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStage.stage  = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        shaderStage.module = module;
        shaderStage.pName  = "main";
        shaders.push_back(shaderStage);
    }

    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    std::vector<VkVertexInputAttributeDescription> inputAttributes = {};
    std::vector<VkVertexInputBindingDescription> inputBindings     = {};

    if (info.vertexAttributeCount > 0 && info.vertexAttributes) {
        for (u32 i = 0; i < info.vertexAttributeCount; i++) {
            inputAttributes.push_back(info.vertexAttributes[i]);
        }
        for (u32 i = 0; i < info.vertexBindingCount; i++) {
            inputBindings.push_back(info.vertexBindings[i]);
        }
    } else {
        if (info.in1bind.stride) {
            inputBindings.push_back(info.in1bind);
        }
        if (info.in1attr.format) {
            inputAttributes.push_back(info.in1attr);
        }
        if (info.in2bind.stride) {
            inputBindings.push_back(info.in2bind);
        }
        if (info.in2attr.format) {
            inputAttributes.push_back(info.in2attr);
        }
    }

    vertexInput.pVertexAttributeDescriptions    = inputAttributes.data();
    vertexInput.vertexAttributeDescriptionCount = static_cast<i32>(inputAttributes.size());
    vertexInput.pVertexBindingDescriptions      = inputBindings.data();
    vertexInput.vertexBindingDescriptionCount   = static_cast<i32>(inputBindings.size());

    VkPipelineTessellationStateCreateInfo tess = {};
    tess.sType              = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
    tess.patchControlPoints = info.patchControlPoints > 0 ? info.patchControlPoints : 3;

    VkGraphicsPipelineCreateInfo pipelineCreateInfo = {};
    pipelineCreateInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineCreateInfo.layout              = pipe->layout;
    pipelineCreateInfo.renderPass          = nullptr;
    pipelineCreateInfo.flags               = 0;
    pipelineCreateInfo.basePipelineIndex   = -1;
    pipelineCreateInfo.basePipelineHandle  = VK_NULL_HANDLE;
    pipelineCreateInfo.pNext               = &renderingInfo;
    pipelineCreateInfo.pInputAssemblyState = &assembly;
    pipelineCreateInfo.pRasterizationState = &rasterizationState;
    pipelineCreateInfo.pColorBlendState    = &blend;
    pipelineCreateInfo.pMultisampleState   = &msaa;
    pipelineCreateInfo.pViewportState      = &viewport;
    pipelineCreateInfo.pDepthStencilState  = &depth;
    pipelineCreateInfo.pDynamicState       = &dynamic;
    pipelineCreateInfo.pStages             = shaders.data();
    pipelineCreateInfo.stageCount          = static_cast<i32>(shaders.size());
    pipelineCreateInfo.pVertexInputState   = &vertexInput;
    pipelineCreateInfo.pTessellationState  = &tess;
    VK_CHECK(
        vkCreateGraphicsPipelines(vulkan.device, nullptr, 1, &pipelineCreateInfo, nullptr, &pipe->pipe),
        "vkCreateGraphicsPipelines");

    if (utils::isDebug()) {
        vulkanUtilsSetName((u64)pipe->pipe,
                           VK_OBJECT_TYPE_PIPELINE,
                           utils::strtmp("%s%s", "pipeline ", info.name));
        vulkanUtilsSetName((u64)pipe->layout,
                           VK_OBJECT_TYPE_PIPELINE_LAYOUT,
                           utils::strtmp("%s%s", "pipelineLayout ", info.name));
    }

    for (VkPipelineShaderStageCreateInfo& shaderStage : shaders) {
        vulkanDestroyShader(shaderStage.module);
    }

}

VkShaderModule vulkanCreateShader(const char* path) {
    const char* ppath;

    if (utils::strContains(path, "/spv/")) {
        ppath = utils::strtmp("%s.%s", path, utils::isDebug() ? "debug" : "release");
    } else {
        ppath = path;
    }

    utils::String fileContents                       = utils::dataManagerRead(ppath);
    VkShaderModule module                     = {};
    VkShaderModuleCreateInfo moduleCreateInfo = {};
    moduleCreateInfo.sType                    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleCreateInfo.codeSize                 = fileContents.size;
    moduleCreateInfo.pCode                     = static_cast<const uint32_t*>((void*)fileContents.data);
    VK_CHECK(vkCreateShaderModule(vulkan.device, &moduleCreateInfo, nullptr, &module),
             "vkCreateShaderModule");
    utils::stringDestroy(&fileContents);

    if (utils::isDebug()) {
        vulkanUtilsSetName((u64)module,
                           VK_OBJECT_TYPE_SHADER_MODULE,
                           utils::strtmp("%s%s", "shader ", path));
    }
    return module;
}

void vulkanDestroyShader(VkShaderModule module) {
    vkDestroyShaderModule(vulkan.device, module, nullptr);
}

void r_vulkanBeginRender(VulkanBeginRenderInfo beginRenderInfo) {
    int colorAttachmentCount = 0;

    u32 width  = 0;
    u32 height = 0;

    VkRenderingAttachmentInfo colorAttachmentInfos[3] = {};

    if (beginRenderInfo.color1) {
        colorAttachmentInfos[0].imageView = beginRenderInfo.color1->view;
        colorAttachmentInfos[0].storeOp   = VK_ATTACHMENT_STORE_OP_STORE;
        if (beginRenderInfo.pipe->clearColor1Enabled) {
            colorAttachmentInfos[0].clearValue = beginRenderInfo.pipe->clearColor1;
        }
        colorAttachmentInfos[0].imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
        colorAttachmentInfos[0].sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachmentInfos[0].loadOp      = beginRenderInfo.pipe->clearColor1Enabled
                                                  ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                  : VK_ATTACHMENT_LOAD_OP_LOAD;
        width                               = beginRenderInfo.color1->extent.width;
        height                              = beginRenderInfo.color1->extent.height;
        colorAttachmentCount++;
    }

    if (beginRenderInfo.color2) {
        colorAttachmentInfos[1].imageView = beginRenderInfo.color2->view;
        colorAttachmentInfos[1].storeOp   = VK_ATTACHMENT_STORE_OP_STORE;
        if (beginRenderInfo.pipe->clearColor2Enabled) {
            colorAttachmentInfos[1].clearValue = beginRenderInfo.pipe->clearColor2;
        }
        colorAttachmentInfos[1].imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
        colorAttachmentInfos[1].sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachmentInfos[1].loadOp      = beginRenderInfo.pipe->clearColor2Enabled
                                                  ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                  : VK_ATTACHMENT_LOAD_OP_LOAD;
        width                               = beginRenderInfo.color2->extent.width;
        height                              = beginRenderInfo.color2->extent.height;
        colorAttachmentCount++;
    }

    if (beginRenderInfo.color3) {
        colorAttachmentInfos[2].imageView = beginRenderInfo.color3->view;
        colorAttachmentInfos[2].storeOp   = VK_ATTACHMENT_STORE_OP_STORE;
        if (beginRenderInfo.pipe->clearColor3Enabled) {
            colorAttachmentInfos[2].clearValue = beginRenderInfo.pipe->clearColor3;
        }
        colorAttachmentInfos[2].imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
        colorAttachmentInfos[2].sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachmentInfos[2].loadOp      = beginRenderInfo.pipe->clearColor3Enabled
                                                  ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                  : VK_ATTACHMENT_LOAD_OP_LOAD;
        width                               = beginRenderInfo.color3->extent.width;
        height                              = beginRenderInfo.color3->extent.height;
        colorAttachmentCount++;
    }

    // if (beginRenderInfo.color2) {
    //     colorAttachmentInfos[1].sType       =
    //     VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    //     colorAttachmentInfos[1].imageView   = beginRenderInfo.color2->view;
    //     colorAttachmentInfos[1].imageLayout =
    //     VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL; colorAttachmentInfos[1].loadOp =
    //     beginRenderInfo.pipe->hasClearColor2 ? VK_ATTACHMENT_LOAD_OP_CLEAR :
    //     VK_ATTACHMENT_LOAD_OP_LOAD; colorAttachmentInfos[1].storeOp     =
    //     VK_ATTACHMENT_STORE_OP_STORE; if
    //     (beginRenderInfo.pipe->hasClearColor2) {
    //         colorAttachmentInfos[1].clearValue =
    //         beginRenderInfo.pipe->clearColor2;
    //     }
    //     colorAttachmentCount++;
    // }

    // if (beginRenderInfo.color3) {
    //     colorAttachmentInfos[2].sType       =
    //     VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    //     colorAttachmentInfos[2].imageView   = beginRenderInfo.color3->view;
    //     colorAttachmentInfos[2].imageLayout =
    //     VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL; colorAttachmentInfos[2].loadOp =
    //     beginRenderInfo.pipe->hasClearColor3 ? VK_ATTACHMENT_LOAD_OP_CLEAR :
    //     VK_ATTACHMENT_LOAD_OP_LOAD; colorAttachmentInfos[2].storeOp     =
    //     VK_ATTACHMENT_STORE_OP_STORE; if
    //     (beginRenderInfo.pipe->hasClearColor3) {
    //         colorAttachmentInfos[2].clearValue =
    //         beginRenderInfo.pipe->clearColor3;
    //     }
    //     colorAttachmentCount++;
    // }

    VkRenderingAttachmentInfo depthAttachmentInfo = {};
    if (beginRenderInfo.depth) {
        depthAttachmentInfo.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        if (beginRenderInfo.depthLayer > 0 && !beginRenderInfo.depth->views.empty()) {
            depthAttachmentInfo.imageView =
                beginRenderInfo.depth->views[beginRenderInfo.depthLayer - 1];
            depthAttachmentInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        } else {
            depthAttachmentInfo.imageView = beginRenderInfo.depth->view;
            depthAttachmentInfo.storeOp   = VK_ATTACHMENT_STORE_OP_STORE;
        }
        depthAttachmentInfo.imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
        depthAttachmentInfo.loadOp      = beginRenderInfo.pipe->clearDepthEnabled
                                              ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                              : VK_ATTACHMENT_LOAD_OP_LOAD;
        if (beginRenderInfo.pipe->clearDepthEnabled) {
            depthAttachmentInfo.clearValue = beginRenderInfo.pipe->clearDepth;
        }
        width  = beginRenderInfo.depth->extent.width;
        height = beginRenderInfo.depth->extent.height;
    }

    VkRenderingInfo renderInfo = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR,
        .pNext                = nullptr,
        .flags                = 0,
        .renderArea           = {.offset = {0, 0}, .extent = {width, height}},
        .layerCount           = 1,
        .viewMask             = 0,
        .colorAttachmentCount = static_cast<uint32_t>(colorAttachmentCount),
        .pColorAttachments    = colorAttachmentInfos,
        .pDepthAttachment     = beginRenderInfo.depth ? &depthAttachmentInfo : nullptr,
        .pStencilAttachment   = nullptr,
    };

    if (utils::isDebug()) {
        vulkanLabelBegin(beginRenderInfo.cmd, beginRenderInfo.pipe->name);
    }
    // Central layout guarantee: depth attachments must be in
    // DEPTH_STENCIL_ATTACHMENT_OPTIMAL at Begin time. Many passes sample the
    // resolved depth (TAA, FSR, SSR, decal, weather, ...) and leave it in
    // SHADER_READ_ONLY, so consumers binding it as an attachment cannot assume
    // the prior state (validation VUID 09588 fired every frame at
    // oit_accumulate). vulkanTransition is a no-op when the tracked layout
    // already matches, so this costs nothing for callers (contact_shadow,
    // light_culling) that transition it themselves.
    if (beginRenderInfo.depth) {
        vulkanTransition(beginRenderInfo.cmd,
                         beginRenderInfo.depth,
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, 1);
    }
    vkCmdBeginRendering(beginRenderInfo.cmd->cmd, &renderInfo);
}

void vulkanEndRender(VulkanCommand* cmd) {
    vkCmdEndRendering(cmd->cmd);
    if (utils::isDebug()) {
        vulkanLabelEnd(cmd);
    }
}

void vulkanDraw(VulkanCommand* cmd, int vertexCount, int instanceCount) {
    vkCmdDraw(cmd->cmd, vertexCount, instanceCount, 0, 0);
}

void vulkanDrawIndexed(VulkanCommand* cmd, int indexCount, int instanceCount) {
    vkCmdDrawIndexed(cmd->cmd, indexCount, instanceCount, 0, 0, 0);
}

void vulkanDrawIndexedIndirect(VulkanCommand* cmd, VulkanBuffer* buf, u32 drawCount, u32 stride) {
    vkCmdDrawIndexedIndirect(cmd->cmd, buf->buf, 0, drawCount, stride);
}

void vulkanDrawIndirectCount(VulkanCommand* cmd,
                             VulkanBuffer* buffer,
                             u64 offset,
                             VulkanBuffer* countBuffer,
                             u64 countBufferOffset,
                             u32 maxDrawCount,
                             u32 stride) {
    vkCmdDrawIndirectCount(cmd->cmd,
                           buffer->buf,
                           offset,
                           countBuffer->buf,
                           countBufferOffset,
                           maxDrawCount,
                           stride);
}

void vulkanBindIndex(VulkanCommand* cmd,
                     VulkanBuffer* indexBuffer,
                     u64 offset,
                     VkIndexType indexType) {
    vkCmdBindIndexBuffer(cmd->cmd, indexBuffer->buf, offset, indexType);
}

void vulkanBindVertex(VulkanCommand* cmd,
                      VulkanBuffer* buffer1,
                      u64 offset1,
                      VulkanBuffer* buffer2,
                      u64 offset2,
                      VulkanBuffer* buffer3,
                      u64 offset3) {
    VkDeviceSize offsets[3] = {offset1, offset2, offset3};

    int vertexBufferCount     = 0;
    VkBuffer vertexBuffers[3] = {};
    if (buffer1) {
        vertexBuffers[0] = buffer1->buf;
        vertexBufferCount++;
    }
    if (buffer2) {
        vertexBuffers[1] = buffer2->buf;
        vertexBufferCount++;
    }
    if (buffer3) {
        vertexBuffers[2] = buffer3->buf;
        vertexBufferCount++;
    }

    vkCmdBindVertexBuffers(cmd->cmd, 0, vertexBufferCount, vertexBuffers, offsets);
}

void vulkanDispatch(VulkanCommand* cmd, VulkanPipe* pipe, int x, int y, int z) {
    if (utils::isDebug()) {
        vulkanLabelBeginColor(cmd, pipe->name, 1.0f, 1.0f, 0.0f, 1.0f);
    }
    vkCmdDispatch(cmd->cmd, x, y, z);
    if (utils::isDebug()) {
        vulkanLabelEnd(cmd);
    }
}

void vulkanDispatchIndirect(VulkanCommand* cmd,
                            VulkanPipe* pipe,
                            VulkanBuffer* buffer,
                            u64 offset) {
    if (utils::isDebug()) {
        vulkanLabelBeginColor(cmd, pipe->name, 1.0f, 1.0f, 0.0f, 1.0f);
    }
    vkCmdDispatchIndirect(cmd->cmd, buffer->buf, offset);
    if (utils::isDebug()) {
        vulkanLabelEnd(cmd);
    }
}

void vulkanViewport(VulkanCommand* cmd, int x, int y, int w, int h) {
    VkViewport viewPort = {};
    viewPort.x          = x;
    viewPort.y          = y;
    viewPort.width      = w;
    viewPort.height     = h;
    viewPort.minDepth   = 0;
    viewPort.maxDepth   = 1;
    vkCmdSetViewport(cmd->cmd, 0, 1, &viewPort);
}

void vulkanScissor(VulkanCommand* cmd, int x, int y, int w, int h) {
    VkRect2D rect2D      = {};
    rect2D.offset.x      = x;
    rect2D.offset.y      = y;
    rect2D.extent.width  = w;
    rect2D.extent.height = h;
    vkCmdSetScissor(cmd->cmd, 0, 1, &rect2D);
}
}  // namespace engine
