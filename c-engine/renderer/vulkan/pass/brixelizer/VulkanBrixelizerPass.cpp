#include "VulkanBrixelizerPass.h"
#include "ecs/Ecs.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/scene/Scene.h"
#include "ecs/system/scene/SceneSystem.h"
#include "ecs/system/heightmap/HeightmapTerrain.h"
#include "ecs/system/transform/TransformComponent.h"
#include "ecs/system/window/WindowSystem.h"
#include "events/Events.h"
#include "futuretask/FutureTask.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/barrier/VulkanBarrier.h"
#include "renderer/vulkan/pipeline/VulkanProfile.h"
#include "renderer/vulkan/resources/VulkanBuffer.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/scene/VulkanScene.h"
#include "renderer/vulkan/utils/VulkanFfxUtils.h"
#include "timer/Timer.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function" /* ffx_core_cpu.h static inlines */
#include <FidelityFX/host/ffx_brixelizer.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#pragma GCC diagnostic pop
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <vector>

namespace engine {
    /* Static-only cascade layout (Steps 1–9; Step 10 switches to the sample's
     * 3-per-level layout): 8 cascades, voxel size doubling per level —
     * 2, 4, …, 256 m. The far cascade spans 256 m × 64 bricks = 16.4 km,
     * which covers the 10.24 km heightmap streaming window. */
    static const u32 BRIX_NUM_CASCADES      = 8;
    static const float BRIX_BASE_VOXEL_SIZE = 2.0f;
    /* Bake budgets from the FFX sample (tuned in Step 9). The scratch buffer must
     * hold the reference/swap partitions sized by those budgets (~892 MB at the
     * sample values) — the sample allocates 1 GiB, which we match. */
    static const u32 BRIX_MAX_REFERENCES      = 32u * (1u << 20);
    static const u32 BRIX_TRIANGLE_SWAP_SIZE  = 300u * (1u << 20);
    static const u32 BRIX_MAX_BRICKS_PER_BAKE = 1u << 14;
    static const u64 BRIX_GPU_SCRATCH_SIZE    = 1u << 30;
    /* SceneVertex stride (56 B) — the voxelizer fetches positions from offset 0
     * at this stride; the index buffer is u32 (4 B per index). */
    static const u32 BRIX_SCENE_VERTEX_STRIDE = 56;

    static void swapchainCreated(void* _);
    static void destroyResources(void);
    static void destroyContext(void);
    static char createResources(void);
    static char ensureContext(void);
    static void entityWorldTransform(Scene* scene, u32 entityId, versor outRot, vec3 outPos, float* outScale);
    static char registerScene(Scene* scene);
    static void brixelizerSceneCreateTask(void* pScene);
    static FfxBrixelizerTraceDebugModes getSdfDebugMode(void);

    static double elapsedCPU;
    static double elapsedGPU;

    /* Step 4: terrain SDF tiles. The heightmap terrain has no mesh; when a
     * HeightmapTile reaches READY (polled via heightmapTerrainSnapshotTiles,
     * the same mechanism AzgaarProps uses) a decimated position-only grid is
     * generated from its CPU height grid and registered as one static
     * instance (identity transform — the positions are already world-space).
     * Evicted / regenerated tiles delete their instance; the GPU buffers are
     * destroyed a few frames later so in-flight FFX dispatches keep a valid
     * handle (the bindless slot may be re-registered meanwhile). */
    struct BrixelTerrainTile {
        i32 tileX, tileZ;
        u64 readyStamp;
        char inUse;
        VulkanBuffer vertBuf;
        VulkanBuffer idxBuf;
        u32 vertBufIdx;
        u32 idxBufIdx;
        FfxBrixelizerInstanceID instanceID;
        char instanceCreated;
    };
    struct BrixelTerrainDeferred {
        VulkanBuffer vertBuf;
        VulkanBuffer idxBuf;
        u32 vertBufIdx;
        u32 idxBufIdx;
        char unreg;
        u32 framesLeft;
    };
    static std::vector<BrixelTerrainTile> terrainTiles;
    static std::vector<BrixelTerrainDeferred> terrainDeferred;
    static HeightmapTerrain* terrainHt;
    static u32 terrainRes;
    static char terrainResSet;
    static char terrainStatsLogged;
    static void terrainSyncTiles(void);
    static void terrainTileEvict(BrixelTerrainTile* e);
    static char terrainTileCreate(const HeightmapTileView* v);
    static void terrainClearAll(void);

    /* FFX backend shared by the brixelizer + GI contexts (scratch sized for
     * 2; the FSR/CACAO/LPM passes keep their own single-context interfaces). */
    static void* scratchBuffer;
    static size_t scratchBufferSize;
    static FfxInterface backendInterface;
    static char backendReady;

    /* Voxelizer context + engine-owned SDF resources (table in
     * plans/brixelizer-gi.md). */
    static FfxBrixelizerContext brixelizerContext;
    static char contextReady;
    static VulkanImage sdfAtlas;
    static VulkanBuffer brickAABBs;
    static VulkanBuffer cascadeAABBTrees[FFX_BRIXELIZER_MAX_CASCADES];
    static VulkanBuffer cascadeBrickMaps[FFX_BRIXELIZER_MAX_CASCADES];
    static VulkanBuffer gpuScratch;
    /* Step 2: SDF debug visualization target (render-res R16F RGBA, written
     * by the FFX debug pass as a UAV, dumped via brixelSdf). */
    static VulkanImage sdfDebug;

    /* Step 3: registered scene geometry. One entry per scene (its VBO/IBO
     * registered once, one static instance per non-skinned draw). The FFX
     * instance/buffer tables die with the context, so the list is cleared on
     * context destroy and rebuilt by ensureContext (all ecs.scenes) or by the
     * per-scene create hook. */
    struct BrixelSceneReg {
        Scene* scene;
        VulkanScene* vs;
        u32 vertBufIdx;
        u32 idxBufIdx;
        std::vector<FfxBrixelizerInstanceID> instanceIDs;
        u32 instanceCount;
        u32 triangleCount;
    };
    static std::vector<BrixelSceneReg> sceneRegs;
    static u32 totalRegisteredInstances;
    static u32 totalRegisteredTriangles;
    /* First-bake cost tracking (Gate 3): the cascade round-robin (lowest set
     * bit of the frame index) visits cascade 7 only once per 256 updates,
     * so the far cascades' first turns — which carry the whole scene on a
     * fresh SDF — land inside a 256-update window after registration. The
     * heaviest single update in that window is the first-bake proxy (the
     * per-update brick budget caps each turn; the rest spreads across later
     * turns of the same cascade). */
    static const u32 BRIX_FIRST_BAKE_WINDOW = 256;
    static u32 regBakeFrame;
    static u32 regBakeLogged;
    static char regBakeActive;
    static u64 regBakeGpuTotal;
    static double regBakeGpuMax;

    /* Debug visualization mode (ENGINE_BRIX_SDF_DEBUG=distance|grad|brick|
     * cascade|uvw|iter; default distance). Step 9 moves this to the GUI. */
    static FfxBrixelizerTraceDebugModes sdfDebugMode;
    static char sdfDebugModeSet;
    static char sdfDebugEnabled = 1;
    static char statsTrisLogged;
    /* Debug ray-march range (ENGINE_BRIX_SDF_TMAX, default the sample's
     * 10000). The distance view normalizes hit distance by tMax, so near
     * objects need a smaller value to show a visible band. */
    static float sdfDebugTMax;
    static char sdfDebugTMaxSet;
    /* ~8 MB (wchar-inflated on Linux) — file scope, not stack. */
    static FfxBrixelizerBakedUpdateDescription bakedUpdateDesc;
    static u32 frameIndex;
    static FfxBrixelizerStats stats;
    static char statsLiveLogged;
    static VulkanProfile profile;
    static char profileReady;

    VulkanBrixelizerPass vulkanBrixelizerPass;

    VulkanBrixelizerPass::VulkanBrixelizerPass() : System("brixelizer") {}

    void VulkanBrixelizerPass::added() {
        utils::signalSubscribe("swapchainCreated", swapchainCreated);
        profile      = vulkanCreateProfile("brixelizer");
        profileReady = 1;
    }

    void VulkanBrixelizerPass::preUpdate() {
        if (profileReady) {
            /* force: the stats GUI is usually closed, but the brixelizer cost is
             * tracked in the log until Step 9 moves tuning to the GUI. */
            vulkanResetProfile(vulkan.currentCmd, &profile, 1);
        }
        /* Deferred terrain-tile buffer destruction (3 frames past the GPU
         * queue depth, like the heightmap pass's deferred descriptors). */
        for (i32 i = (i32)terrainDeferred.size() - 1; i >= 0; i--) {
            BrixelTerrainDeferred* d = &terrainDeferred[i];
            if (d->framesLeft > 1) {
                d->framesLeft--;
                continue;
            }
            if (d->unreg && contextReady) {
                u32 dropIdx[2] = {d->vertBufIdx, d->idxBufIdx};
                FfxErrorCode unregResult = ffxBrixelizerUnregisterBuffers(&brixelizerContext, dropIdx, 2);
                if (unregResult != FFX_OK) {
                    utils::error("vulkanBrixelizerPass: ffxBrixelizerUnregisterBuffers (terrain) failed: %d",
                                 unregResult);
                }
            }
            if (d->vertBuf.buf) {
                vulkanDestroyBuffer(&d->vertBuf, NULL);
            }
            if (d->idxBuf.buf) {
                vulkanDestroyBuffer(&d->idxBuf, NULL);
            }
            terrainDeferred[(u32)i] = terrainDeferred.back();
            terrainDeferred.pop_back();
        }
    }

    static void swapchainCreated(void* _) {
        (void)_;
        destroyContext();
        destroyResources();
        frameIndex      = 0;
        stats           = FfxBrixelizerStats{};
        statsLiveLogged = 0;
        statsTrisLogged = 0;
        /* The registered-buffer + instance tables live inside the FFX
         * context — both are recreated with it; ensureContext re-registers
         * all scenes on the next update. */
        sceneRegs.clear();
        totalRegisteredInstances   = 0;
        totalRegisteredTriangles   = 0;
        regBakeFrame               = 0;
        regBakeLogged              = 0;
        regBakeActive              = 0;
        regBakeGpuTotal            = 0;
        regBakeGpuMax              = 0.0;
        /* The FFX context (buffer/instance tables) died with the swapchain —
         * drop the terrain registrations and destroy the tile buffers. No
         * unregistration needed: the tables were recreated with the context. */
        terrainClearAll();
    }

    static void terrainClearAll(void) {
        for (u32 i = 0; i < terrainTiles.size(); i++) {
            BrixelTerrainTile* e = &terrainTiles[i];
            if (!e->inUse) {
                continue;
            }
            if (e->vertBuf.buf) {
                vulkanDestroyBuffer(&e->vertBuf, NULL);
                e->vertBuf = VulkanBuffer{};
            }
            if (e->idxBuf.buf) {
                vulkanDestroyBuffer(&e->idxBuf, NULL);
                e->idxBuf = VulkanBuffer{};
            }
            *e = BrixelTerrainTile{};
        }
        terrainTiles.clear();
        for (u32 i = 0; i < terrainDeferred.size(); i++) {
            BrixelTerrainDeferred* d = &terrainDeferred[i];
            if (d->vertBuf.buf) {
                vulkanDestroyBuffer(&d->vertBuf, NULL);
            }
            if (d->idxBuf.buf) {
                vulkanDestroyBuffer(&d->idxBuf, NULL);
            }
        }
        terrainDeferred.clear();
        terrainHt = NULL;
    }

    static void destroyResources(void) {
        if (sdfAtlas.img) {
            vulkanDestroyImage(&sdfAtlas, NULL);
            sdfAtlas = VulkanImage{};
        }
        if (sdfDebug.img) {
            vulkanDestroyImage(&sdfDebug, NULL);
            sdfDebug = VulkanImage{};
        }
        if (brickAABBs.buf) {
            vulkanDestroyBuffer(&brickAABBs, NULL);
            brickAABBs = VulkanBuffer{};
        }
        for (i32 i = 0; i < (i32)FFX_BRIXELIZER_MAX_CASCADES; i++) {
            if (cascadeAABBTrees[i].buf) {
                vulkanDestroyBuffer(&cascadeAABBTrees[i], NULL);
                cascadeAABBTrees[i] = VulkanBuffer{};
            }
            if (cascadeBrickMaps[i].buf) {
                vulkanDestroyBuffer(&cascadeBrickMaps[i], NULL);
                cascadeBrickMaps[i] = VulkanBuffer{};
            }
        }
        if (gpuScratch.buf) {
            vulkanDestroyBuffer(&gpuScratch, NULL);
            gpuScratch = VulkanBuffer{};
        }
    }

    static void destroyContext(void) {
        if (contextReady) {
            ffxBrixelizerContextDestroy(&brixelizerContext);
            brixelizerContext = FfxBrixelizerContext{};
            contextReady      = 0;
        }
    }

    static char createResources(void) {
        /* 512³ R8 SDF atlas: STORAGE (FFX UAV brick writes) + SAMPLED (the GI
         * ray-march reads it in Step 7) + TRANSFER_DST (one-time clear below;
         * history resets in Step 10). Not engine-descriptor-bound, so keep it
         * out of the sampled/storage pools. */
        sdfAtlas =
            vulkanCreateImage(.name     = "BrixelSdfAtlas",
                              .format   = VK_FORMAT_R8_UNORM,
                              .usage    = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              .type     = VK_IMAGE_TYPE_3D,
                              .viewType = VK_IMAGE_VIEW_TYPE_3D,
                              .width    = (int)FFX_BRIXELIZER_STATIC_CONFIG_SDF_ATLAS_SIZE,
                              .height   = (int)FFX_BRIXELIZER_STATIC_CONFIG_SDF_ATLAS_SIZE,
                              .layers   = (int)FFX_BRIXELIZER_STATIC_CONFIG_SDF_ATLAS_SIZE,
                              .noPool   = 1);
        if (!sdfAtlas.img) {
            utils::error("vulkanBrixelizerPass: SDF atlas image creation failed");
            return 0;
        }

        brickAABBs = vulkanCreateGpuBuffer("BrixelBrickAABBs",
                                           FFX_BRIXELIZER_BRICK_AABBS_SIZE,
                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        for (u32 i = 0; i < FFX_BRIXELIZER_MAX_CASCADES; i++) {
            cascadeAABBTrees[i] = vulkanCreateGpuBuffer("BrixelCascadeAABBTrees",
                                                        FFX_BRIXELIZER_CASCADE_AABB_TREE_SIZE,
                                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            /* TRANSFER_SRC: lets a debug readback copy the brick map to a CPU
             * buffer. No VRAM cost. */
            cascadeBrickMaps[i] = vulkanCreateGpuBuffer("BrixelCascadeBrickMaps",
                                                        FFX_BRIXELIZER_CASCADE_BRICK_MAP_SIZE,
                                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        }
        /* TRANSFER_SRC: the FFX backend vkCmdCopyBuffer's job/constant data into
         * the scratch buffer each update (Cauldron's VK backend adds the same
         * flag for its upload buffers). */
        gpuScratch = vulkanCreateGpuBuffer(
            "BrixelGpuScratch",
            BRIX_GPU_SCRATCH_SIZE,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        if (!brickAABBs.buf || !gpuScratch.buf) {
            utils::error("vulkanBrixelizerPass: brixelizer buffer creation failed");
            destroyResources();
            return 0;
        }
        for (u32 i = 0; i < FFX_BRIXELIZER_MAX_CASCADES; i++) {
            if (!cascadeAABBTrees[i].buf || !cascadeBrickMaps[i].buf) {
                utils::error("vulkanBrixelizerPass: cascade buffer creation failed (index %u)", i);
                destroyResources();
                return 0;
            }
        }

        /* SDF debug visualization target (Step 2.2): STORAGE (the FFX debug
         * pass writes it as a UAV) + SAMPLED (dumps / later sampling) +
         * TRANSFER_SRC (vulkanSaveImage dumps it). */
        u32 renderW = window.renderWidth > 0 ? (u32)window.renderWidth : (u32)window.width;
        u32 renderH = window.renderHeight > 0 ? (u32)window.renderHeight : (u32)window.height;
        sdfDebug    = vulkanCreateImage(.name   = "BrixelSdfDebug",
                                        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                        .usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                        .width  = (int)renderW,
                                        .height = (int)renderH);
        if (!sdfDebug.img) {
            utils::error("vulkanBrixelizerPass: brixelizer debug resource creation failed");
            destroyResources();
            return 0;
        }

        /* One-time: SDF atlas clear so pre-bake dumps are predictable
         * (0 = no brick allocated yet); the FFX clear-bricks pass only
         * rewrites bricks that were previously allocated. */
        VulkanCommand* cmd = vulkanTransientBegin();
        vulkanTransition(cmd, &sdfAtlas, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        VkClearColorValue black = {};
        vulkanClearColorImage(cmd, &sdfAtlas, black);
        vulkanTransientEnd(cmd, 1);
        return 1;
    }

    /* World transform of a scene entity, composed from the local Transform
     * components of the entity chain (root → entity). Mirrors
     * TransformSystem::transformSetWorld but does not depend on the
     * 2-second active-entity window: a freshly parsed scene's WorldTransform
     * component is only filled on the next update after load, while the
     * per-node local Transforms are valid from parse time. Registration runs
     * on the main thread (scene-create task / ensureContext), so the ECS
     * reads are safe. */
    static void entityWorldTransform(Scene* scene, u32 entityId, versor outRot, vec3 outPos, float* outScale) {
        Entity* chain[32];
        u32 depth = 0;
        Entity* cur = getEntity(scene, entityId);
        if (!cur) {
            glm_quat_identity(outRot);
            glm_vec4_zero(outPos);
            *outScale = 1.0f;
            return;
        }
        while (cur && depth < 32) {
            chain[depth++] = cur;
            cur            = cur->parent;
        }
        versor wrot;
        glm_quat_identity(wrot);
        vec3 wpos  = {0.0f, 0.0f, 0.0f};
        float wscale = 1.0f;
        for (u32 i = depth; i-- > 0;) { /* root → entity */
            Transform* t = getComponent(scene, Transform, chain[i]->id);
            if (!t) {
                continue;
            }
            /* child world pos = parent pos + parentRot * (parentScale * childPos) */
            vec3 scaled = {t->pos[0] * wscale, t->pos[1] * wscale, t->pos[2] * wscale};
            vec3 rotated;
            glm_quat_rotatev(wrot, scaled, rotated);
            glm_vec3_add(wpos, rotated, wpos);
            versor localRot;
            memcpy(localRot, t->rot, sizeof(localRot));
            glm_quat_mul(wrot, localRot, wrot);
            wscale *= t->pos[3];
        }
        glm_quat_normalize(wrot);
        glm_vec3_copy(wpos, outPos);
        glm_quat_copy(wrot, outRot);
        *outScale = wscale;
    }

    /* Registers a scene's geometry with the voxelizer: its VBO/IBO once,
     * then one static instance per non-skinned draw (the skinned characters
     * come with dynamic geometry in Step 10). Main thread only. */
    static char registerScene(Scene* scene) {
        if (!scene || !scene->backendScene) {
            return 1;
        }
        VulkanScene* vs = static_cast<VulkanScene*>(scene->backendScene);
        if (!vs->vertexBuffer.buf || !vs->indexBuffer.buf || vs->cpuDraws.empty()) {
            return 1;
        }
        for (u32 i = 0; i < sceneRegs.size(); i++) {
            if (sceneRegs[i].scene == scene) {
                return 1; /* already registered with the live context */
            }
        }

        /* PIXEL_COMPUTE_READ like the sample's GetBufferIndex — the FFX
         * backend binds every SRV buffer as a shader storage buffer. The
         * creation usage bits are passed through: FFX keys UAV on
         * VK_BUFFER_USAGE_STORAGE_BUFFER_BIT (added in Step 3.1, pitfall
         * #13). */
        FfxResource vertRes =
            vulkanFfxWrapBufferResource(&vs->vertexBuffer,
                                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ,
                                        L"BrixelSceneVerts");
        FfxResource idxRes =
            vulkanFfxWrapBufferResource(&vs->indexBuffer,
                                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ,
                                        L"BrixelSceneIdx");
        FfxBrixelizerBufferDescription bufDescs[2] = {};
        u32 vertBufIdx                              = 0;
        u32 idxBufIdx                               = 0;
        bufDescs[0].buffer   = vertRes;
        bufDescs[0].outIndex = &vertBufIdx;
        bufDescs[1].buffer   = idxRes;
        bufDescs[1].outIndex = &idxBufIdx;
        FfxErrorCode regResult = ffxBrixelizerRegisterBuffers(&brixelizerContext, bufDescs, 2);
        if (regResult != FFX_OK) {
            utils::error("vulkanBrixelizerPass: ffxBrixelizerRegisterBuffers failed for scene '%s': %d",
                         scene->name.data,
                         regResult);
            return 0;
        }

        /* One instance per non-skinned draw. The instance transforms are
         * ROW-major 3x4 (plan pitfall #2 — the GLSL LoadInstanceTransform
         * loads 3 rows and applies them row-vector style): row r of the
         * cglm column-major mat4 m is (m[0][r], m[1][r], m[2][r], m[3][r]). */
        u32 drawCount      = (u32)vs->cpuDraws.size();
        u32 instanceCount  = 0;
        u32 skippedSkinned = 0;
        u32 sceneTris      = 0;
        std::vector<FfxBrixelizerInstanceDescription> descs(drawCount);
        std::vector<FfxBrixelizerInstanceID> ids(drawCount);
        for (u32 i = 0; i < drawCount; i++) {
            VulkanSceneDraw* sd = &vs->cpuDraws[i];
            GpuDrawInstance* draw = &sd->draw;
            if (draw->flags & DRAW_FLAG_SKINNED) {
                skippedSkinned++;
                continue;
            }
            FfxBrixelizerInstanceDescription* inst = &descs[instanceCount];
            inst->indexFormat                       = FFX_INDEX_TYPE_UINT32;
            inst->indexBuffer                        = idxBufIdx;
            inst->indexBufferOffset                  = draw->firstIndex * (u32)sizeof(u32);
            inst->triangleCount                      = draw->indexCount / 3;
            inst->vertexBuffer                       = vertBufIdx;
            inst->vertexStride                       = BRIX_SCENE_VERTEX_STRIDE;
            inst->vertexBufferOffset                 = (u32)draw->vertexOffset * BRIX_SCENE_VERTEX_STRIDE;
            inst->vertexCount                        = sd->vertexCount;
            inst->vertexFormat                       = FFX_SURFACE_FORMAT_R32G32B32_FLOAT;
            inst->flags                              = FFX_BRIXELIZER_INSTANCE_FLAG_NONE;
            inst->outInstanceID                      = &ids[instanceCount];

            versor wrot;
            vec3 wpos;
            float wscale;
            entityWorldTransform(scene, draw->entity, wrot, wpos, &wscale);

            /* Conservative world AABB from the local bounding sphere:
             * center = M * c, radius = |scale| * r (the sphere from
             * computeBoundingSphere is a centroid circumsphere, already
             * conservative). */
            float radius = fabsf(draw->boundingSphere[3] * wscale);
            vec3 scaledCenter = {draw->boundingSphere[0] * wscale,
                                 draw->boundingSphere[1] * wscale,
                                 draw->boundingSphere[2] * wscale};
            vec3 worldCenter;
            glm_quat_rotatev(wrot, scaledCenter, worldCenter);
            glm_vec3_add(wpos, worldCenter, worldCenter);
            for (u32 a = 0; a < 3; a++) {
                inst->aabb.min[a] = worldCenter[a] - radius;
                inst->aabb.max[a] = worldCenter[a] + radius;
            }
            /* Step 3 baseline: submit every instance to every cascade (the
             * size-based maxCascade heuristic — small objects out of far
             * cascades — lands in Step 9 with the bake-cost tuning; a too
             * aggressive filter here would silently drop world geometry from
             * the SDF, since the cascade regions are camera-centered). */
            inst->maxCascade = BRIX_NUM_CASCADES - 1;

            Transform t   = {};
            memcpy(t.rot, wrot, sizeof(wrot));
            t.pos[0]      = wpos[0];
            t.pos[1]      = wpos[1];
            t.pos[2]      = wpos[2];
            t.pos[3]      = wscale;
            mat4 m;
            transformToMat4(&t, m);
            for (u32 r = 0; r < 3; r++) {
                inst->transform[r * 4 + 0] = m[0][r];
                inst->transform[r * 4 + 1] = m[1][r];
                inst->transform[r * 4 + 2] = m[2][r];
                inst->transform[r * 4 + 3] = m[3][r];
            }
            sceneTris += inst->triangleCount;
            instanceCount++;
        }

        if (totalRegisteredInstances + instanceCount > FFX_BRIXELIZER_MAX_INSTANCES) {
            utils::error(
                "vulkanBrixelizerPass: scene '%s' would exceed the instance cap (%u + %u > %u); "
                "skipped",
                scene->name.data,
                totalRegisteredInstances,
                instanceCount,
                FFX_BRIXELIZER_MAX_INSTANCES);
            u32 dropIdx[2] = {vertBufIdx, idxBufIdx};
            ffxBrixelizerUnregisterBuffers(&brixelizerContext, dropIdx, 2);
            return 0;
        }

        /* [diag] world AABBs of the registered instances (ENGINE_BRIX_DIAG=1) —
         * check overlap with the cascade regions around the camera. */
        static char diagSet;
        static char diagOn;
        if (!diagSet) {
            diagSet = 1;
            const char* env = getenv("ENGINE_BRIX_DIAG");
            diagOn          = (env && !strcmp(env, "1")) ? 1 : 0;
        }
        if (diagOn) {
            for (u32 i = 0; i < instanceCount; i++) {
                if (i < 4 || (i % 8) == 0) {
                    utils::info(
                        "vulkanBrixelizerPass: [diag] inst %u (scene '%s'): aabb=(%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f) maxC=%u tris=%u",
                        i,
                        scene->name.data,
                        descs[i].aabb.min[0],
                        descs[i].aabb.min[1],
                        descs[i].aabb.min[2],
                        descs[i].aabb.max[0],
                        descs[i].aabb.max[1],
                        descs[i].aabb.max[2],
                        descs[i].maxCascade,
                        descs[i].triangleCount);
                }
            }
        }

        if (instanceCount > 0) {
            FfxErrorCode instResult =
                ffxBrixelizerCreateInstances(&brixelizerContext, descs.data(), instanceCount);
            if (instResult != FFX_OK) {
                utils::error("vulkanBrixelizerPass: ffxBrixelizerCreateInstances failed for scene '%s': %d",
                             scene->name.data,
                             instResult);
                u32 dropIdx[2] = {vertBufIdx, idxBufIdx};
                ffxBrixelizerUnregisterBuffers(&brixelizerContext, dropIdx, 2);
                return 0;
            }
        }

        BrixelSceneReg reg = {};
        reg.scene          = scene;
        reg.vs             = vs;
        reg.vertBufIdx     = vertBufIdx;
        reg.idxBufIdx      = idxBufIdx;
        reg.instanceIDs.assign(ids.begin(), ids.begin() + (ptrdiff_t)instanceCount);
        reg.instanceCount  = instanceCount;
        reg.triangleCount  = sceneTris;
        sceneRegs.push_back(reg);
        totalRegisteredInstances += instanceCount;
        totalRegisteredTriangles += sceneTris;
        /* The bake of the new (invalidated) instances lands across the next
         * full cascade round (256 updates) — track that window's GPU cost
         * for Gate 3. Only for non-empty registrations (skinned-only scenes
         * bake nothing). */
        if (instanceCount > 0) {
            regBakeFrame    = frameIndex;
            regBakeLogged   = 0;
            regBakeActive   = 1;
            regBakeGpuTotal = 0;
            regBakeGpuMax   = 0.0;
        }
        utils::info("vulkanBrixelizerPass: registered scene '%s': %u instances / %u tris "
                    "(%u skinned draws skipped, buf %u/%u)",
                    scene->name.data,
                    instanceCount,
                    sceneTris,
                    skippedSkinned,
                    vertBufIdx,
                    idxBufIdx);
        utils::info("vulkanBrixelizerPass: brixelizer totals: %u instances / %u tris (cap %u)",
                    totalRegisteredInstances,
                    totalRegisteredTriangles,
                    FFX_BRIXELIZER_MAX_INSTANCES);
        return 1;
    }

    /* rendererSceneCreate hook. Called on the scene-load worker thread
     * (sceneLoadOffThread) — the FFX context is only ever touched from the
     * main thread, so defer to a main-thread task. If the context is not up
     * yet, ensureContext re-registers all ecs.scenes on creation and picks
     * the scene up there. */
    static void brixelizerSceneCreateTask(void* pScene) {
        Scene* scene = static_cast<Scene*>(pScene);
        if (contextReady && scene && scene->backendScene) {
            registerScene(scene);
        }
    }

    /* Step 4: terrain SDF tiles. The heightmap surface is not a mesh — the
     * voxelizer gets one decimated position-only grid per streaming tile.
     * Resolution N (ENGINE_BRIXEL_TERRAIN_RES, default 65 → 32 m spacing on a
     * 2048 m tile): N×N vertices (positions only, 12 B), 2(N−1)² triangles,
     * u16 indices. Clamped to 255: N² must fit the u16 index range. */
    static void terrainResolveRes(void) {
        if (terrainResSet) {
            return;
        }
        terrainResSet = 1;
        const char* env = getenv("ENGINE_BRIXEL_TERRAIN_RES");
        terrainRes      = (env && *env) ? (u32)atoi(env) : 65;
        if (terrainRes < 2) {
            terrainRes = 2;
        }
        if (terrainRes > 255) {
            terrainRes = 255;
        }
    }

    static void terrainTileEvict(BrixelTerrainTile* e) {
        i32 x = e->tileX;
        i32 z = e->tileZ;
        u64 stamp = e->readyStamp;
        if (e->instanceCreated && contextReady) {
            FfxErrorCode delResult =
                ffxBrixelizerDeleteInstances(&brixelizerContext, &e->instanceID, 1);
            if (delResult != FFX_OK) {
                utils::error("vulkanBrixelizerPass: ffxBrixelizerDeleteInstances (terrain tile %d,%d) failed: %d",
                             x,
                             z,
                             delResult);
            }
            totalRegisteredInstances--;
            totalRegisteredTriangles -= 2u * (terrainRes - 1) * (terrainRes - 1);
        }
        /* Defer the GPU buffer teardown: in-flight FFX dispatches still hold
         * the wrapped buffer handles, and the bindless slot may be re-registered
         * for another tile before they drain. */
        BrixelTerrainDeferred d = {};
        d.vertBuf    = e->vertBuf;
        d.idxBuf     = e->idxBuf;
        d.vertBufIdx = e->vertBufIdx;
        d.idxBufIdx  = e->idxBufIdx;
        d.unreg      = contextReady;
        d.framesLeft = 3;
        terrainDeferred.push_back(d);
        *e = BrixelTerrainTile{};
        utils::info("vulkanBrixelizerPass: terrain tile (%d,%d) evicted from SDF (stamp %llu)",
                    x,
                    z,
                    (unsigned long long)stamp);
    }

    static char terrainTileCreate(const HeightmapTileView* v) {
        u32 n   = terrainRes;
        u32 tex = HEIGHTMAP_TEX;
        /* The tile's CPU grid is row-major heights[z * TEX + x] (metres); the
         * decimated sample (i, j) lifts the nearest grid texel at the same
         * normalized position, so tile borders stay watertight with the
         * rendered/physics lattice. */
        std::vector<float> verts((size_t)n * n * 3);
        std::vector<u16> idx((size_t)2 * (n - 1) * (n - 1) * 3);
        float step = v->sizeMeters / (float)(n - 1);
        float minH = 1e30f;
        float maxH = -1e30f;
        for (u32 j = 0; j < n; j++) {
            u32 sz = (u32)roundf((float)j * (float)(tex - 1) / (float)(n - 1));
            if (sz >= tex) {
                sz = tex - 1;
            }
            for (u32 i = 0; i < n; i++) {
                u32 sx = (u32)roundf((float)i * (float)(tex - 1) / (float)(n - 1));
                float h = v->heights[(size_t)sz * tex + sx];
                verts[(size_t)(j * n + i) * 3 + 0] = v->originX + (float)i * step;
                verts[(size_t)(j * n + i) * 3 + 1] = h;
                verts[(size_t)(j * n + i) * 3 + 2] = v->originZ + (float)j * step;
                if (h < minH) {
                    minH = h;
                }
                if (h > maxH) {
                    maxH = h;
                }
            }
        }
        u32 k = 0;
        for (u32 j = 0; j + 1 < n; j++) {
            for (u32 i = 0; i + 1 < n; i++) {
                u16 a = (u16)(j * n + i);
                u16 b = (u16)(a + 1);
                u16 c = (u16)(a + n);
                u16 d = (u16)(c + 1);
                idx[k++] = a;
                idx[k++] = b;
                idx[k++] = d;
                idx[k++] = a;
                idx[k++] = d;
                idx[k++] = c;
            }
        }

        VulkanBuffer vb =
            vulkanCreateGpuBuffer(utils::strtmp("BrixelTerrainVerts %d_%d", v->tileX, v->tileZ),
                                  (u64)verts.size() * sizeof(float),
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        VulkanBuffer ib =
            vulkanCreateGpuBuffer(utils::strtmp("BrixelTerrainIdx %d_%d", v->tileX, v->tileZ),
                                  (u64)idx.size() * sizeof(u16),
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        if (!vb.buf || !ib.buf) {
            utils::error("vulkanBrixelizerPass: terrain tile (%d,%d) buffer creation failed", v->tileX, v->tileZ);
            if (vb.buf) {
                vulkanDestroyBuffer(&vb, NULL);
            }
            if (ib.buf) {
                vulkanDestroyBuffer(&ib, NULL);
            }
            return 0;
        }
        VulkanCommand* tcmd = vulkanTransientBegin();
        vulkanCopy(.cmd         = tcmd,
                   .source.data = verts.data(),
                   .target.buf  = &vb,
                   .size        = (u32)(verts.size() * sizeof(float)));
        vulkanCopy(.cmd         = tcmd,
                   .source.data = idx.data(),
                   .target.buf  = &ib,
                   .size        = (u32)(idx.size() * sizeof(u16)));
        /* Wait: the FFX voxelizer reads these as SSBs, the data must be
         * complete before the bake dispatch. */
        vulkanTransientEnd(tcmd, 1);

        /* PIXEL_COMPUTE_READ like the scene registration — the FFX backend
         * binds every SRV buffer as a shader storage buffer. */
        FfxResource vRes =
            vulkanFfxWrapBufferResource(&vb,
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ,
                                        L"BrixelTerrainVerts");
        FfxResource iRes =
            vulkanFfxWrapBufferResource(&ib,
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ,
                                        L"BrixelTerrainIdx");
        FfxBrixelizerBufferDescription bufDescs[2] = {};
        u32 vertBufIdx                              = 0;
        u32 idxBufIdx                               = 0;
        bufDescs[0].buffer   = vRes;
        bufDescs[0].outIndex = &vertBufIdx;
        bufDescs[1].buffer   = iRes;
        bufDescs[1].outIndex = &idxBufIdx;
        FfxErrorCode regResult = ffxBrixelizerRegisterBuffers(&brixelizerContext, bufDescs, 2);
        if (regResult != FFX_OK) {
            utils::error("vulkanBrixelizerPass: terrain tile (%d,%d) ffxBrixelizerRegisterBuffers failed: %d",
                         v->tileX,
                         v->tileZ,
                         regResult);
            vulkanDestroyBuffer(&vb, NULL);
            vulkanDestroyBuffer(&ib, NULL);
            return 0;
        }

        if (totalRegisteredInstances + 1 > FFX_BRIXELIZER_MAX_INSTANCES) {
            utils::error("vulkanBrixelizerPass: terrain tile (%d,%d) would exceed the instance cap (%u >= %u); skipped",
                         v->tileX,
                         v->tileZ,
                         totalRegisteredInstances,
                         FFX_BRIXELIZER_MAX_INSTANCES);
            u32 dropIdx[2] = {vertBufIdx, idxBufIdx};
            ffxBrixelizerUnregisterBuffers(&brixelizerContext, dropIdx, 2);
            vulkanDestroyBuffer(&vb, NULL);
            vulkanDestroyBuffer(&ib, NULL);
            return 0;
        }

        FfxBrixelizerInstanceDescription desc = {};
        desc.indexFormat   = FFX_INDEX_TYPE_UINT16;
        desc.indexBuffer   = idxBufIdx;
        desc.triangleCount = 2u * (n - 1) * (n - 1);
        desc.vertexBuffer  = vertBufIdx;
        desc.vertexStride  = 12;
        desc.vertexCount   = n * n;
        desc.vertexFormat  = FFX_SURFACE_FORMAT_R32G32B32_FLOAT;
        desc.flags         = FFX_BRIXELIZER_INSTANCE_FLAG_NONE;
        desc.aabb.min[0]   = v->originX;
        desc.aabb.min[1]   = minH;
        desc.aabb.min[2]   = v->originZ;
        desc.aabb.max[0]   = v->originX + v->sizeMeters;
        desc.aabb.max[1]   = maxH;
        desc.aabb.max[2]   = v->originZ + v->sizeMeters;
        /* Identity, ROW-major (plan pitfall #2 — the GLSL loads 3 rows; the
         * diagonal must sit at [0]/[5]/[10], the Step-2 bug was writing it
         * column-major-style at [0]/[4]/[8]). The positions are already
         * world-space. */
        desc.transform[0]  = 1.0f;
        desc.transform[5]  = 1.0f;
        desc.transform[10] = 1.0f;
        /* A 2048 m tile spans every cascade region that reaches it — the far
         * cascade's 16.4 km block covers the streaming window, so submit all
         * cascades (the voxelizer only stamps the bricks the AABB overlaps). */
        desc.maxCascade = BRIX_NUM_CASCADES - 1;
        FfxBrixelizerInstanceID instanceID;
        desc.outInstanceID = &instanceID;
        FfxErrorCode instResult = ffxBrixelizerCreateInstances(&brixelizerContext, &desc, 1);
        if (instResult != FFX_OK) {
            utils::error("vulkanBrixelizerPass: terrain tile (%d,%d) ffxBrixelizerCreateInstances failed: %d",
                         v->tileX,
                         v->tileZ,
                         instResult);
            u32 dropIdx[2] = {vertBufIdx, idxBufIdx};
            ffxBrixelizerUnregisterBuffers(&brixelizerContext, dropIdx, 2);
            vulkanDestroyBuffer(&vb, NULL);
            vulkanDestroyBuffer(&ib, NULL);
            return 0;
        }

        BrixelTerrainTile* e = NULL;
        for (u32 i = 0; i < terrainTiles.size(); i++) {
            if (!terrainTiles[i].inUse) {
                e = &terrainTiles[i];
                break;
            }
        }
        if (!e) {
            e = &terrainTiles.emplace_back(BrixelTerrainTile{});
        }
        e->tileX           = v->tileX;
        e->tileZ           = v->tileZ;
        e->readyStamp      = v->readyStamp;
        e->inUse           = 1;
        e->vertBuf         = vb;
        e->idxBuf          = ib;
        e->vertBufIdx      = vertBufIdx;
        e->idxBufIdx       = idxBufIdx;
        e->instanceID      = instanceID;
        e->instanceCreated = 1;
        totalRegisteredInstances++;
        totalRegisteredTriangles += desc.triangleCount;
        utils::info("vulkanBrixelizerPass: terrain tile (%d,%d) registered in SDF: stamp=%llu res=%u tris=%u h=[%.0f,%.0f]m "
                    "(totals: %u instances / %u tris, cap %u)",
                    v->tileX,
                    v->tileZ,
                    (unsigned long long)v->readyStamp,
                    n,
                    desc.triangleCount,
                    minH,
                    maxH,
                    totalRegisteredInstances,
                    totalRegisteredTriangles,
                    FFX_BRIXELIZER_MAX_INSTANCES);
        return 1;
    }

    static void terrainSyncTiles(void) {
        terrainResolveRes();
        HeightmapTerrain* ht = heightmapTerrainGetActive();
        if (!ht || !ht->initialized) {
            return;
        }
        if (ht != terrainHt) {
            /* World switch: the old tiles are gone; drop every registration
             * (instances die with them, buffers deferred as usual). */
            for (u32 i = 0; i < terrainTiles.size(); i++) {
                if (terrainTiles[i].inUse) {
                    terrainTileEvict(&terrainTiles[i]);
                }
            }
            terrainHt = ht;
            utils::info("vulkanBrixelizerPass: terrain world switched, SDF tile registrations reset");
        }
        u32 cap = ht->windowSize * ht->windowSize;
        if (terrainTiles.size() < cap) {
            terrainTiles.resize(cap);
        }

        std::vector<HeightmapTileView> views(cap);
        u32 viewCount = heightmapTerrainSnapshotTiles(ht, views.data(), cap);

        /* Evict tiles that left the window or were regenerated (stamp bump). */
        for (u32 i = 0; i < terrainTiles.size(); i++) {
            BrixelTerrainTile* e = &terrainTiles[i];
            if (!e->inUse) {
                continue;
            }
            char match = 0;
            for (u32 j = 0; j < viewCount; j++) {
                if (views[j].tileX == e->tileX && views[j].tileZ == e->tileZ &&
                    views[j].readyStamp == e->readyStamp) {
                    match = 1;
                    break;
                }
            }
            if (!match) {
                terrainTileEvict(e);
            }
        }

        /* Register missing tiles, a few per frame (the GPU upload is a
         * fence-waiting transient command; spreading it matches the
         * heightmap pass's upload budget and keeps the per-frame hitch small).
         * The bake of the new instances lands across the next cascade round —
         * the pass GPU cost on a registration frame is the per-tile bake proxy
         * (Gate 4's "per-tile bake cost"). */
        u32 budget = 3;
        for (u32 j = 0; j < viewCount && budget > 0; j++) {
            char have = 0;
            for (u32 i = 0; i < terrainTiles.size(); i++) {
                if (terrainTiles[i].inUse && terrainTiles[i].tileX == views[j].tileX &&
                    terrainTiles[i].tileZ == views[j].tileZ &&
                    terrainTiles[i].readyStamp == views[j].readyStamp) {
                    have = 1;
                    break;
                }
            }
            if (have) {
                continue;
            }
            if (terrainTileCreate(&views[j])) {
                budget--;
                if (!terrainStatsLogged) {
                    terrainStatsLogged = 1;
                    utils::info("vulkanBrixelizerPass: terrain SDF registration frame: pass gpu=%.3f ms (includes the first tile bakes)",
                                profile.elapsed / MILLION);
                }
            }
        }
    }

    static char ensureContext(void) {
        if (!backendReady) {
            scratchBufferSize = ffxGetScratchMemorySizeVK(vulkan.physicalDevice, 2);
            scratchBuffer     = calloc(1, scratchBufferSize);
            if (!scratchBuffer) {
                utils::error(
                    "vulkanBrixelizerPass: failed to allocate %zu bytes of FFX backend scratch "
                    "memory",
                    scratchBufferSize);
                return 0;
            }

            VkDeviceContext deviceContext = {
                .vkDevice         = vulkan.device,
                .vkPhysicalDevice = vulkan.physicalDevice,
                .vkDeviceProcAddr = vkGetDeviceProcAddr,
            };

            FfxDevice device = ffxGetDeviceVK(&deviceContext);
            FfxErrorCode backendResult =
                ffxGetInterfaceVK(&backendInterface, device, scratchBuffer, scratchBufferSize, 2);
            if (backendResult != FFX_OK) {
                utils::error("vulkanBrixelizerPass: ffxGetInterfaceVK failed: %d", backendResult);
                free(scratchBuffer);
                scratchBuffer     = NULL;
                scratchBufferSize = 0;
                return 0;
            }

            backendReady = 1;
        }

        if (contextReady) {
            return 1;
        }

        if (!sdfAtlas.img && !createResources()) {
            return 0;
        }

        FfxBrixelizerContextDescription desc = {};
        desc.numCascades                     = BRIX_NUM_CASCADES;
        /* ALL_DEBUG carries the context/cascade readback flags, which are
         * required for outStats: the readback buffers are only allocated when
         * set, and the stats are filled from that (lagged) GPU readback. */
        desc.flags = FFX_BRIXELIZER_CONTEXT_FLAG_ALL_DEBUG;
        for (u32 i = 0; i < BRIX_NUM_CASCADES; i++) {
            desc.cascadeDescs[i].flags     = FFX_BRIXELIZER_CASCADE_STATIC;
            desc.cascadeDescs[i].voxelSize = BRIX_BASE_VOXEL_SIZE * (float)(1u << i);
        }
        desc.backendInterface = backendInterface;

        FfxErrorCode createResult = ffxBrixelizerContextCreate(&desc, &brixelizerContext);
        if (createResult != FFX_OK) {
            if ((u32)createResult == FFX_ERROR_OUT_OF_MEMORY ||
                (u32)createResult == FFX_ERROR_INSUFFICIENT_MEMORY) {
                utils::terminate(
                    "vulkanBrixelizerPass: not enough GPU memory to create the brixelizer context. "
                    "Free VRAM by closing other GPU applications and try again.");
            }
            utils::error("vulkanBrixelizerPass: ffxBrixelizerContextCreate failed: %d",
                         createResult);
            destroyResources();
            return 0;
        }

        contextReady = 1;
        utils::info(
            "vulkanBrixelizerPass: created voxelizer context (%u cascades, voxel %.0f-%.0f m, "
            "gpu scratch %llu MiB)",
            BRIX_NUM_CASCADES,
            BRIX_BASE_VOXEL_SIZE,
            BRIX_BASE_VOXEL_SIZE * (float)(1u << (BRIX_NUM_CASCADES - 1)),
            BRIX_GPU_SCRATCH_SIZE >> 20);

        /* (Re)register every loaded scene — the FFX buffer/instance tables
         * were recreated with the context. All scenes (not just the
         * frustum-visible ones): the SDF is a world-space representation and
         * must not pop with camera orientation. */
        for (u32 i = 0; i < ecs.scenes.size(); i++) {
            registerScene(ecs.scenes[i]);
        }
        return 1;
    }

    static FfxBrixelizerTraceDebugModes getSdfDebugMode(void) {
        if (!sdfDebugModeSet) {
            sdfDebugModeSet = 1;
            const char* env = getenv("ENGINE_BRIX_SDF_DEBUG");
            if (env && !strcmp(env, "grad")) {
                sdfDebugMode = FFX_BRIXELIZER_TRACE_DEBUG_MODE_GRAD;
            } else if (env && !strcmp(env, "brick")) {
                sdfDebugMode = FFX_BRIXELIZER_TRACE_DEBUG_MODE_BRICK_ID;
            } else if (env && !strcmp(env, "cascade")) {
                sdfDebugMode = FFX_BRIXELIZER_TRACE_DEBUG_MODE_CASCADE_ID;
            } else if (env && !strcmp(env, "uvw")) {
                sdfDebugMode = FFX_BRIXELIZER_TRACE_DEBUG_MODE_UVW;
            } else if (env && !strcmp(env, "iter")) {
                sdfDebugMode = FFX_BRIXELIZER_TRACE_DEBUG_MODE_ITERATIONS;
            } else if (env && !strcmp(env, "off")) {
                sdfDebugEnabled = 0;
            } else {
                sdfDebugMode = FFX_BRIXELIZER_TRACE_DEBUG_MODE_DISTANCE;
            }
        }
        if (!sdfDebugTMaxSet) {
            sdfDebugTMaxSet = 1;
            const char* env = getenv("ENGINE_BRIX_SDF_TMAX");
            sdfDebugTMax    = (env && *env) ? (float)atof(env) : 10000.0f;
        }
        return sdfDebugMode;
    }

    void VulkanBrixelizerPass::update() {
        elapsedCPU = utils::nanos();
        if (vulkan.skipFrame) {
            elapsedCPU = utils::nanos() - elapsedCPU;
            return;
        }

        VulkanCommand* cmd = vulkan.currentCmd;
        Entity* camEntity  = cameraGetEntity();
        if (!cmd || !camEntity) {
            elapsedCPU = utils::nanos() - elapsedCPU;
            return;
        }
        Camera* camera = getComponent(camEntity->scene, Camera, camEntity->id);
        if (!camera || !ensureContext()) {
            elapsedCPU = utils::nanos() - elapsedCPU;
            return;
        }

        /* Step 4: sync the streaming heightmap tiles with the SDF (register
         * newly READY tiles, evict out-of-window ones) before this frame's
         * bake dispatch so new tiles are baked with the rest. */
        terrainSyncTiles();

        /* Step 2.2: read the debug-visualization mode once (ENGINE_BRIX_SDF_DEBUG;
         * "off" disables the extra dispatch). */
        getSdfDebugMode();

        /* The SDF atlas (and the debug image, when active) are the images this
         * update touches; the FFX dispatch does not manage engine layouts
         * (plan pitfall #11) — stage them for UAV writes. */
        vulkanTransition(cmd, &sdfAtlas, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        if (sdfDebugEnabled && sdfDebug.img) {
            vulkanTransition(cmd, &sdfDebug, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        }

        FfxBrixelizerUpdateDescription updateDesc = {};
        updateDesc.resources.sdfAtlas =
            vulkanFfxWrapImageResource(&sdfAtlas,
                                       FFX_RESOURCE_USAGE_UAV,
                                       FFX_RESOURCE_STATE_UNORDERED_ACCESS,
                                       L"BrixelSdfAtlas");
        updateDesc.resources.brickAABBs =
            vulkanFfxWrapBufferResource(&brickAABBs,
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        FFX_RESOURCE_STATE_UNORDERED_ACCESS,
                                        L"BrixelBrickAABBs");
        wchar_t cascadeName[64];
        for (u32 i = 0; i < FFX_BRIXELIZER_MAX_CASCADES; i++) {
            swprintf(cascadeName, 64, L"BrixelCascade%uAabbTree", i);
            updateDesc.resources.cascadeResources[i].aabbTree =
                vulkanFfxWrapBufferResource(&cascadeAABBTrees[i],
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                            FFX_RESOURCE_STATE_UNORDERED_ACCESS,
                                            cascadeName);
            swprintf(cascadeName, 64, L"BrixelCascade%uBrickMap", i);
            updateDesc.resources.cascadeResources[i].brickMap =
                vulkanFfxWrapBufferResource(&cascadeBrickMaps[i],
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                            FFX_RESOURCE_STATE_UNORDERED_ACCESS,
                                            cascadeName);
        }

        updateDesc.frameIndex = frameIndex++;
        /* Cascades follow the camera (the sample's m_SdfCenterFollowCamera
         * behavior). */
        updateDesc.sdfCenter[0]            = camera->cameraUbo.renderLocation[0];
        updateDesc.sdfCenter[1]            = camera->cameraUbo.renderLocation[1];
        updateDesc.sdfCenter[2]            = camera->cameraUbo.renderLocation[2];
        updateDesc.populateDebugAABBsFlags = FFX_BRIXELIZER_POPULATE_AABBS_NONE;

        /* Step 2.2: SDF debug visualization — ray-march the baked SDF into a
         * render-res R16F image (mode via ENGINE_BRIX_SDF_DEBUG, default
         * distance; off disables the extra dispatch). The inverse matrices
         * are the engine's cglm column-major mat4s memcpy'd verbatim (plan
         * pitfall #1 — the GLSL unprojection expects them column-major). The
         * static-only cascade layout puts the detail cascade at index 0. */
        FfxBrixelizerDebugVisualizationDescription debugVisDesc = {};
        if (sdfDebugEnabled && sdfDebug.img) {
            memcpy(debugVisDesc.inverseViewMatrix,
                   camera->cameraUbo.invView,
                   sizeof(debugVisDesc.inverseViewMatrix));
            memcpy(debugVisDesc.inverseProjectionMatrix,
                   camera->cameraUbo.invProjection,
                   sizeof(debugVisDesc.inverseProjectionMatrix));
            debugVisDesc.debugState        = getSdfDebugMode();
            debugVisDesc.startCascadeIndex = 0;
            debugVisDesc.endCascadeIndex   = BRIX_NUM_CASCADES - 1;
            debugVisDesc.sdfSolveEps       = 0.5f;
            debugVisDesc.tMin              = 0.0f;
            debugVisDesc.tMax              = sdfDebugTMax;
            debugVisDesc.renderWidth       = (u32)sdfDebug.extent.width;
            debugVisDesc.renderHeight      = (u32)sdfDebug.extent.height;
            debugVisDesc.output            = vulkanFfxWrapImageResource(&sdfDebug,
                                                                        FFX_RESOURCE_USAGE_UAV,
                                                                        FFX_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                        L"BrixelSdfDebug");
            updateDesc.debugVisualizationDesc = &debugVisDesc;
        }

        updateDesc.maxReferences           = BRIX_MAX_REFERENCES;
        updateDesc.triangleSwapSize        = BRIX_TRIANGLE_SWAP_SIZE;
        updateDesc.maxBricksPerBake        = BRIX_MAX_BRICKS_PER_BAKE;
        updateDesc.outStats                = &stats;

        size_t scratchNeeded            = 0;
        updateDesc.outScratchBufferSize = &scratchNeeded;

        vulkanBeginProfile(cmd, &profile, 1);
        FfxErrorCode bakeResult =
            ffxBrixelizerBakeUpdate(&brixelizerContext, &updateDesc, &bakedUpdateDesc);
        if (bakeResult == FFX_OK) {
            if (scratchNeeded > BRIX_GPU_SCRATCH_SIZE) {
                utils::error("vulkanBrixelizerPass: brixelizer scratch overflow: %zu > %llu",
                             scratchNeeded,
                             BRIX_GPU_SCRATCH_SIZE);
            }
            FfxResource scratch = vulkanFfxWrapBufferResource(
                &gpuScratch,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                FFX_RESOURCE_STATE_UNORDERED_ACCESS,
                L"BrixelGpuScratch");
            FfxErrorCode result = ffxBrixelizerUpdate(&brixelizerContext,
                                                      &bakedUpdateDesc,
                                                      scratch,
                                                      ffxGetCommandListVK(cmd->cmd));
            if (result != FFX_OK) {
                utils::error("vulkanBrixelizerPass: ffxBrixelizerUpdate failed: %d", result);
            }
        } else {
            utils::error("vulkanBrixelizerPass: ffxBrixelizerBakeUpdate failed: %d", bakeResult);
        }
        vulkanEndProfile(cmd, &profile, 1);

        /* The GI ray-march (Step 7) samples the atlas — leave it readable.
         * The debug image is a dump target — leave it readable too. */
        vulkanTransition(cmd, &sdfAtlas, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        if (sdfDebugEnabled && sdfDebug.img) {
            vulkanTransition(cmd, &sdfDebug, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        }

        /* outStats is a lagged GPU readback (filled a few updates later).
         * Track the free-brick pool across updates (lagged readback). */
        static u32 lastFreeBricks = 0;
        if (stats.contextStats.freeBricks && stats.contextStats.freeBricks != lastFreeBricks) {
            lastFreeBricks = stats.contextStats.freeBricks;
            utils::info(
                "vulkanBrixelizerPass: free-brick pool (lagged stats, cascade %u): free=%u allocAttempted=%u allocSucceeded=%u cleared=%u",
                stats.cascadeIndex,
                stats.contextStats.freeBricks,
                stats.contextStats.brickAllocationsAttempted,
                stats.contextStats.brickAllocationsSucceeded,
                stats.contextStats.bricksCleared);
        }
        /* Gate 3: the scene meshes actually baked (triangles/bricks allocated
         * in a static cascade) — fires once, lagged. */
        if ((stats.staticCascadeStats.trianglesAllocated || stats.staticCascadeStats.bricksAllocated) &&
            !statsTrisLogged) {
            statsTrisLogged = 1;
            utils::info(
                "vulkanBrixelizerPass: scene instances baked (lagged): cascade=%u staticTris=%u "
                "staticRefs=%u staticBricks=%u freeBricks=%u",
                stats.cascadeIndex,
                stats.staticCascadeStats.trianglesAllocated,
                stats.staticCascadeStats.referencesAllocated,
                stats.staticCascadeStats.bricksAllocated,
                stats.contextStats.freeBricks);
        }
        /* Gate 3: first-bake cost — heaviest update in the 256-update window
         * after a (re)registration (covers the first full cascade round). */
        if (regBakeActive && regBakeLogged < BRIX_FIRST_BAKE_WINDOW && frameIndex > regBakeFrame &&
            frameIndex <= regBakeFrame + BRIX_FIRST_BAKE_WINDOW) {
            double ms = profile.elapsed / MILLION;
            regBakeGpuTotal += profile.elapsed;
            if (ms > regBakeGpuMax) {
                regBakeGpuMax = ms;
                utils::info("vulkanBrixelizerPass: first-bake: heaviest update so far gpu=%.3f ms (update %u/%u)",
                            ms,
                            regBakeLogged + 1,
                            BRIX_FIRST_BAKE_WINDOW);
            }
            regBakeLogged++;
            if (regBakeLogged == BRIX_FIRST_BAKE_WINDOW) {
                utils::info(
                    "vulkanBrixelizerPass: first-bake cost (%u updates, full cascade round): total=%.1f ms heaviest=%.3f ms",
                    BRIX_FIRST_BAKE_WINDOW,
                    (double)regBakeGpuTotal / MILLION,
                    regBakeGpuMax);
                regBakeActive = 0;
            }
        }
        if (frameIndex % 120 == 0) {
            utils::info(
                "vulkanBrixelizerPass: stats cascade=%u freeBricks=%u bricksCleared=%u "
                "staticTris=%u staticRefs=%u staticBricks=%u gpu=%.3f ms cam=(%.1f, %.1f, %.1f)",
                stats.cascadeIndex,
                stats.contextStats.freeBricks,
                stats.contextStats.bricksCleared,
                stats.staticCascadeStats.trianglesAllocated,
                stats.staticCascadeStats.referencesAllocated,
                stats.staticCascadeStats.bricksAllocated,
                profile.elapsed / MILLION, /* ns → ms */
                camera->cameraUbo.renderLocation[0],
                camera->cameraUbo.renderLocation[1],
                camera->cameraUbo.renderLocation[2]);
        }

        elapsedGPU = profile.elapsed;
        elapsedCPU = utils::nanos() - elapsedCPU;
    }

    void VulkanBrixelizerPass::postUpdate() {
        vulkanBrixelizerPass.cpuElapsed = elapsedCPU;
        vulkanBrixelizerPass.gpuElapsed = elapsedGPU;
    }

    void VulkanBrixelizerPass::removed() {
        /*
            Crashes on cleanup, safe to ignore for now, we will investigate later.
            Produces validation error;
            vkDestroyDevice(): VkDevice 0x58e2083d2630 has 4226 leaked objects that have not been destroyed.
            Safe to ignore.
        */

        // destroyContext();
        // destroyResources();
        // if (scratchBuffer) {
        //     free(scratchBuffer);
        //     scratchBuffer     = NULL;
        //     scratchBufferSize = 0;
        // }
        // backendInterface = FfxInterface{};
        // backendReady     = 0;
        // if (profileReady) {
        //     vulkanDestroyProfile(&profile);
        //     profile      = VulkanProfile{};
        //     profileReady = 0;
        // }
    }

    char vulkanBrixelizerPassGetInterface(FfxInterface* out) {
        if (out) {
            *out = backendInterface;
        }
        return backendReady;
    }

    char vulkanBrixelizerPassIsReady(void) {
        return contextReady;
    }

    struct VulkanImage* vulkanBrixelizerPassGetSdfDebug(void) {
        return sdfDebug.img ? &sdfDebug : NULL;
    }

    void vulkanBrixelizerPassSceneCreate(Scene* scene) {
        if (!scene) {
            return;
        }
        /* Worker-thread safe (see the hook's comment in the header). */
        utils::futureTaskAdd(0, brixelizerSceneCreateTask, scene);
    }

    void vulkanBrixelizerPassSceneDestroy(Scene* scene) {
        /* Main thread (rendererSceneDestroy calls it before
         * vulkanSceneDestroy frees the backend scene). */
        if (!scene) {
            return;
        }
        for (u32 i = 0; i < sceneRegs.size(); i++) {
            if (sceneRegs[i].scene != scene) {
                continue;
            }
            BrixelSceneReg* reg = &sceneRegs[i];
            if (contextReady) {
                if (reg->instanceCount > 0) {
                    FfxErrorCode delResult =
                        ffxBrixelizerDeleteInstances(&brixelizerContext,
                                                     reg->instanceIDs.data(),
                                                     reg->instanceCount);
                    if (delResult != FFX_OK) {
                        utils::error("vulkanBrixelizerPass: ffxBrixelizerDeleteInstances failed: %d",
                                     delResult);
                    }
                }
                u32 dropIdx[2] = {reg->vertBufIdx, reg->idxBufIdx};
                FfxErrorCode unregResult = ffxBrixelizerUnregisterBuffers(&brixelizerContext, dropIdx, 2);
                if (unregResult != FFX_OK) {
                    utils::error("vulkanBrixelizerPass: ffxBrixelizerUnregisterBuffers failed: %d",
                                 unregResult);
                }
            }
            totalRegisteredInstances -= reg->instanceCount;
            totalRegisteredTriangles -= reg->triangleCount;
            utils::info("vulkanBrixelizerPass: unregistered scene '%s' (%u instances, %u tris)",
                        scene->name.data,
                        reg->instanceCount,
                        reg->triangleCount);
            sceneRegs.erase(sceneRegs.begin() + (ptrdiff_t)i);
            return;
        }
    }
}  // namespace engine