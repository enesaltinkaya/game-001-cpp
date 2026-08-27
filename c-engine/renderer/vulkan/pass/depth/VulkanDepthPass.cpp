#include "VulkanDepthPass.h"
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

namespace engine {
static void recreatePipelines(void);

static double elapsedCPU;
static double elapsedGPU;

VulkanDepthPass vulkanDepthPass;

VulkanDepthPass::VulkanDepthPass() : System("depth") {}

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
        .colorFormat3       = VK_FORMAT_R16G16B16A16_SFLOAT,
        .clearColor1        = {0, 0, 0, 0}, .clearColor1Enabled = 1,
        .clearColor2        = {0, 0, 0, 0}, .clearColor2Enabled = 1,
        .clearColor3        = {0, 0, 0, 0}, .clearColor3Enabled = 1,
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
        .colorFormat3       = VK_FORMAT_R16G16B16A16_SFLOAT,
        .depthFormat        = VK_FORMAT_D32_SFLOAT,
        .noCull             = 1,
        .vertexAttributes   = sceneVertexAttrs,
        .vertexAttributeCount = 6,
        .vertexBindings     = &sceneVertexBinding,
        .vertexBindingCount = 1);

    // Azgaar water depth/velocity pre-pass: animated water surface needs
    // motion vectors for FSR. Uses a water-specific vertex shader that
    // reproduces the camera-snapped grid, the depth-attenuated wave
    // displacement and the color pass' dry-land cull (depth-buffer
    // sampling + fragment early-out), so the swell never leaks onto the
    // beach or the player character.
    //
    // It runs in its OWN render pass (see update()): no depth attachment —
    // the scene depth buffer is sampled as a texture in the VS/FS, which is
    // only possible once it is no longer this pass' depth attachment.
    // The velocity / view-normal attachments are LOADed (no clears): the
    // scene / heightmap / props pre-passes already wrote their motion
    // vectors; water only overwrites the pixels where it is actually drawn.
    waterDepthPipe = vulkanCreatePipe(
        .name               = "azgaar_water_depth_prepass",
        .vs                 = "shaders/pass/azgaar_water/spv/azgaar_water_depth.vert.spv",
        .fs                 = "shaders/pass/azgaar_water/spv/azgaar_water_depth.frag.spv",
        .colorFormat1       = VK_FORMAT_R16G16_SFLOAT,
        .colorFormat2       = VK_FORMAT_R16G16_SNORM,
        .colorFormat3       = VK_FORMAT_R16G16B16A16_SFLOAT,
        .clearColor1        = {0, 0, 0, 0}, .clearColor1Enabled = 0,
        .clearColor2        = {0, 0, 0, 0}, .clearColor2Enabled = 0,
        .clearColor3        = {0, 0, 0, 0}, .clearColor3Enabled = 0,
        .noCull             = 1,
        .vertexAttributes   = terrainVertexAttrs,
        .vertexAttributeCount = 4,
        .vertexBindings     = &sceneVertexBinding,
        .vertexBindingCount = 1);
}

// Must match the GLSL WaterPushConstants in azgaar_water_depth.vert/.frag
// (identical payload to the color pass' WaterPushConstants).
typedef struct WaterDepthPushConstants {
    u32 depthIndex;
    u32 width;
    u32 height;
    float nearZ;
    float farZ;
    float projM00;
    float projM11;
    float projM20;
    float projM21;
} WaterDepthPushConstants;

void VulkanDepthPass::added() {
    utils::signalSubscribe("swapchainCreated", swapchainCreated);
    recreatePipelines();
}

void VulkanDepthPass::preUpdate() {
    if (vulkan.skipFrame) return;

    VulkanImage* depthImage      = vulkanFrameResourcesGetDepth();
    VulkanImage* velocityImage   = vulkanFrameResourcesGetVelocity();
    VulkanImage* viewNormalImage = vulkanFrameResourcesGetViewNormal();
    VulkanImage* worldNormalImage = vulkanFrameResourcesGetWorldNormal();
    VulkanImage* sceneColorImage = vulkanFrameResourcesGetSceneColor();
    VulkanImage* normalsImage    = vulkanFrameResourcesGetNormals();
    VulkanImage* materialImage   = vulkanFrameResourcesGetMaterial();

    if (depthImage)     vulkanTransition(vulkan.currentCmd, depthImage,     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, 1);
    if (velocityImage)  vulkanTransition(vulkan.currentCmd, velocityImage,  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    if (viewNormalImage) vulkanTransition(vulkan.currentCmd, viewNormalImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    if (worldNormalImage) vulkanTransition(vulkan.currentCmd, worldNormalImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    if (sceneColorImage) vulkanTransition(vulkan.currentCmd, sceneColorImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    if (normalsImage)   vulkanTransition(vulkan.currentCmd, normalsImage,   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    if (materialImage)  vulkanTransition(vulkan.currentCmd, materialImage,  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);

    vulkanResetProfile(vulkan.currentCmd, &depthPipe.profile, 0);
}

void VulkanDepthPass::update() {
    VulkanImage* depthImage      = vulkanFrameResourcesGetDepth();
    VulkanImage* velocityImage   = vulkanFrameResourcesGetVelocity();
    VulkanImage* viewNormalImage = vulkanFrameResourcesGetViewNormal();
    VulkanImage* worldNormalImage = vulkanFrameResourcesGetWorldNormal();

    if (vulkan.skipFrame || !depthImage) return;

    VulkanCommand* cmd = vulkan.currentCmd;
    u32 fi             = renderer.flightIndex;

    vulkanBeginProfile(cmd, &depthPipe.profile, 0);

    u32 visibleSceneCount = 0;
    const Scene** visibleScenes = vulkanGetVisibleScenes(&visibleSceneCount);

    vulkanBeginRender(.cmd = cmd, .pipe = &depthPipe,
                      .color1 = velocityImage,
                      .color2 = viewNormalImage,
                      .color3 = worldNormalImage,
                      .depth  = depthImage);

    vulkanViewport(cmd, 0, depthImage->extent.height, depthImage->extent.width, -((i32)depthImage->extent.height));
    vulkanScissor(cmd, 0, 0, depthImage->extent.width, depthImage->extent.height);

    for (u32 si = 0; si < visibleSceneCount; si++) {
        const Scene* scene = visibleScenes[si];
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
    // that downstream passes (contact shadows, HiZ, shadows) and FSR see
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

    if (cam && heightmapBackend) {
        // Heightmap terrain (Azgaar world): the heightmap pass renders
        // its implicit grid into the same depth/velocity/view-normal
        // attachments with its own pre-pass pipe (per-tile height textures),
        // so downstream passes and FSR see the heightmap surface. This gives
        // FSR valid per-pixel motion vectors without a full-mesh terrain.
        vulkanHeightmapTerrainDrawPrepass();
    }

    // Azgaar props need motion vectors for FSR; the prepass renders the
    // animated (wind-swayed) props into the velocity / view-normal
    // attachments and writes their depth so pre-colour consumers (contact
    // shadow, HiZ) see prop geometry.  No-op when props are not
    // active.
    vulkanAzgaarPropsDrawPrepass();

    vulkanEndRender(cmd);

    // ── Azgaar water depth/velocity (own render pass) ────────────────────
    // The water surface' motion vectors must match the surface the color
    // pass displays: the same depth-attenuated swell and the same
    // "only where water is drawn" dry-land early-out.  Both need the scene
    // depth buffer as a *sampled texture*, so the water pre-pass runs in
    // its own render pass (no depth attachment) after the depth buffer is
    // fully populated by the scene / heightmap / props draws above.
    // Skipped entirely when no water can be on screen (disabled,
    // underwater camera, or frustum fully above the sea plane).
    if (cam && hasWater && vulkanAzgaarWaterIsVisible()) {
        // Depth buffer: attachment (write) → sampled texture (read).
        // The transition barrier's dst stages (fragment/compute) do not
        // cover the vertex stage, and the water VS also samples the depth
        // buffer (terrainHeightAt for the swell attenuation): make the
        // depth writes of the pass above explicitly visible to the VS.
        {
            VkMemoryBarrier2 depthReadBarrier = {};
            depthReadBarrier.sType            = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            depthReadBarrier.srcStageMask     = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            depthReadBarrier.srcAccessMask    = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            depthReadBarrier.dstStageMask     = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            depthReadBarrier.dstAccessMask    = VK_ACCESS_2_SHADER_READ_BIT;

            VkDependencyInfo depthReadDep = {};
            depthReadDep.sType            = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            depthReadDep.memoryBarrierCount = 1;
            depthReadDep.pMemoryBarriers  = &depthReadBarrier;
            vkCmdPipelineBarrier2(cmd->cmd, &depthReadDep);
        }
        vulkanTransition(cmd, depthImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

        // Velocity / view-normal: LOAD the data the previous pre-passes
        // wrote; water overwrites only its own (submerged) pixels.
        vulkanBeginRender(.cmd = cmd,
                          .pipe = &waterDepthPipe,
                          .color1 = velocityImage,
                          .color2 = viewNormalImage,
                          .color3 = worldNormalImage);

        vulkanViewport(cmd, 0, depthImage->extent.height, depthImage->extent.width,
                       -((i32)depthImage->extent.height));
        vulkanScissor(cmd, 0, 0, depthImage->extent.width, depthImage->extent.height);

        vulkanBindPipe(cmd, &waterDepthPipe);
        vulkanBindVertex(cmd, waterVbo, 0, NULL, 0, NULL, 0);
        vulkanBindIndex(cmd, waterIbo, 0, VK_INDEX_TYPE_UINT32);

        // Same push-constant payload the color pass pushes: lets the VS/FS
        // sample the scene depth and reconstruct the terrain height exactly
        // like azgaar_water.vert/.frag (jitter-corrected projection).
        WaterDepthPushConstants pc = {
            .depthIndex = (u32)depthImage->sampledPoolIndex,
            .width      = depthImage->extent.width,
            .height     = depthImage->extent.height,
            .nearZ      = cam->znear,
            .farZ       = cam->zfar,
            .projM00    = cam->cameraUbo.projection[0][0],
            .projM11    = cam->cameraUbo.projection[1][1],
            .projM20    = cam->cameraUbo.projection[2][0],
            .projM21    = cam->cameraUbo.projection[2][1],
        };
        vulkanPush(cmd, &waterDepthPipe, sizeof(pc), &pc);

        vkCmdDrawIndexed(cmd->cmd, waterIndexCount, 1, 0, 0, 0);

        vulkanEndRender(cmd);

        // Restore the layout contract downstream pre-passes rely on: the
        // HiZ and occlusion passes hardcode the scene-depth transition
        // ATTACHMENT → SHADER_READ (raw barriers, bypassing the VulkanImage
        // layout tracker), so the depth image must be back in
        // DEPTH_STENCIL_ATTACHMENT_OPTIMAL once the water pre-pass is done.
        vulkanTransition(cmd, depthImage, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, 1);
    }

    // Depth + color attachment write -> subsequent read barrier
    // (covers both the scene/terrain/props pass and the water pre-pass).
    VkMemoryBarrier2 barriers[2] = {
        {
            .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .pNext         = nullptr,
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
            .pNext         = nullptr,
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

void VulkanDepthPass::postUpdate() {
    vulkanDepthPass.cpuElapsed = elapsedCPU;
    vulkanDepthPass.gpuElapsed = elapsedGPU;
}

void VulkanDepthPass::removed() {
    vulkanDestroyPipe(&depthPipe);
    vulkanDestroyPipe(&depthPipeDoubleSided);
    vulkanDestroyPipe(&waterDepthPipe);
}
}  // namespace engine
