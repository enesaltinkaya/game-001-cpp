#include "VulkanScenePass.h"
#include "VulkanScenePass.h"
#include "ecs/system/scene/Scene.h"
#include "ecs/system/window/WindowSystem.h"
#include "events/Events.h"
#include "renderer/Renderer.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/scene/VulkanScene.h"
#include "renderer/vulkan/scene/VulkanVisibleScenes.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"

namespace engine {
static void recreatePipelines(void);

static double elapsedCPU;
static double elapsedGPU;

VulkanScenePass vulkanScenePass;

VulkanScenePass::VulkanScenePass() : System("scene_render") {}

static VulkanPipe scenePipe;
static VulkanPipe scenePipeDoubleSided;

typedef struct ScenePushConstants {
    u64 transformBufferAddress;
    u64 drawInstanceBufferAddress;
    u64 culledBufferAddress;
    u64 jointMatrixBufferAddress;
    u64 entitySkinMapBufferAddress;
    u64 prevJointMatrixBufferAddress;
} ScenePushConstants;

static VkVertexInputBindingDescription sceneVertexBinding = {
    .binding   = 0,
    .stride    = sizeof(SceneVertex),
    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
};

static VkVertexInputAttributeDescription sceneVertexAttrs[] = {
    {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0},
    {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 12},
    {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 24},
    {.location = 3, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 40},
    {.location = 4, .binding = 0, .format = VK_FORMAT_R32_UINT, .offset = 48},  // joints
    {.location = 5, .binding = 0, .format = VK_FORMAT_R32_UINT, .offset = 52},  // weights
};

static void recreatePipelines(void) {
    if (scenePipe.pipe) {
        vulkanDestroyPipe(&scenePipe);
        vulkanDestroyPipe(&scenePipeDoubleSided);
    }

    scenePipe = vulkanCreatePipe(.name                 = "scene_graphics",
                                  .vs                   = "shaders/pass/scene/spv/scene.vert.spv",
                                  .fs                   = "shaders/pass/scene/spv/scene.frag.spv",
                                  .colorFormat1         = VK_FORMAT_R16G16B16A16_SFLOAT,
                                  .colorFormat2         = VK_FORMAT_R16G16_SFLOAT,
                                  .colorFormat3         = VK_FORMAT_R8G8B8A8_UNORM,
                                  .colorFormat4         = VK_FORMAT_R16G16B16A16_SFLOAT,
                                  .depthFormat          = VK_FORMAT_D32_SFLOAT,
                                  .depthTestOnly        = 1,
                                  .clearColor1          = {0, 0, 0, 0},
                                  .clearColor1Enabled   = 0,  // terrain pass clears color1 first; scene loads on top
                                  .clearColor2          = {0, 0, 0, 0},
                                  .clearColor2Enabled   = 0,  // terrain pass clears first; scene loads to preserve terrain G-buffer
                                  .clearColor3          = {0, 0, 0, 0},
                                  .clearColor3Enabled   = 0,  // terrain pass clears first; scene loads to preserve terrain G-buffer
                                  .clearColor4          = {0, 0, 0, 0},
                                  .clearColor4Enabled   = 0,  // terrain pass clears first; scene loads to preserve terrain albedo
                                  .vertexAttributes     = sceneVertexAttrs,
                                  .vertexAttributeCount = 6,
                                  .vertexBindings       = &sceneVertexBinding,
                                  .vertexBindingCount   = 1);

    scenePipeDoubleSided = vulkanCreatePipe(.name         = "scene_graphics_ds",
                                             .vs           = "shaders/pass/scene/spv/scene.vert.spv",
                                             .fs           = "shaders/pass/scene/spv/scene.frag.spv",
                                             .colorFormat1 = VK_FORMAT_R16G16B16A16_SFLOAT,
                                             .colorFormat2 = VK_FORMAT_R16G16_SFLOAT,
                                             .colorFormat3 = VK_FORMAT_R8G8B8A8_UNORM,
                                             .colorFormat4 = VK_FORMAT_R16G16B16A16_SFLOAT,
                                             .depthFormat  = VK_FORMAT_D32_SFLOAT,
                                             .depthTestOnly       = 1,
                                             .noCull              = 1,
                                             .vertexAttributes    = sceneVertexAttrs,
                                             .vertexAttributeCount = 6,
                                             .vertexBindings       = &sceneVertexBinding,
                                             .vertexBindingCount   = 1);
}

static void swapchainCreated(void*) {
    recreatePipelines();
}

void VulkanScenePass::added() {
    utils::signalSubscribe("swapchainCreated", swapchainCreated);
    recreatePipelines();
}

void VulkanScenePass::preUpdate() {
    vulkanResetProfile(vulkan.currentCmd, &scenePipe.profile, 0);
}

void VulkanScenePass::update() {
    if (vulkan.skipFrame) return;

    VulkanCommand* cmd          = vulkan.currentCmd;
    VulkanImage* sceneColor     = vulkanFrameResourcesGetSceneColor();
    VulkanImage* normals        = vulkanFrameResourcesGetNormals();
    VulkanImage* material       = vulkanFrameResourcesGetMaterial();
    VulkanImage* albedo         = vulkanFrameResourcesGetAlbedo();
    VulkanImage* depthImage     = vulkanFrameResourcesGetDepth();
    u32 fi                      = renderer.flightIndex;

    if (!sceneColor || !normals || !material || !albedo || !depthImage) return;

    vulkanBeginProfile(cmd, &scenePipe.profile, 0);

    // NOTE: GPU culling was already dispatched by the culling pass.
    // The scene pass reuses the same indirect/culled/count buffers.

    vulkanBeginRender(.cmd     = cmd,
                      .pipe    = &scenePipe,
                      .color1  = sceneColor,
                      .color2  = normals,
                      .color3  = material,
                      .color4  = albedo,
                      .depth   = depthImage);

    vulkanViewport(cmd,
                   0,
                   sceneColor->extent.height,
                   sceneColor->extent.width,
                   -((i32)sceneColor->extent.height));
    vulkanScissor(cmd, 0, 0, sceneColor->extent.width, sceneColor->extent.height);

    u32 visibleSceneCount = 0;
    const Scene** visibleScenes = vulkanGetVisibleScenes(&visibleSceneCount);
    for (u32 si = 0; si < visibleSceneCount; si++) {
        const Scene* scene = visibleScenes[si];
        if (!scene->backendScene) continue;
        VulkanScene* vs  = static_cast<VulkanScene*>(scene->backendScene);
        if (!vs->totalDraws) continue;

        vulkanBindVertex(cmd, &vs->vertexBuffer, 0, NULL, 0, NULL, 0);
        vulkanBindIndex(cmd, &vs->indexBuffer, 0, VK_INDEX_TYPE_UINT32);

        // Single-sided opaque
        vulkanBindPipe(cmd, &scenePipe);
        {
            ScenePushConstants pc = {
                .transformBufferAddress    = vs->transformBuffer[fi].address,
                .drawInstanceBufferAddress = vs->drawInstanceBuffer.address,
                .culledBufferAddress       = vs->culledBuffer[fi].address,
                .jointMatrixBufferAddress  = vs->jointMatrixBuffer[fi].address,
                .entitySkinMapBufferAddress = vs->entitySkinMapBuffer[fi].address,
                .prevJointMatrixBufferAddress = vs->prevJointMatrixBuffer[fi].address,
            };
            vulkanPush(cmd, &scenePipe, sizeof(pc), &pc);

            vkCmdDrawIndexedIndirectCount(cmd->cmd,
                                          vs->indirectBuffer[fi].buf,
                                          0,
                                          vs->drawCountBuffer[fi].buf,
                                          0,
                                          vs->totalDraws,
                                          sizeof(SceneDrawIndexedCommand));
        }

        // Double-sided opaque
        vulkanBindPipe(cmd, &scenePipeDoubleSided);
        {
            ScenePushConstants pc = {
                .transformBufferAddress    = vs->transformBuffer[fi].address,
                .drawInstanceBufferAddress = vs->drawInstanceBuffer.address,
                .culledBufferAddress       = vs->dsCulledBuffer[fi].address,
                .jointMatrixBufferAddress  = vs->jointMatrixBuffer[fi].address,
                .entitySkinMapBufferAddress = vs->entitySkinMapBuffer[fi].address,
                .prevJointMatrixBufferAddress = vs->prevJointMatrixBuffer[fi].address,
            };
            vulkanPush(cmd, &scenePipeDoubleSided, sizeof(pc), &pc);

            vkCmdDrawIndexedIndirectCount(cmd->cmd,
                                          vs->dsIndirectBuffer[fi].buf,
                                          0,
                                          vs->dsDrawCountBuffer[fi].buf,
                                          0,
                                          vs->totalDraws,
                                          sizeof(SceneDrawIndexedCommand));
        }
    }

    vulkanEndRender(cmd);
    vulkanEndProfile(cmd, &scenePipe.profile, 0);
    elapsedGPU = scenePipe.profile.elapsed;
}

void VulkanScenePass::postUpdate() {
    vulkanScenePass.cpuElapsed = elapsedCPU;
    vulkanScenePass.gpuElapsed = elapsedGPU;
}

void VulkanScenePass::removed() {
    vulkanDestroyPipe(&scenePipe);
    vulkanDestroyPipe(&scenePipeDoubleSided);
}
}  // namespace engine
