#include "VulkanDepthPass.h"
#include "ecs/Ecs.h"
#include "ecs/system/scene/Scene.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "events/Events.h"
#include "renderer/Renderer.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/scene/VulkanScene.h"
#include "renderer/vulkan/scene/VulkanVisibleScenes.h"
#include "renderer/vulkan/pass/heightmap_terrain/VulkanHeightmapTerrainPass.h"
#include "ecs/system/heightmap/HeightmapTerrain.h"
#include "renderer/vulkan/pass/azgaar_water/VulkanAzgaarWaterPass.h"
#include "renderer/vulkan/pass/azgaar_props/VulkanAzgaarPropsPass.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"

static void added(void);
static void preUpdate(void);
static void update(void);
static void postUpdate(void);
static void removed(void);
static void recreatePipelines(void);

static double elapsedCPU;
static double elapsedGPU;

System vulkanDepthPass = {
    .name       = "depth",
    .added      = added,
    .preUpdate  = preUpdate,
    .update     = update,
    .postUpdate = postUpdate,
    .removed    = removed,
};

static VulkanPipe depthPipe;
static VulkanPipe depthPipeDoubleSided;
static VulkanPipe waterDepthPipe;

typedef struct DepthPushConstants {
    u64 transformBufferAddress;
    u64 prevTransformBufferAddress;
    u64 drawInstanceBufferAddress;
    u64 culledBufferAddress;
    u64 jointMatrixBufferAddress;
    u64 entitySkinMapBufferAddress;
    u64 prevJointMatrixBufferAddress;
} DepthPushConstants;

// Vertex input description for SceneVertex (48 bytes)
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
    {.location = 4, .binding = 0, .format = VK_FORMAT_R32_UINT,            .offset = 48},  // joints
    {.location = 5, .binding = 0, .format = VK_FORMAT_R32_UINT,            .offset = 52},  // weights
};

// Terrain shaders don't consume joints/weights — use a subset to avoid validation errors
static VkVertexInputAttributeDescription terrainVertexAttrs[] = {
    {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,    .offset = 0},
    {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,    .offset = 12},
    {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 24},
    {.location = 3, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,       .offset = 40},
};

static void swapchainCreated(void*) {
    recreatePipelines();
}

static void recreatePipelines(void) {
    if (depthPipe.pipe) {
        vulkanDestroyPipe(&depthPipe);
        vulkanDestroyPipe(&depthPipeDoubleSided);
        vulkanDestroyPipe(&waterDepthPipe);
    }

    depthPipe = vulkanCreatePipe(
        .name               = "scene_depth_prepass",
        .vs                 = "shaders/pass/scene/spv/scene_depth.vert.spv",
        .fs                 = "shaders/pass/scene/spv/scene_depth.frag.spv",
        .colorFormat1       = VK_FORMAT_R16G16_SFLOAT,
        .colorFormat2       = VK_FORMAT_R16G16_SNORM,
        .clearColor1        = {0, 0, 0, 0}, .clearColor1Enabled = 1,
        .clearColor2        = {0, 0, 0, 0}, .clearColor2Enabled = 1,
        .depthFormat        = VK_FORMAT_D32_SFLOAT,
        .clearDepth         = {0, 0}, .clearDepthEnabled = 1,
        .vertexAttributes   = sceneVertexAttrs,
        .vertexAttributeCount = 6,
        .vertexBindings     = &sceneVertexBinding,
        .vertexBindingCount = 1);

    depthPipeDoubleSided = vulkanCreatePipe(
        .name               = "scene_depth_prepass_ds",
        .vs                 = "shaders/pass/scene/spv/scene_depth.vert.spv",
        .fs                 = "shaders/pass/scene/spv/scene_depth.frag.spv",
        .colorFormat1       = VK_FORMAT_R16G16_SFLOAT,
        .colorFormat2       = VK_FORMAT_R16G16_SNORM,
        .depthFormat        = VK_FORMAT_D32_SFLOAT,
        .noCull             = 1,
        .vertexAttributes   = sceneVertexAttrs,
        .vertexAttributeCount = 6,
        .vertexBindings     = &sceneVertexBinding,
        .vertexBindingCount = 1);

    // Azgaar water depth/velocity pre-pass: animated water surface needs
    // motion vectors for FSR. Uses a water-specific vertex shader that
    // reproduces the camera-snapped grid and wave displacement.
    waterDepthPipe = vulkanCreatePipe(
        .name               = "azgaar_water_depth_prepass",
        .vs                 = "shaders/pass/azgaar_water/spv/azgaar_water_depth.vert.spv",
        .fs                 = "shaders/pass/azgaar_water/spv/azgaar_water_depth.frag.spv",
        .colorFormat1       = VK_FORMAT_R16G16_SFLOAT,
        .colorFormat2       = VK_FORMAT_R16G16_SNORM,
        .depthFormat        = VK_FORMAT_D32_SFLOAT,
        .depthFormat        = VK_FORMAT_D32_SFLOAT,
        .noCull             = 1,
        .depthTestOnly      = 1,
        .depthCompareOp     = VK_COMPARE_OP_GREATER_OR_EQUAL,
        .vertexAttributes   = terrainVertexAttrs,
        .vertexAttributeCount = 4,
        .vertexBindings     = &sceneVertexBinding,
        .vertexBindingCount = 1);
}

static void added(void) {
    signalSubscribe("swapchainCreated", swapchainCreated);
    recreatePipelines();
}

static void preUpdate(void) {
    if (vulkan.skipFrame) return;

    VulkanImage* depthImage      = vulkanFrameResourcesGetDepth();
    VulkanImage* velocityImage   = vulkanFrameResourcesGetVelocity();
    VulkanImage* viewNormalImage = vulkanFrameResourcesGetViewNormal();
    VulkanImage* sceneColorImage = vulkanFrameResourcesGetSceneColor();
    VulkanImage* normalsImage    = vulkanFrameResourcesGetNormals();
    VulkanImage* materialImage   = vulkanFrameResourcesGetMaterial();

    if (depthImage)     vulkanTransition(vulkan.currentCmd, depthImage,     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, 1);
    if (velocityImage)  vulkanTransition(vulkan.currentCmd, velocityImage,  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    if (viewNormalImage) vulkanTransition(vulkan.currentCmd, viewNormalImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    if (sceneColorImage) vulkanTransition(vulkan.currentCmd, sceneColorImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    if (normalsImage)   vulkanTransition(vulkan.currentCmd, normalsImage,   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    if (materialImage)  vulkanTransition(vulkan.currentCmd, materialImage,  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);

    vulkanResetProfile(vulkan.currentCmd, &depthPipe.profile, 0);
}

static void update(void) {
    VulkanImage* depthImage      = vulkanFrameResourcesGetDepth();
    VulkanImage* velocityImage   = vulkanFrameResourcesGetVelocity();
    VulkanImage* viewNormalImage = vulkanFrameResourcesGetViewNormal();

    if (vulkan.skipFrame || !depthImage) return;

    VulkanCommand* cmd = vulkan.currentCmd;
    u32 fi             = renderer.flightIndex;

    vulkanBeginProfile(cmd, &depthPipe.profile, 0);

    u32 visibleSceneCount = 0;
    Scene** visibleScenes = vulkanGetVisibleScenes(&visibleSceneCount);

    vulkanBeginRender(.cmd = cmd, .pipe = &depthPipe,
                      .color1 = velocityImage,
                      .color2 = viewNormalImage,
                      .depth  = depthImage);

    vulkanViewport(cmd, 0, depthImage->extent.height, depthImage->extent.width, -((i32)depthImage->extent.height));
    vulkanScissor(cmd, 0, 0, depthImage->extent.width, depthImage->extent.height);

    for (u32 si = 0; si < visibleSceneCount; si++) {
        Scene* scene = visibleScenes[si];
        if (!scene->backendScene) continue;
        VulkanScene* vs  = static_cast<VulkanScene*>(scene->backendScene);
        if (!vs->totalDraws) continue;

        vulkanBindVertex(cmd, &vs->vertexBuffer, 0, NULL, 0, NULL, 0);
        vulkanBindIndex(cmd, &vs->indexBuffer, 0, VK_INDEX_TYPE_UINT32);

        // Single-sided draws
        vulkanBindPipe(cmd, &depthPipe);
        {
            DepthPushConstants pc = {
                .transformBufferAddress     = vs->transformBuffer[fi].address,
                .prevTransformBufferAddress = vs->prevTransformBuffer[fi].address,
                .drawInstanceBufferAddress  = vs->drawInstanceBuffer.address,
                .culledBufferAddress        = vs->culledBuffer[fi].address,
                .jointMatrixBufferAddress   = vs->jointMatrixBuffer[fi].address,
                .entitySkinMapBufferAddress = vs->entitySkinMapBuffer[fi].address,
                .prevJointMatrixBufferAddress = vs->prevJointMatrixBuffer[fi].address,
            };
            vulkanPush(cmd, &depthPipe, sizeof(pc), &pc);

            vkCmdDrawIndexedIndirectCount(
                cmd->cmd,
                vs->indirectBuffer[fi].buf, 0,
                vs->drawCountBuffer[fi].buf, 0,
                vs->totalDraws,
                sizeof(SceneDrawIndexedCommand));
        }

        // Double-sided draws
        vulkanBindPipe(cmd, &depthPipeDoubleSided);
        {
            DepthPushConstants pc = {
                .transformBufferAddress     = vs->transformBuffer[fi].address,
                .prevTransformBufferAddress = vs->prevTransformBuffer[fi].address,
                .drawInstanceBufferAddress  = vs->drawInstanceBuffer.address,
                .culledBufferAddress        = vs->dsCulledBuffer[fi].address,
                .jointMatrixBufferAddress   = vs->jointMatrixBuffer[fi].address,
                .entitySkinMapBufferAddress = vs->entitySkinMapBuffer[fi].address,
                .prevJointMatrixBufferAddress = vs->prevJointMatrixBuffer[fi].address,
            };
            vulkanPush(cmd, &depthPipeDoubleSided, sizeof(pc), &pc);

            vkCmdDrawIndexedIndirectCount(
                cmd->cmd,
                vs->dsIndirectBuffer[fi].buf, 0,
                vs->dsDrawCountBuffer[fi].buf, 0,
                vs->totalDraws,
                sizeof(SceneDrawIndexedCommand));
        }

        // Alpha-blended (transparent) draws: the OIT pass draws these with
        // depth test but no depth write (depthTestOnly), so their depth
        // never reached the depth buffer.  Draw them here (depth writes on)
        // so the depth buffer reflects the true nearest surface, including
        // transparent leaves.  The composite pass' screen-space fog
        // reconstructs worldPos from this depth; without leaf depth, leaf
        // pixels either read as sky (depth 0 → isSky → fog skipped) or use
        // the stale depth of the opaque surface behind the leaves, so the
        // leaves escaped fog while the trunks did not.
        vulkanBindPipe(cmd, &depthPipe);
        {
            DepthPushConstants pc = {
                .transformBufferAddress     = vs->transformBuffer[fi].address,
                .prevTransformBufferAddress = vs->prevTransformBuffer[fi].address,
                .drawInstanceBufferAddress  = vs->drawInstanceBuffer.address,
                .culledBufferAddress        = vs->transCulledBuffer[fi].address,
                .jointMatrixBufferAddress   = vs->jointMatrixBuffer[fi].address,
                .entitySkinMapBufferAddress = vs->entitySkinMapBuffer[fi].address,
                .prevJointMatrixBufferAddress = vs->prevJointMatrixBuffer[fi].address,
            };
            vulkanPush(cmd, &depthPipe, sizeof(pc), &pc);

            vkCmdDrawIndexedIndirectCount(
                cmd->cmd,
                vs->transIndirectBuffer[fi].buf, 0,
                vs->transDrawCountBuffer[fi].buf, 0,
                vs->totalDraws,
                sizeof(SceneDrawIndexedCommand));
        }
    }

    // ── Heightmap / water depth ────────────────────────────────────────
    // Render the streaming heightmap surface (Azgaar world) and the animated
    // water grid into the same depth / velocity / view-normal attachments so
    // that downstream passes (GTAO, contact shadows, HiZ, shadows) and FSR see
    // their depth.  No color clears — we append to the existing attachments
    // that already contain scene-object data.
Entity* camEntity = cameraGetEntity();
    Camera* cam = camEntity ? getComponent(camEntity->scene, Camera, camEntity->id) : NULL;

    // Heightmap terrain (Azgaar world): the heightmap pass renders its own
    // implicit-grid pre-pass draws (see below).
    bool heightmapBackend = heightmapTerrainGetActive() != NULL;

    VulkanBuffer* waterVbo = NULL;
    VulkanBuffer* waterIbo = NULL;
    u32 waterVertexCount = 0;
    u32 waterIndexCount = 0;
    bool hasWater = vulkanAzgaarWaterGetGpuMesh(&waterVbo, &waterIbo,
                                                &waterVertexCount, &waterIndexCount);

    if (cam && (hasWater || heightmapBackend)) {
        // Heightmap terrain (Azgaar world): the heightmap pass renders
        // its implicit grid into the same depth/velocity/view-normal
        // attachments with its own pre-pass pipe (per-tile height textures),
        // so downstream passes and FSR see the heightmap surface. This gives
        // FSR valid per-pixel motion vectors without a full-mesh terrain.
        if (heightmapBackend) {
            vulkanHeightmapTerrainDrawPrepass();
        }

        // Azgaar water needs motion vectors for FSR; render the animated grid
        // into the velocity / view-normal attachments using a water-specific
        // depth/velocity shader that reproduces the vertex displacement.
        // Skipped entirely when no water can be on screen (disabled,
        // underwater camera, or frustum fully above the sea plane).
        if (hasWater && vulkanAzgaarWaterIsVisible()) {
            vulkanBindPipe(cmd, &waterDepthPipe);
            vulkanBindVertex(cmd, waterVbo, 0, NULL, 0, NULL, 0);
            vulkanBindIndex(cmd, waterIbo, 0, VK_INDEX_TYPE_UINT32);
            // Water grid is a single draw; depth test is kept to avoid writing
            // over opaque geometry, but depth writes are disabled in the
            // waterDepthPipe's depth state to match the water render pass.
            vkCmdDrawIndexed(cmd->cmd, waterIndexCount, 1, 0, 0, 0);
        }
    }

    // Azgaar props need motion vectors for FSR; the prepass renders the
    // animated (wind-swayed) props into the velocity / view-normal
    // attachments.  No-op when props are not active.
    vulkanAzgaarPropsDrawPrepass();

    vulkanEndRender(cmd);

    // Depth + color attachment write -> subsequent read barrier
    VkMemoryBarrier2 barriers[2] = {
        {
            .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            .srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_2_SHADER_READ_BIT,
        },
        {
            .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
        },
    };
    VkDependencyInfo depInfo   = {};
    depInfo.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    depInfo.memoryBarrierCount = 2;
    depInfo.pMemoryBarriers    = barriers;
    vkCmdPipelineBarrier2(cmd->cmd, &depInfo);

    vulkanEndProfile(cmd, &depthPipe.profile, 0);
    elapsedGPU = depthPipe.profile.elapsed;
}

static void postUpdate(void) {
    vulkanDepthPass.cpuElapsed = elapsedCPU;
    vulkanDepthPass.gpuElapsed = elapsedGPU;
}

static void removed(void) {
    vulkanDestroyPipe(&depthPipe);
    vulkanDestroyPipe(&depthPipeDoubleSided);
    vulkanDestroyPipe(&waterDepthPipe);
}
