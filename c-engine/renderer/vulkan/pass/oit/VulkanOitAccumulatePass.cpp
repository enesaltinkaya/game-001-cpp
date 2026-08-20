#include "VulkanOitAccumulatePass.h"
#include "ecs/system/scene/Scene.h"
#include "events/Events.h"
#include "renderer/Renderer.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/scene/VulkanScene.h"
#include "renderer/vulkan/scene/VulkanVisibleScenes.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"

static void added(void);
static void preUpdate(void);
static void update(void);
static void postUpdate(void);
static void removed(void);
static void recreatePipelines(void);

static double elapsedCPU;
static double elapsedGPU;

System vulkanOitAccumulatePass = {
    .name       = "oit_accumulate",
    .added      = added,
    .preUpdate  = preUpdate,
    .update     = update,
    .postUpdate = postUpdate,
    .removed    = removed,
};

static VulkanPipe graphicsPipe;

typedef struct OitAccumPushConstants {
    u64 transformBufferAddress;
    u64 drawInstanceBufferAddress;
    u64 culledBufferAddress;
    u64 jointMatrixBufferAddress;
    u64 entitySkinMapBufferAddress;
    u64 prevJointMatrixBufferAddress;
} OitAccumPushConstants;

static VkVertexInputBindingDescription sceneVertexBinding = {
    .binding   = 0,
    .stride    = sizeof(SceneVertex),
    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
};

static VkVertexInputAttributeDescription sceneVertexAttrs[] = {
    {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,    .offset = 0},   // position
    {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,    .offset = 12},  // normal
    {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 24},  // tangent
    {.location = 3, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,       .offset = 40},  // uv
    {.location = 4, .binding = 0, .format = VK_FORMAT_R32_UINT, .offset = 48},  // joints
    {.location = 5, .binding = 0, .format = VK_FORMAT_R32_UINT, .offset = 52},  // weights
};

static void swapchainCreated(void*) {
    recreatePipelines();
}

static void recreatePipelines(void) {
    if (graphicsPipe.pipe) {
        vulkanDestroyPipe(&graphicsPipe);
    }

    // OIT always renders at 1x; no MSAA resolve images exist for oitAccum/oitReveal
    graphicsPipe =
        vulkanCreatePipe(.name          = "oit_accumulate",
                         .vs            = "shaders/pass/oit/spv/oit_accumulate.vert.spv",
                         .fs            = "shaders/pass/oit/spv/oit_accumulate.frag.spv",
                         .colorFormat1  = VK_FORMAT_R16G16B16A16_SFLOAT,
                         .colorFormat2  = VK_FORMAT_R16_SFLOAT,
                         .depthFormat   = VK_FORMAT_D32_SFLOAT,
                         .depthTestOnly = 1,
                         .depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL,
                         .blendOit      = 1,
                         .clearColor1   = {0.0f, 0.0f, 0.0f, 0.0f},
                         .clearColor2   = {1.0f, 0.0f, 0.0f, 0.0f},
                         .clearColor1Enabled = 1,
                         .clearColor2Enabled = 1,
                         .vertexAttributes    = sceneVertexAttrs,
                         .vertexAttributeCount = 6,
                         .vertexBindings      = &sceneVertexBinding,
                         .vertexBindingCount  = 1);
}

static void added(void) {
    signalSubscribe("swapchainCreated", swapchainCreated);
    recreatePipelines();
}

static void preUpdate(void) {
    vulkanResetProfile(vulkan.currentCmd, &graphicsPipe.profile, 0);
}

static void update(void) {
    if (vulkan.skipFrame) return;

    VulkanCommand* cmd       = vulkan.currentCmd;
    VulkanImage* oitAccum    = vulkanFrameResourcesGetOitAccum();
    VulkanImage* oitReveal   = vulkanFrameResourcesGetOitReveal();
    VulkanImage* depthImage  = vulkanFrameResourcesGetDepth();
    u32 fi                   = renderer.flightIndex;

    if (!oitAccum || !oitReveal || !depthImage) return;

    // Transition OIT targets to color attachment
    vulkanTransition(cmd, oitAccum,  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    vulkanTransition(cmd, oitReveal, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);

    vulkanBeginProfile(cmd, &graphicsPipe.profile, 0);

    vulkanBeginRender(.cmd    = cmd,
                      .pipe   = &graphicsPipe,
                      .color1 = oitAccum,
                      .color2 = oitReveal,
                      .depth  = depthImage);

    vulkanViewport(cmd,
                   0,
                   oitAccum->extent.height,
                   oitAccum->extent.width,
                   -((i32)oitAccum->extent.height));
    vulkanScissor(cmd,
                  0,
                  0,
                  oitAccum->extent.width,
                  oitAccum->extent.height);

    vulkanBindPipe(cmd, &graphicsPipe);

    u32 visibleSceneCount = 0;
    Scene** visibleScenes = vulkanGetVisibleScenes(&visibleSceneCount);
    for (u32 si = 0; si < visibleSceneCount; si++) {
        Scene* scene = visibleScenes[si];
        if (!scene->backendScene) continue;
        VulkanScene* vs  = static_cast<VulkanScene*>(scene->backendScene);
        if (!vs->totalDraws) continue;

        vulkanBindVertex(cmd, &vs->vertexBuffer, 0, NULL, 0, NULL, 0);
        vulkanBindIndex(cmd, &vs->indexBuffer, 0, VK_INDEX_TYPE_UINT32);

        OitAccumPushConstants pc = {
            .transformBufferAddress    = vs->transformBuffer[fi].address,
            .drawInstanceBufferAddress = vs->drawInstanceBuffer.address,
            .culledBufferAddress       = vs->transCulledBuffer[fi].address,
            .jointMatrixBufferAddress  = vs->jointMatrixBuffer[fi].address,
            .entitySkinMapBufferAddress = vs->entitySkinMapBuffer[fi].address,
            .prevJointMatrixBufferAddress = vs->prevJointMatrixBuffer[fi].address,
        };
        vulkanPush(cmd, &graphicsPipe, sizeof(OitAccumPushConstants), &pc);

        vkCmdDrawIndexedIndirectCount(
            cmd->cmd,
            vs->transIndirectBuffer[fi].buf, 0,
            vs->transDrawCountBuffer[fi].buf, 0,
            vs->totalDraws,
            sizeof(SceneDrawIndexedCommand));
    }

    vulkanEndRender(cmd);
    vulkanEndProfile(cmd, &graphicsPipe.profile, 0);

    elapsedGPU = graphicsPipe.profile.elapsed;
}

static void postUpdate(void) {
    vulkanOitAccumulatePass.cpuElapsed = elapsedCPU;
    vulkanOitAccumulatePass.gpuElapsed = elapsedGPU;
    elapsedCPU                         = nanos();
    elapsedCPU                         = nanos() - elapsedCPU;
}

static void removed(void) {
    vulkanDestroyPipe(&graphicsPipe);
}
