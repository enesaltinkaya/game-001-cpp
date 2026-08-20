#include "renderer/vulkan/pass/terrain/VulkanTerrainPass.h"
#include "ecs/Ecs.h"
#include "ecs/system/terrain/Terrain.h"
#include "renderer/Renderer.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/scene/VulkanScene.h"
#include "renderer/vulkan/scene/VulkanTerrain.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/pass/terrain/VulkanTerrainHeightBaker.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/scene/SceneSystem.h"
#include "renderer/texture/TextureManager.h"
#include "events/Events.h"
#include "renderer/material/Material.h"

// Terrain pass: renders chunked terrain as regular mesh draws.
// Visibility is precomputed by TerrainSystem; each chunk is drawn
// with vkCmdDrawIndexed if its visible flag is set.

static void added(void);
static void preUpdate(void);
static void update(void);
static void postUpdate(void);
static void removed(void);

struct System vulkanTerrainPass = {
    .name       = "terrain",
    .added      = added,
    .preUpdate  = preUpdate,
    .update     = update,
    .postUpdate = postUpdate,
    .removed    = removed,
};

static VulkanPipe terrainPipe;

// Push constants for terrain draw
typedef struct TerrainPushConstants {
    u32 materialId;
    u32 wireFrame;
} TerrainPushConstants;

static VkVertexInputBindingDescription vertexBinding = {
    .binding   = 0,
    .stride    = sizeof(SceneVertex),
    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
};

static VkVertexInputAttributeDescription vertexAttrs[] = {
    {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0},
    {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 12},
    {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 24},
    {.location = 3, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 40},
};

static void recreatePipelines(void) {
    if (terrainPipe.pipe) vulkanDestroyPipe(&terrainPipe);

    int sampleCount = rendererGetMsaaSampleCount();

    terrainPipe = vulkanCreatePipe(
        .name                 = "terrain",
        .vs                   = "shaders/pass/terrain/spv/terrain.vert.spv",
        .fs                   = "shaders/pass/terrain/spv/terrain.frag.spv",
        .colorFormat1         = VK_FORMAT_R16G16B16A16_SFLOAT,
        .colorFormat2         = VK_FORMAT_R16G16_SFLOAT,
        .colorFormat3         = VK_FORMAT_R8G8B8A8_UNORM,
        .depthFormat          = VK_FORMAT_D32_SFLOAT,
        .msaa                 = sampleCount,
        .sampleShadingEnable  = rendererIsSampleShadingEnabled(),
        .minSampleShading     = rendererGetMinSampleShading(),
        .clearColor1          = {0, 0, 0, 0}, .clearColor1Enabled = 1,
        .clearColor2          = {0, 0, 0, 0}, .clearColor2Enabled = 1,
        .clearColor3          = {0, 0, 0, 0}, .clearColor3Enabled = 1,
        .vertexAttributes     = vertexAttrs,
        .vertexAttributeCount = 4,
        .vertexBindings       = &vertexBinding,
        .vertexBindingCount   = 1,
    );
}

static void swapchainCreated(void*) {
    recreatePipelines();
}

static bool terrainDefaultsSet;

static void setTerrainDefaults(void) {
    if (terrainDefaultsSet) return;

    Texture* grassAlbedo = getTextureByName("images/terrain/grass_default/albedo.ktx2");
    Texture* grassNormal = getTextureByName("images/terrain/grass_default/normal.ktx2");
    Texture* cliffAlbedo = getTextureByName("images/terrain/cliff_side_default/albedo.ktx2");
    Texture* cliffNormal = getTextureByName("images/terrain/cliff_side_default/normal.ktx2");

    u32 gaIdx = grassAlbedo ? grassAlbedo->id : 0;
    u32 gnIdx = grassNormal ? grassNormal->id : 0;
    u32 caIdx = cliffAlbedo ? cliffAlbedo->id : 0;
    u32 cnIdx = cliffNormal ? cliffNormal->id : 0;

  vulkanResourceSetTerrainDefaults(gaIdx, gnIdx, caIdx, cnIdx);

    terrainDefaultsSet = true;
}

static void added(void) {
    signalSubscribe("swapchainCreated", swapchainCreated);
    recreatePipelines();
}

static void preUpdate(void) {
    // Upload terrain GPU data on first frame if not done yet
    for (u32 i = 0; i < arraySize(ecs.terrains); i++) {
        Terrain* terrain = ecs.terrains[i];
        if (terrain && !terrain->backendData) {
            vulkanTerrainUpload(terrain);
            // Set world bounds in scene buffer for splatmap UV computation
            setTerrainDefaults();
      vulkanResourceSetTerrainBounds(
                terrain->boundsMin[0], terrain->boundsMin[1], terrain->boundsMin[2],
                terrain->boundsMax[0], terrain->boundsMax[1], terrain->boundsMax[2]);

            // Bake heightfield after terrain upload
            vulkanTerrainHeightBakerBake(terrain, 8.0f);
        }
    }

}

static void update(void) {
    if (vulkan.skipFrame) return;
    if (!terrainPipe.pipe) return;

    VulkanCommand* cmd = vulkan.currentCmd;
    if (!cmd) return;

    VulkanImage* sceneColor     = vulkanFrameResourcesGetSceneColor();
    VulkanImage* sceneColorMsaa = vulkanFrameResourcesGetSceneColorMsaa();
    VulkanImage* normals        = vulkanFrameResourcesGetNormals();
    VulkanImage* normalsMsaa    = vulkanFrameResourcesGetNormalsMsaa();
    VulkanImage* material       = vulkanFrameResourcesGetMaterial();
    VulkanImage* materialMsaa   = vulkanFrameResourcesGetMaterialMsaa();
    VulkanImage* depthImage     = vulkanFrameResourcesGetDepth();
    VulkanImage* depthImageMsaa = vulkanFrameResourcesGetDepthMsaa();

    if (!sceneColor || !normals || !material || !depthImage) return;

    // Push constants: none needed — all data comes from global set 0
    // (sceneBuffer, materialBuffer, shadows, IBL via globalset.shader)

    vulkanBeginRender(.cmd        = cmd,
                      .pipe       = &terrainPipe,
                      .color1     = sceneColor,
                      .msaaColor1 = sceneColorMsaa,
                      .color2     = normals,
                      .msaaColor2 = normalsMsaa,
                      .color3     = material,
                      .msaaColor3 = materialMsaa,
                      .depth      = depthImage,
                      .msaaDepth  = depthImageMsaa);

    vulkanViewport(cmd, 0, sceneColor->extent.height, sceneColor->extent.width,
                   -((i32)sceneColor->extent.height));
    vulkanScissor(cmd, 0, 0, sceneColor->extent.width, sceneColor->extent.height);

    vulkanBindPipe(cmd, &terrainPipe);

    // Draw visible chunks from all terrains
    for (u32 ti = 0; ti < arraySize(ecs.terrains); ti++) {
        Terrain* terrain = ecs.terrains[ti];
        if (!terrain || !terrain->backendData) continue;

        VulkanTerrain* vt = (VulkanTerrain*)terrain->backendData;
        if (!vt->uploaded) continue;

        vulkanBindVertex(cmd, &vt->vertexBuffer, 0, NULL, 0, NULL, 0);
        vulkanBindIndex(cmd, &vt->indexBuffer, 0, VK_INDEX_TYPE_UINT32);

        for (u32 ci = 0; ci < vt->chunkCount; ci++) {
            TerrainChunkDraw* chunk = &vt->chunks[ci];

            // Visibility precomputed by TerrainSystem
            if (!terrain->chunks[ci].visible) continue;

            // Push material ID for splatmap lookup
            TerrainPushConstants pc = {.materialId = chunk->materialId, .wireFrame = 0};
            vulkanPush(cmd, &terrainPipe, sizeof(pc), &pc);

            vkCmdDrawIndexed(cmd->cmd,
                             chunk->indexCount,
                             1,
                             chunk->firstIndex,
                             chunk->vertexOffset,
                             0);

            renderer.drawCalls++;
            renderer.instanceCount++;
            renderer.triangleCount += chunk->indexCount / 3;
        }
    }

    vulkanEndRender(cmd);
}

static void postUpdate(void) {}

static void removed(void) {
    vulkanDestroyPipe(&terrainPipe);
    vulkanTerrainHeightBakerDestroy();
}
