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
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanBuffer.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/scene/VulkanScene.h"
#include "renderer/vulkan/pass/azgaar_props/VulkanAzgaarPropsPass.h"
#include "renderer/vulkan/utils/VulkanFfxUtils.h"
#include "timer/Timer.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function" /* ffx_core_cpu.h static inlines */
#include <FidelityFX/host/ffx_brixelizer.h>
#include <FidelityFX/host/ffx_brixelizer_raw.h>
#include <FidelityFX/host/ffx_brixelizergi.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#pragma GCC diagnostic pop
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <wchar.h>
#include <algorithm>
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
    /* Step 7: GI context + outputs (recreated on resolution change). */
    static void giDestroyContext(void);
    static void giCreateOutputs(u32 renderW, u32 renderH);
    static char giEnsureContext(u32 renderW, u32 renderH);
    static void giDispatch(VulkanCommand* cmd, Camera* camera);
    static void giDebugDispatch(VulkanCommand* cmd, Camera* camera);
    static FfxBrixelizerGIDebugMode getGIDebugMode(void);
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
    /* Deferred GPU-buffer teardown (3 frames past the GPU queue depth, like
     * the heightmap pass's deferred descriptors): in-flight FFX dispatches
     * still hold the wrapped handles, and the bindless slot may be
     * re-registered for another owner before they drain. Shared by the
     * terrain tiles (Step 4) and the props variant buffers (Step 5). */
    struct BrixelDeferred {
        VulkanBuffer vertBuf;
        VulkanBuffer idxBuf;
        u32 vertBufIdx;
        u32 idxBufIdx;
        char unreg;
        u32 framesLeft;
    };
    static std::vector<BrixelTerrainTile> terrainTiles;
    static std::vector<BrixelDeferred> deferred;
    static HeightmapTerrain* terrainHt;
    static u32 terrainRes;
    static char terrainResSet;
    static char terrainStatsLogged;
    static void terrainSyncTiles(void);
    static void terrainTileEvict(BrixelTerrainTile* e);
    static char terrainTileCreate(const HeightmapTileView* v);
    static void terrainClearAll(void);

    /* Step 5: props SDF. Per-(species, variant) position-only sub-buffers
     * (extracted from the merged 72 B species mesh — pitfall #8) + the
     * accepted per-set instance lists (tiles / settlements / landmarks).
     * `sdfPriority` / `sdfMaxCascade` come from the variant table the game
     * builds (AzgaarProps::buildAllMeshes). */
    struct BrixelPropsVariant {
        u32 species, variant;
        u32 vertBufIdx;
        u32 idxBufIdx;
        u32 vertCount;
        u32 triCount;
        u32 maxCascade;
        u32 priority;
        u32 indexType;
        char registered;
        float boundsMin[3];
        float boundsMax[3];
        VulkanBuffer vertBuf;
        VulkanBuffer idxBuf;
    };
    struct BrixelPropsVariantCpu {
        u32 species, variant;
        u32 vertCount;
        u32 triCount;
        u32 maxCascade;
        u32 priority;
        u32 indexType;
        float boundsMin[3];
        float boundsMax[3];
        std::vector<float> verts;
        std::vector<u16> idx16;
        std::vector<u32> idx32;
    };
    struct BrixelPropsSet {
        u32 kind; /* 0 = tile, 1 = global (settlements), 2 = landmarks */
        i32 tileX, tileZ;
        u64 readyStamp;
        char inUse;
        std::vector<PropInstance> instances; /* the accepted (budgeted) set */
        std::vector<FfxBrixelizerInstanceID> ids;
        u32 accepted;
        u32 dropped;
        u32 tris; /* sum of the accepted instances' variant triangle counts */
    };
    enum BrixelPropsOp {
        BRIX_PROPS_MESH_SET = 0,
        BRIX_PROPS_MESH_CLEAR,
        BRIX_PROPS_TILE_SET,
        BRIX_PROPS_TILE_CLEAR,
        BRIX_PROPS_GLOBAL_SET,
        BRIX_PROPS_GLOBAL_CLEAR,
        BRIX_PROPS_LANDMARKS_SET,
        BRIX_PROPS_LANDMARKS_CLEAR,
    };
    struct BrixelPropsPending {
        u32 kind;
        i32 tileX, tileZ;
        u64 readyStamp;
        std::vector<PropInstance> instances;
    };
    static std::vector<BrixelPropsVariant> propsVariants;
    static std::vector<PropVariantRange> propsVariantTable;
    static std::vector<BrixelPropsVariantCpu> propsVariantCpu;
    static std::vector<BrixelPropsSet> propsSets;
    static std::vector<BrixelPropsPending> propsPending;
    static utils::Thread propsLock = {.mutex = PTHREAD_MUTEX_INITIALIZER};
    static char propsContextRegistered;
    static char propsStatsLogged;
    static u32 propsBudget;
    static char propsBudgetSet;
    /* Reserve of the shared 65536 instance table for Step 10's dynamic
     * instances (the voxelizer asserts static+pending <= 65536). */
    static const u32 BRIX_DYNAMIC_HEADROOM = 1024;
    static void propsResolveBudget(void);
    static BrixelPropsVariant* propsVariantFind(u32 species, u32 variant);
    static void propsVariantRegister(u32 i);
    static void propsEnsureRegistered(void);
    static BrixelPropsSet* propsSetFind(u32 kind, i32 tileX, i32 tileZ);
    static void propsSetDelete(BrixelPropsSet* s);
    static void propsSetApply(u32 kind, i32 tileX, i32 tileZ, u64 readyStamp,
                              const PropInstance* insts, u32 count, const float* camPos);
    static void propsSyncPending(const float* camPos);
    static char propsExtractMeshes(const void* verts, u32 vertCount, const void* idx, u32 idxCount,
                                   const PropVariantRange* variants, u32 variantCount);

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

    /* ── Step 6: GI inputs (plans/brixelizer-gi.md) ──────────────
     * - blueNoise: 128² RG8 CPU-generated blue-noise rank mask (Bridson
     *   best-candidate). The GI shader masks pixel coords with & 127 and
     *   reads .xy — 128² is the native tile size.
     * - envCube: 128²×6 RGBA16F cube (CUBE_COMPATIBLE) — one-shot compute
     *   bake of the shared procedural sky (includes/sky.shader, the same
     *   function skybox.frag renders). Re-baked on swapchain creation.
     * - history*: render-res temporal inputs for the GI context (Step 7):
     *   HistoryDepth R32F (D32 copy; clear 0.0 = background, reverse-Z),
     *   HistoryNormal RGBA16F (copy of worldNormal), HistoryLitOutput
     *   RGBA16F (copy of the composited color — after GI compositing in
     *   Step 8). Copied at end of frame (postUpdate runs after every
     *   pass's update, so compositeColor is current-frame). */
    static VulkanImage blueNoise;
    static VulkanImage envCube;
    static VulkanImage envFaceDump;
    static VulkanImage historyDepth;
    static VulkanImage historyNormal;
    static VulkanImage historyLit;
    static VulkanPipe  envCubePipe;
    static char        envCubePipeReady;
    /* Dirty by default: the swapchainCreated signal can fire before the pass
     * subscribes (it is registered in added(), the signal is emitted during
     * renderer init) — the first update after context creation bakes the
     * cube. Later swapchain recreations re-flag it via the handler. */
    static char        envCubeDirty = 1;
    static char        giInputsLogged;
    static void renderEnvCube(void);
    static void giHistoryCopy(void);
    static void blueNoiseGenerate(u8* out, u32 width);

    /* ── Step 7: GI context + outputs (plans/brixelizer-gi.md) ──────
     * - giContext: ffxBrixelizerGIContext (DEPTH_INVERTED, 50% internal,
     *   displaySize = render res). Holds a raw pointer to the voxelizer
     *   context, so it is destroyed before it (destroyContext ordering).
     *   displaySize is fixed at creation → recreated on resolution change
     *   (giEnsureContext, FSR-pass pattern).
     * - giDiffuse / giSpecular: outputDiffuseGI / outputSpecularGI (R16F RGBA
     *   render-res) — written by the GI context's final upsample pass as UAVs.
     * - giDebug: BrixelGIDebug (R16F render-res) — radiance / irradiance cache
     *   debug visualization (ENGINE_BRIXGI_DEBUG=radiance|irradiance). Runs as
     *   a ONE-SHOT on a single frame (ENGINE_BRIXGI_DEBUG_FRAME, default 120)
     *   rather than every frame — a per-frame second FFX dispatch would halve
     *   the backend's dynamic-view lifetime. The image persists afterwards so
     *   a later dump reads that frame's cache.
     * - giPrevView / giPrevProjection: the engine only keeps the prev view*proj
     *   product, so the GI's prevView / prevProjection are the pass's own
     *   saved copies of the previous frame's jittered matrices. */
    static FfxBrixelizerGIContext giContext;
    static char        giContextReady;
    static char        giContextLogged;
    static VulkanImage giDiffuse;
    static VulkanImage giSpecular;
    static VulkanImage giDebug;
    static VulkanProfile giProfile;
    static char        giProfileReady;
    static mat4        giPrevView;
    static mat4        giPrevProjection;
    static char        giHavePrev;
    /* OFF by default: the GI cache debug visualization is a SEPARATE FFX
     * dispatch (its own fpExecuteGpuJobs), and the FFX VK backend advances its
     * per-effect-context frame index — destroying that frame's dynamic image
     * views — on EVERY ExecuteGpuJobs. Two GI dispatches per frame would halve
     * the view lifetime to ~2 engine frames and destroy views still in flight
     * (the voxelizer's single per-frame dispatch does not hit this). Enable
     * explicitly with ENGINE_BRIXGI_DEBUG=radiance|irradiance. */
    static char        giDebugEnabled = 0;
    static FfxBrixelizerGIDebugMode giDebugMode;
    static char        giDebugModeSet;
    /* One-shot: the cache debug visualization runs on a SINGLE frame
     * (giDebugFrame, counted from GI-context creation) rather than every
     * frame — a per-frame second dispatch would keep the FFX frame index at
     * 2x the engine frame rate, halving the dynamic-view lifetime. The
     * written giDebug image persists (it is not cleared per frame), so a
     * later dump reads the one-shot frame's cache. */
    static u32 giFrameCount = 0;
    static u32 giDebugFrame = 120;
    static char giDebugDone = 0;
    /* Step 8: GI on/off (ENGINE_BRIXGI, default 1 = on). When off the per-frame
     * GI dispatch is skipped and the composite pass passes sentinel GI indices
     * (the term is skipped entirely -> pixel-identical to pre-GI). The history
     * copy still runs (harmless; it feeds a disabled consumer). */
    static char        giEnabled = 1;
    static char        giEnabledSet = 0;
    static void resolveGiEnabled(void) {
        if (giEnabledSet) return;
        giEnabledSet = 1;
        const char* env = getenv("ENGINE_BRIXGI");
        if (env && !strcmp(env, "0")) giEnabled = 0;
    }

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
        envCubePipe  = vulkanCreatePipe(.name = "brixelizer_envcube",
                                        .comp = "shaders/pass/brixelizer/spv/envcube.comp.spv");
        envCubePipeReady = envCubePipe.pipe != 0;
        if (!envCubePipeReady) {
            utils::warn("vulkanBrixelizerPass: env-cube bake pipeline unavailable, environment cube stays empty");
        }
        giProfile      = vulkanCreateProfile("brixelizer_gi");
        giProfileReady = 1;
    }

    void VulkanBrixelizerPass::preUpdate() {
        if (profileReady) {
            /* force: the stats GUI is usually closed, but the brixelizer cost is
             * tracked in the log until Step 9 moves tuning to the GUI. */
            vulkanResetProfile(vulkan.currentCmd, &profile, 1);
        }
        if (giProfileReady) {
            vulkanResetProfile(vulkan.currentCmd, &giProfile, 1);
        }
        /* Deferred GPU buffer destruction (3 frames past the GPU queue depth,
         * like the heightmap pass's deferred descriptors). */
        for (i32 i = (i32)deferred.size() - 1; i >= 0; i--) {
            BrixelDeferred* d = &deferred[i];
            if (d->framesLeft > 1) {
                d->framesLeft--;
                continue;
            }
            if (d->unreg && contextReady) {
                u32 dropIdx[2] = {d->vertBufIdx, d->idxBufIdx};
                FfxErrorCode unregResult = ffxBrixelizerUnregisterBuffers(&brixelizerContext, dropIdx, 2);
                if (unregResult != FFX_OK) {
                    utils::error("vulkanBrixelizerPass: ffxBrixelizerUnregisterBuffers (deferred) failed: %d",
                                 unregResult);
                }
            }
            if (d->vertBuf.buf) {
                vulkanDestroyBuffer(&d->vertBuf, NULL);
            }
            if (d->idxBuf.buf) {
                vulkanDestroyBuffer(&d->idxBuf, NULL);
            }
            deferred[(u32)i] = deferred.back();
            deferred.pop_back();
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
        /* The GI env cube is re-baked with the (re)created resources (the
         * directional light is static for v1; the swapchain hook is the
         * plan's one-shot re-render point). */
        envCubeDirty = 1;
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
        propsStatsLogged           = 0;
        /* The FFX context (buffer/instance tables) died with the swapchain —
         * drop the terrain registrations and destroy the tile buffers. No
         * unregistration needed: the tables were recreated with the context. */
        terrainClearAll();
        /* The props variant buffers are device-level and survive the swapchain
         * (their contents do not change), but their FFX registrations and the
         * prop instance IDs died with the context — ensureContext re-registers
         * everything from the stored CPU data. */
        for (u32 i = 0; i < propsSets.size(); i++) {
            BrixelPropsSet* s = &propsSets[i];
            if (!s->inUse) {
                continue;
            }
            s->ids.clear();
        }
        propsContextRegistered = 0;
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
        for (u32 i = 0; i < deferred.size(); i++) {
            BrixelDeferred* d = &deferred[i];
            if (d->vertBuf.buf) {
                vulkanDestroyBuffer(&d->vertBuf, NULL);
            }
            if (d->idxBuf.buf) {
                vulkanDestroyBuffer(&d->idxBuf, NULL);
            }
        }
        deferred.clear();
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
        if (blueNoise.img) {
            vulkanDestroyImage(&blueNoise, NULL);
            blueNoise = VulkanImage{};
        }
        if (envCube.img) {
            vulkanDestroyImage(&envCube, NULL);
            envCube = VulkanImage{};
        }
        if (envFaceDump.img) {
            vulkanDestroyImage(&envFaceDump, NULL);
            envFaceDump = VulkanImage{};
        }
        if (historyDepth.img) {
            vulkanDestroyImage(&historyDepth, NULL);
            historyDepth = VulkanImage{};
        }
        if (historyNormal.img) {
            vulkanDestroyImage(&historyNormal, NULL);
            historyNormal = VulkanImage{};
        }
        if (historyLit.img) {
            vulkanDestroyImage(&historyLit, NULL);
            historyLit = VulkanImage{};
        }
        /* Step 7: GI outputs (destroyed with the voxelizer resources; the GI
         * context is destroyed first in destroyContext). */
        if (giDiffuse.img) {
            vulkanDestroyImage(&giDiffuse, NULL);
            giDiffuse = VulkanImage{};
        }
        if (giSpecular.img) {
            vulkanDestroyImage(&giSpecular, NULL);
            giSpecular = VulkanImage{};
        }
        if (giDebug.img) {
            vulkanDestroyImage(&giDebug, NULL);
            giDebug = VulkanImage{};
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
        /* The GI context holds a raw pointer into the voxelizer context, so
         * destroy it first (the swapchain-recreate / world-unload paths drain
         * the GPU before this runs). */
        giDestroyContext();
        if (contextReady) {
            ffxBrixelizerContextDestroy(&brixelizerContext);
            brixelizerContext = FfxBrixelizerContext{};
            contextReady      = 0;
        }
    }

    static void giDestroyContext(void) {
        if (giContextReady) {
            FfxErrorCode result = ffxBrixelizerGIContextDestroy(&giContext);
            if (result != FFX_OK) {
                utils::error("vulkanBrixelizerPass: ffxBrixelizerGIContextDestroy failed: %d", result);
            }
            giContext      = FfxBrixelizerGIContext{};
            giContextReady = 0;
            /* The temporal prev matrices are only valid across frames of one
             * GI context — drop them so the next context starts clean. */
            giHavePrev     = 0;
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

        /* ── Step 6: GI inputs ──────────────────────────────────── */
        const u32 giResW = renderW;
        const u32 giResH = renderH;

        /* 6.1 Blue noise: 128² RG8 rank mask, generated on the CPU once and
         * uploaded (the GI shader masks pixel coords with & 127 and reads
         * .xy — 128² is the native tile size). */
        static const u32 GI_NOISE_SIZE = 128;
        std::vector<u8> noiseData((size_t)GI_NOISE_SIZE * GI_NOISE_SIZE * 2);
        blueNoiseGenerate(noiseData.data(), GI_NOISE_SIZE);
        blueNoise = vulkanCreateImage(.name    = "BrixelBlueNoise",
                                      .format  = VK_FORMAT_R8G8_UNORM,
                                      .usage   = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                      .width   = (int)GI_NOISE_SIZE,
                                      .height  = (int)GI_NOISE_SIZE);

        /* 6.2 Environment cube: 128²×6 RGBA16F, CUBE_COMPATIBLE (the FFX
         * backend keys the cube wrap on that flag — pitfall #14). Both
         * SAMPLED + STORAGE: the engine bake writes it through the
         * imageCube storage pool, GI reads it as a samplerCube. */
        static const u32 GI_ENV_CUBE_SIZE = 128;
        envCube = vulkanCreateImage(.name     = "BrixelEnvCube",
                                    .format   = VK_FORMAT_R16G16B16A16_SFLOAT,
                                    .usage    = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    .flags    = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                                    .viewType = VK_IMAGE_VIEW_TYPE_CUBE,
                                    .width    = (int)GI_ENV_CUBE_SIZE,
                                    .height   = (int)GI_ENV_CUBE_SIZE,
                                    .layers   = 6);

        /* 6.3 History buffers (render res). STORAGE|SAMPLED: the GI context
         * (Step 7) re-wraps them as UAVs; TRANSFER for the per-frame copies
         * and dumps. */
        historyDepth =
            vulkanCreateImage(.name   = "BrixelHistoryDepth",
                              .format = VK_FORMAT_R32_SFLOAT,
                              .usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              .width  = (int)giResW,
                              .height = (int)giResH);
        historyNormal =
            vulkanCreateImage(.name   = "BrixelHistoryNormal",
                              .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                              .usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              .width  = (int)giResW,
                              .height = (int)giResH);
        historyLit =
            vulkanCreateImage(.name   = "BrixelHistoryLitOutput",
                              .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                              .usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              .width  = (int)giResW,
                              .height = (int)giResH);
        if (!blueNoise.img || !envCube.img || !historyDepth.img || !historyNormal.img || !historyLit.img) {
            utils::error("vulkanBrixelizerPass: GI input resource creation failed");
            destroyResources();
            return 0;
        }

        /* One-time: SDF atlas clear so pre-bake dumps are predictable
         * (0 = no brick allocated yet); the FFX clear-bricks pass only
         * rewrites bricks that were previously allocated. History buffers
         * clear to 0.0 (background — reverse-Z) and the noise mask is
         * uploaded in the same transient command. */
        VulkanCommand* cmd = vulkanTransientBegin();
        vulkanTransition(cmd, &sdfAtlas, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        VkClearColorValue black = {};
        vulkanClearColorImage(cmd, &sdfAtlas, black);
        vulkanTransition(cmd, &blueNoise, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);
        vulkanCopy(.cmd         = cmd,
                   .target.img  = &blueNoise,
                   .source.data = noiseData.data(),
                   .size        = (u32)noiseData.size());
        vulkanTransition(cmd, &blueNoise, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, &historyDepth, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        vulkanClearColorImage(cmd, &historyDepth, black);
        vulkanTransition(cmd, &historyNormal, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        vulkanClearColorImage(cmd, &historyNormal, black);
        vulkanTransition(cmd, &historyLit, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        vulkanClearColorImage(cmd, &historyLit, black);
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
        BrixelDeferred d = {};
        d.vertBuf    = e->vertBuf;
        d.idxBuf     = e->idxBuf;
        d.vertBufIdx = e->vertBufIdx;
        d.idxBufIdx  = e->idxBufIdx;
        d.unreg      = contextReady;
        d.framesLeft = 3;
        deferred.push_back(d);
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

    /* The 3×4 ROW-major instance transform (plan pitfall #2 — the GLSL loads
     * 3 rows and applies them row-vector style): T(pos)·R_y(yaw)·S(scale),
     * matching azgaar_props.vert (x' = cy·x − sy·z, z' = sy·x + cy·z, both ×
     * scale + pos). */
    static void propsFillDesc(BrixelPropsVariant* v, const PropInstance* inst,
                              FfxBrixelizerInstanceDescription* d) {
        float cy     = cosf(inst->yaw);
        float sy     = sinf(inst->yaw);
        float s      = inst->scale;
        d->transform[0]  = s * cy;
        d->transform[1]  = 0.0f;
        d->transform[2]  = s * sy;
        d->transform[3]  = inst->pos[0];
        d->transform[4]  = 0.0f;
        d->transform[5]  = s;
        d->transform[6]  = 0.0f;
        d->transform[7]  = inst->pos[1];
        d->transform[8]  = -s * sy;
        d->transform[9]  = 0.0f;
        d->transform[10] = s * cy;
        d->transform[11] = inst->pos[2];

        /* World AABB: the local bounds rotated around Y — only the four XZ
         * corners move, Y is translation + scale. */
        float xs[2] = {v->boundsMin[0] * s, v->boundsMax[0] * s};
        float zs[2] = {v->boundsMin[2] * s, v->boundsMax[2] * s};
        float rx0 = cy * xs[0] - sy * zs[0];
        float rx1 = cy * xs[0] - sy * zs[1];
        float rx2 = cy * xs[1] - sy * zs[0];
        float rx3 = cy * xs[1] - sy * zs[1];
        float rz0 = cy * zs[0] - sy * xs[0];
        float rz1 = cy * zs[1] - sy * xs[0];
        float rz2 = cy * zs[0] - sy * xs[1];
        float rz3 = cy * zs[1] - sy * xs[1];
        d->aabb.min[0] = inst->pos[0] + fminf(fminf(rx0, rx1), fminf(rx2, rx3));
        d->aabb.max[0] = inst->pos[0] + fmaxf(fmaxf(rx0, rx1), fmaxf(rx2, rx3));
        d->aabb.min[1] = inst->pos[1] + v->boundsMin[1] * s;
        d->aabb.max[1] = inst->pos[1] + v->boundsMax[1] * s;
        d->aabb.min[2] = inst->pos[2] + fminf(fminf(rz0, rz1), fminf(rz2, rz3));
        d->aabb.max[2] = inst->pos[2] + fmaxf(fmaxf(rz0, rz1), fmaxf(rz2, rz3));
        /* 0.1 m inflation: the SDF is voxel-resolution, keep grazing traces
         * from clipping the surface. */
        for (u32 a = 0; a < 3; a++) {
            d->aabb.min[a] -= 0.1f;
            d->aabb.max[a] += 0.1f;
        }
        d->vertexBuffer           = v->vertBufIdx;
        d->vertexStride           = 12;
        d->vertexBufferOffset     = 0;
        d->vertexCount            = v->vertCount;
        d->vertexFormat           = FFX_SURFACE_FORMAT_R32G32B32_FLOAT;
        d->indexBuffer            = v->idxBufIdx;
        d->indexBufferOffset      = 0;
        d->indexFormat            = (v->indexType == FFX_INDEX_TYPE_UINT32) ? FFX_INDEX_TYPE_UINT32
                                                                             : FFX_INDEX_TYPE_UINT16;
        d->triangleCount          = v->triCount;
        d->maxCascade             = v->maxCascade;
        d->flags                  = FFX_BRIXELIZER_INSTANCE_FLAG_NONE;
    }

    static BrixelPropsSet* propsSetFind(u32 kind, i32 tileX, i32 tileZ) {
        for (u32 i = 0; i < propsSets.size(); i++) {
            BrixelPropsSet* s = &propsSets[i];
            if (s->inUse && s->kind == kind && s->tileX == tileX && s->tileZ == tileZ) {
                return s;
            }
        }
        if (kind == 0) {
            BrixelPropsSet* s = &propsSets.emplace_back(BrixelPropsSet{});
            s->kind           = 0;
            s->tileX          = tileX;
            s->tileZ          = tileZ;
            s->inUse          = 1;
            return s;
        }
        for (u32 i = 0; i < propsSets.size(); i++) {
            if (propsSets[i].inUse && propsSets[i].kind == kind) {
                return &propsSets[i];
            }
        }
        BrixelPropsSet* s = &propsSets.emplace_back(BrixelPropsSet{});
        s->kind           = kind;
        s->inUse          = 1;
        return s;
    }

    static void propsSetDelete(BrixelPropsSet* s) {
        if (s->ids.size() > 0 && contextReady) {
            FfxErrorCode delResult =
                ffxBrixelizerDeleteInstances(&brixelizerContext, s->ids.data(), (u32)s->ids.size());
            if (delResult != FFX_OK) {
                utils::error("vulkanBrixelizerPass: ffxBrixelizerDeleteInstances (props set) failed: %d",
                             delResult);
            }
        }
        s->ids.clear();
        totalRegisteredInstances -= s->accepted;
        totalRegisteredTriangles -= s->tris;
        s->accepted = 0;
        s->tris     = 0;
    }

    static void propsResolveBudget(void) {
        if (propsBudgetSet) {
            return;
        }
        propsBudgetSet = 1;
        const char* env = getenv("ENGINE_BRIXGI_PROP_BUDGET");
        propsBudget     = (env && *env) ? (u32)atoi(env) : 40960;
    }

    /* Creates one FFX static instance per accepted PropInstance of a set.
     * `kind` selects the set (0 tile / 1 global / 2 landmarks); the camera
     * position drives the distance-based budget priority. Replacing an
     * existing set deletes its instances first (LOD re-culls, tile re-scatter,
     * variant-table swap all re-push the same set). */
    static void propsSetApply(u32 kind, i32 tileX, i32 tileZ, u64 readyStamp,
                             const PropInstance* insts, u32 count, const float* camPos) {
        propsResolveBudget();
        if (!contextReady || count == 0) {
            return;
        }
        BrixelPropsSet* s = propsSetFind(kind, tileX, tileZ);
        if (s->inUse && (s->ids.size() > 0 || s->accepted > 0)) {
            propsSetDelete(s);
            s->instances.clear();
            s->accepted = 0;
            s->dropped  = 0;
        }

        /* Candidates: only instances whose (species, variant) has a registered
         * SDF buffer — the rest (unloaded / unregistered species) never reach
         * the voxelizer and must not count against the budget. */
        std::vector<u32> cand;
        cand.reserve(count);
        for (u32 i = 0; i < count; i++) {
            const BrixelPropsVariant* v =
                propsVariantFind(insts[i].species, insts[i].variant);
            if (v && v->registered) {
                cand.push_back(i);
            }
        }

        /* Budget: the global prop cap (default 40 k) plus the shared instance
         * table headroom for Step 10's dynamic instances. */
        u32 others = 0;
        for (u32 i = 0; i < propsSets.size(); i++) {
            if (propsSets[i].inUse && &propsSets[i] != s) {
                others += propsSets[i].accepted;
            }
        }
        u32 capacity = (propsBudget > others) ? (propsBudget - others) : 0;
        u32 tableCap = (totalRegisteredInstances + BRIX_DYNAMIC_HEADROOM < FFX_BRIXELIZER_MAX_INSTANCES)
                           ? (FFX_BRIXELIZER_MAX_INSTANCES - totalRegisteredInstances - BRIX_DYNAMIC_HEADROOM)
                           : 0;
        if (tableCap < capacity) {
            capacity = tableCap;
        }

        if (capacity < cand.size()) {
            /* Priority (plan Step 5.2): species class (sdfPriority — canopy /
             * buildings first, grass tufts last), then distance to the camera.
             * camPos may be NULL (the context-recreation reapply has no live
             * camera at that point) — the distance term is then a constant and
             * the sort degrades to pure species priority. */
            float cam[3];
            if (camPos) {
                cam[0] = camPos[0];
                cam[1] = camPos[1];
                cam[2] = camPos[2];
            } else {
                cam[0] = 0.0f;
                cam[1] = 0.0f;
                cam[2] = 0.0f;
            }
            std::stable_sort(cand.begin(),
                             cand.end(),
                             [&](u32 a, u32 b) {
                                 const PropInstance* ia = &insts[a];
                                 const PropInstance* ib = &insts[b];
                                 u32 pa = 255, pb = 255;
                                 const BrixelPropsVariant* va = propsVariantFind(ia->species, ia->variant);
                                 const BrixelPropsVariant* vb = propsVariantFind(ib->species, ib->variant);
                                 if (va) {
                                     pa = va->priority;
                                 }
                                 if (vb) {
                                     pb = vb->priority;
                                 }
                                 if (pa != pb) {
                                     return pa < pb;
                                 }
                                 float dx = ia->pos[0] - cam[0];
                                 float dy = ia->pos[1] - cam[1];
                                 float dz = ia->pos[2] - cam[2];
                                 float ex = ib->pos[0] - cam[0];
                                 float ey = ib->pos[1] - cam[1];
                                 float ez = ib->pos[2] - cam[2];
                                 return dx * dx + dy * dy + dz * dz < ex * ex + ey * ey + ez * ez;
                             });
            for (u32 i = 0; i < capacity; i++) {
                s->instances.push_back(insts[cand[i]]);
            }
            s->dropped = count - (u32)s->instances.size();
        } else {
            for (u32 i = 0; i < cand.size(); i++) {
                s->instances.push_back(insts[cand[i]]);
            }
            s->dropped = count - (u32)s->instances.size();
        }
        s->accepted = (u32)s->instances.size();
        s->readyStamp = readyStamp;

        if (s->accepted == 0) {
            return;
        }

        std::vector<FfxBrixelizerInstanceDescription> descs(s->accepted);
        std::vector<FfxBrixelizerInstanceID> ids(s->accepted);
        u32 setTris = 0;        for (u32 i = 0; i < s->accepted; i++) {
            FfxBrixelizerInstanceDescription* d = &descs[i];
            PropInstance* inst                   = &s->instances[i];
            BrixelPropsVariant* v =
                propsVariantFind(inst->species, inst->variant);
            if (!v || !v->registered) {
                continue; /* filtered above; defensive */
            }
            propsFillDesc(v, inst, d);
            d->outInstanceID = &ids[i];
            setTris += v->triCount;
        }
        FfxErrorCode instResult =
            ffxBrixelizerCreateInstances(&brixelizerContext, descs.data(), s->accepted);
        if (instResult != FFX_OK) {
            utils::error("vulkanBrixelizerPass: ffxBrixelizerCreateInstances (props) failed: %d",
                         instResult);
            s->instances.clear();
            s->accepted = 0;
            return;
        }
        for (u32 i = 0; i < s->accepted; i++) {
            s->ids.push_back(ids[i]);
        }
        s->tris = setTris;
        totalRegisteredInstances += s->accepted;
        totalRegisteredTriangles += s->tris;
        const char* setKind = (kind == 0) ? "tile" : (kind == 1) ? "settlements" : "landmarks";
        utils::info("vulkanBrixelizerPass: props %s(%d,%d) SDF: %u/%u accepted (dropped %u, "
                    "budget %u; totals: %u instances / %u tris, cap %u)",
                    setKind,
                    tileX,
                    tileZ,
                    s->accepted,
                    count,
                    s->dropped,
                    propsBudget,
                    totalRegisteredInstances,
                    totalRegisteredTriangles,
                    FFX_BRIXELIZER_MAX_INSTANCES);
        if (!propsStatsLogged) {
            propsStatsLogged = 1;
            utils::info("vulkanBrixelizerPass: props registration frame: pass gpu=%.3f ms (includes the first prop bakes)",
                        profile.elapsed / MILLION);
        }
    }

    static BrixelPropsVariant* propsVariantFind(u32 species, u32 variant) {
        for (u32 i = 0; i < propsVariants.size(); i++) {
            if (propsVariants[i].species == species && propsVariants[i].variant == variant) {
                return &propsVariants[i];
            }
        }
        return NULL;
    }

    static void propsSyncPending(const float* camPos) {
        /* Move the queue out under the lock (worker threads push into it);
         * the FFX work runs unlocked on the render thread. */
        std::vector<BrixelPropsPending> batch;
        utils::threadLock(&propsLock);
        batch = std::move(propsPending);
        propsPending.clear();
        utils::threadUnlock(&propsLock);
        if (!batch.size()) {
            return;
        }
        for (u32 i = 0; i < batch.size(); i++) {
            BrixelPropsPending* p = &batch[i];
            switch (p->kind) {
            case BRIX_PROPS_MESH_SET:
                if (contextReady) {
                    /* The new variant table replaces the old buffers — delete
                     * every prop instance (they reference the old variants),
                     * defer-destroy the old variant buffers, register the new
                     * ones, and recreate the stored accepted sets against the
                     * new buffers. */
                    for (u32 j = 0; j < propsSets.size(); j++) {
                        if (propsSets[j].inUse) {
                            propsSetDelete(&propsSets[j]);
                        }
                    }
                    for (u32 j = 0; j < propsVariants.size(); j++) {
                        BrixelPropsVariant* v = &propsVariants[j];
                        if (!v->registered) {
                            continue;
                        }
                        v->registered = 0;
                        if (v->vertBuf.buf || v->idxBuf.buf) {
                            BrixelDeferred d = {};
                            d.vertBuf    = v->vertBuf;
                            d.idxBuf     = v->idxBuf;
                            d.vertBufIdx = v->vertBufIdx;
                            d.idxBufIdx  = v->idxBufIdx;
                            d.unreg      = 1;
                            d.framesLeft = 3;
                            deferred.push_back(d);
                            v->vertBuf = VulkanBuffer{};
                            v->idxBuf  = VulkanBuffer{};
                        }
                    }
                    propsEnsureRegistered();
                    for (u32 j = 0; j < propsSets.size(); j++) {
                        BrixelPropsSet* s = &propsSets[j];
                        if (s->inUse && s->instances.size() > 0) {
                            s->ids.clear();
                            s->accepted = 0;
                            s->tris     = 0;
                            propsSetApply(s->kind, s->tileX, s->tileZ, s->readyStamp,
                                          s->instances.data(), (u32)s->instances.size(), camPos);
                        }
                    }
                }
                break;
            case BRIX_PROPS_MESH_CLEAR:
                /* propsVariantCpu / propsVariantTable are owned by the game
                 * thread (cleared under the lock when this item was pushed);
                 * the render thread only resets its own registered state here. */
                for (u32 j = 0; j < propsSets.size(); j++) {
                    if (propsSets[j].inUse) {
                        propsSetDelete(&propsSets[j]);
                        propsSets[j] = BrixelPropsSet{};
                    }
                }
                for (u32 j = 0; j < propsVariants.size(); j++) {
                    BrixelPropsVariant* v = &propsVariants[j];
                    if (!v->registered) {
                        continue;
                    }
                    v->registered = 0;
                    if (v->vertBuf.buf || v->idxBuf.buf) {
                        BrixelDeferred d = {};
                        d.vertBuf    = v->vertBuf;
                        d.idxBuf     = v->idxBuf;
                        d.vertBufIdx = v->vertBufIdx;
                        d.idxBufIdx  = v->idxBufIdx;
                        d.unreg      = 1;
                        d.framesLeft = 3;
                        deferred.push_back(d);
                        v->vertBuf = VulkanBuffer{};
                        v->idxBuf  = VulkanBuffer{};
                    }
                }
                /* propsVariantCpu / propsVariantTable are owned by the game
                 * thread (cleared under the lock when this item was pushed);
                 * the render thread only resets its own registered state. */
                break;
            case BRIX_PROPS_TILE_SET:
                propsSetApply(0, p->tileX, p->tileZ, p->readyStamp, p->instances.data(),
                              (u32)p->instances.size(), camPos);
                break;
            case BRIX_PROPS_TILE_CLEAR: {
                BrixelPropsSet* s = propsSetFind(0, p->tileX, p->tileZ);
                /* propsSetFind creates the entry — drop it again when absent
                 * from the previous state. */
                if (s->ids.size() == 0 && s->instances.size() == 0) {
                    propsSets[(u32)std::distance(propsSets.data(), s)] = BrixelPropsSet{};
                    break;
                }
                propsSetDelete(s);
                s->instances.clear();
                *s = BrixelPropsSet{};
                break;
            }
            case BRIX_PROPS_GLOBAL_SET:
                propsSetApply(1, 0, 0, p->readyStamp, p->instances.data(), (u32)p->instances.size(),
                              camPos);
                break;
            case BRIX_PROPS_GLOBAL_CLEAR: {
                BrixelPropsSet* s = propsSetFind(1, 0, 0);
                if (s->ids.size() == 0 && s->instances.size() == 0) {
                    propsSets[(u32)std::distance(propsSets.data(), s)] = BrixelPropsSet{};
                    break;
                }
                propsSetDelete(s);
                s->instances.clear();
                *s = BrixelPropsSet{};
                break;
            }
            case BRIX_PROPS_LANDMARKS_SET:
                propsSetApply(2, 0, 0, p->readyStamp, p->instances.data(), (u32)p->instances.size(),
                              camPos);
                break;
            case BRIX_PROPS_LANDMARKS_CLEAR: {
                BrixelPropsSet* s = propsSetFind(2, 0, 0);
                if (s->ids.size() == 0 && s->instances.size() == 0) {
                    propsSets[(u32)std::distance(propsSets.data(), s)] = BrixelPropsSet{};
                    break;
                }
                propsSetDelete(s);
                s->instances.clear();
                *s = BrixelPropsSet{};
                break;
            }
            default:
                break;
            }
        }
    }

    static void propsEnsureRegistered(void) {
        if (!contextReady) {
            return;
        }
        /* propsVariantCpu is written by the game thread (azgaarPropsInit /
         * propsRebuildAndPushMeshes) under propsLock — hold the lock while
         * reading it. The GPU upload inside propsVariantRegister is a fence
         * wait; the game thread only holds the lock during extraction, so a
         * few ms of blocking at world load is the worst case. */
        utils::threadLock(&propsLock);
        if (propsVariantCpu.size() == 0) {
            utils::threadUnlock(&propsLock);
            return;
        }
        /* propsVariants stays index-aligned with propsVariantCpu: a world
         * switch (MESH_SET) resets the entries in place, swapchain recreation
         * keeps the device-level buffers and only re-registers them with the
         * new FFX context. */
        if (propsVariants.size() != propsVariantCpu.size()) {
            propsVariants.resize(propsVariantCpu.size());
        }
        for (u32 i = 0; i < propsVariantCpu.size(); i++) {
            propsVariantRegister(i);
        }
        utils::threadUnlock(&propsLock);
        if (propsContextRegistered) {
            return;
        }
        propsContextRegistered = 1;
        /* The FFX instance tables were recreated with the context — recreate
         * the stored accepted sets (swapchain recreation; nothing re-pushes
         * them). */
        for (u32 i = 0; i < propsSets.size(); i++) {
            BrixelPropsSet* s = &propsSets[i];
            if (s->inUse && s->instances.size() > 0) {
                s->ids.clear();
                s->accepted = 0;
                s->tris     = 0;
                propsSetApply(s->kind, s->tileX, s->tileZ, s->readyStamp, s->instances.data(),
                              (u32)s->instances.size(), NULL);
            }
        }
    }

    /* Uploads + FFX-registers one variant's position-only + index buffers (the
     * CPU data comes from the extraction at SetPropsMeshes time). Render
     * thread only; called from propsEnsureRegistered. */
    static void propsVariantRegister(u32 i) {
        BrixelPropsVariantCpu* c = &propsVariantCpu[i];
        BrixelPropsVariant* v    = &propsVariants[i];
        if (c->vertCount == 0) {
            return;
        }
        if (v->registered) {
            return;
        }
        v->species    = c->species;
        v->variant    = c->variant;
        v->vertCount  = c->vertCount;
        v->triCount   = c->triCount;
        v->maxCascade = c->maxCascade;
        v->priority   = c->priority;
        v->indexType  = c->indexType;
        memcpy(v->boundsMin, c->boundsMin, sizeof(v->boundsMin));
        memcpy(v->boundsMax, c->boundsMax, sizeof(v->boundsMax));
        if (!v->vertBuf.buf) {
            v->vertBuf = vulkanCreateGpuBuffer(utils::strtmp("BrixelPropsVerts %u_%u", c->species,
                                                              c->variant),
                                               (u64)c->vertCount * 12,
                                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            u32 idxCount =
                (c->indexType == FFX_INDEX_TYPE_UINT32) ? (u32)c->idx32.size() : (u32)c->idx16.size();
            v->idxBuf = vulkanCreateGpuBuffer(utils::strtmp("BrixelPropsIdx %u_%u", c->species,
                                                             c->variant),
                                              (u64)idxCount * ((c->indexType == FFX_INDEX_TYPE_UINT32) ? 4u : 2u),
                                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            if (!v->vertBuf.buf || !v->idxBuf.buf) {
                utils::error("vulkanBrixelizerPass: props variant (%u,%u) buffer creation failed",
                             c->species,
                             c->variant);
                if (v->vertBuf.buf) {
                    vulkanDestroyBuffer(&v->vertBuf, NULL);
                    v->vertBuf = VulkanBuffer{};
                }
                if (v->idxBuf.buf) {
                    vulkanDestroyBuffer(&v->idxBuf, NULL);
                    v->idxBuf = VulkanBuffer{};
                }
                return;
            }
            VulkanCommand* tcmd = vulkanTransientBegin();
            vulkanCopy(.cmd         = tcmd,
                       .source.data = c->verts.data(),
                       .target.buf  = &v->vertBuf,
                       .size        = (u32)(c->vertCount * 12));
            if (c->indexType == FFX_INDEX_TYPE_UINT32) {
                vulkanCopy(.cmd         = tcmd,
                           .source.data = c->idx32.data(),
                           .target.buf  = &v->idxBuf,
                           .size        = (u32)(c->idx32.size() * sizeof(u32)));
            } else {
                vulkanCopy(.cmd         = tcmd,
                           .source.data = c->idx16.data(),
                           .target.buf  = &v->idxBuf,
                           .size        = (u32)(c->idx16.size() * sizeof(u16)));
            }
            /* Wait: the FFX voxelizer reads these as SSBs, the data must be
             * complete before the bake dispatch. */
            vulkanTransientEnd(tcmd, 1);
        }
        FfxResource vRes =
            vulkanFfxWrapBufferResource(&v->vertBuf,
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ,
                                        L"BrixelPropsVerts");
        FfxResource iRes =
            vulkanFfxWrapBufferResource(&v->idxBuf,
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ,
                                        L"BrixelPropsIdx");
        FfxBrixelizerBufferDescription bufDescs[2] = {};
        bufDescs[0].buffer   = vRes;
        bufDescs[0].outIndex = &v->vertBufIdx;
        bufDescs[1].buffer   = iRes;
        bufDescs[1].outIndex = &v->idxBufIdx;
        FfxErrorCode regResult = ffxBrixelizerRegisterBuffers(&brixelizerContext, bufDescs, 2);
        if (regResult != FFX_OK) {
            utils::error("vulkanBrixelizerPass: props variant (%u,%u) ffxBrixelizerRegisterBuffers failed: %d",
                         c->species,
                         c->variant,
                         regResult);
            return;
        }
        v->registered = 1;
    }

    /* Extracts the per-(species, variant) position-only sub-buffers from the
     * merged 72 B species mesh (plan Step 5.1 — pitfall #8): one 12 B/vertex
     * buffer + a u16 (u32 when the variant spans > 64 k vertices) index
     * sub-range per variant. Runs on the game thread at azgaarPropsInit; the
     * GPU upload / FFX registration happens in propsEnsureRegistered (render
     * thread, once the context exists). */
    static char propsExtractMeshes(const void* verts, u32 vertCount, const void* idx, u32 idxCount,
                                   const PropVariantRange* variants, u32 variantCount) {
        propsVariantCpu.clear();
        propsVariantTable.clear();
        if (!verts || vertCount == 0 || !idx || idxCount == 0 || !variants || variantCount == 0) {
            return 1;
        }
        const PropsVertex* pv = (const PropsVertex*)verts;
        const u32* mi        = (const u32*)idx;
        propsVariantTable.assign(variants, variants + variantCount);
        propsVariantCpu.resize(variantCount);
        for (u32 r = 0; r < variantCount; r++) {
            const PropVariantRange* vr = &variants[r];
            BrixelPropsVariantCpu* c   = &propsVariantCpu[r];
            c->species    = vr->species;
            c->variant    = vr->variant;
            c->maxCascade = vr->sdfMaxCascade;
            c->priority   = vr->sdfPriority;
            memcpy(c->boundsMin, vr->boundsMin, sizeof(c->boundsMin));
            memcpy(c->boundsMax, vr->boundsMax, sizeof(c->boundsMax));
            u32 i0 = vr->indexOffset;
            u32 ic = vr->indexCount;
            if (i0 >= idxCount || i0 + ic > idxCount || ic == 0) {
                continue;
            }
            u32 minV = vertCount, maxV = 0;
            for (u32 k = 0; k < ic; k++) {
                u32 vi = mi[i0 + k];
                if (vi < minV) {
                    minV = vi;
                }
                if (vi >= maxV) {
                    maxV = vi + 1;
                }
            }
            if (minV >= maxV || minV >= vertCount || maxV > vertCount) {
                utils::error("vulkanBrixelizerPass: props variant (%u,%u) index range out of bounds", vr->species, vr->variant);
                continue;
            }
            c->vertCount = maxV - minV;
            c->triCount  = ic / 3;
            c->verts.resize((size_t)c->vertCount * 3);
            for (u32 v = minV; v < maxV; v++) {
                c->verts[(v - minV) * 3 + 0] = pv[v].position[0];
                c->verts[(v - minV) * 3 + 1] = pv[v].position[1];
                c->verts[(v - minV) * 3 + 2] = pv[v].position[2];
            }
            if (c->vertCount > 0xFFFF) {
                c->indexType = FFX_INDEX_TYPE_UINT32;
                c->idx32.resize(ic);
                for (u32 k = 0; k < ic; k++) {
                    c->idx32[k] = mi[i0 + k] - minV;
                }
            } else {
                c->indexType = FFX_INDEX_TYPE_UINT16;
                c->idx16.resize(ic);
                for (u32 k = 0; k < ic; k++) {
                    c->idx16[k] = (u16)(mi[i0 + k] - minV);
                }
            }
            utils::info("vulkanBrixelizerPass: props SDF variant (%u,%u): %u verts / %u tris (%s indices, maxCascade %u, priority %u)",
                         vr->species,
                         vr->variant,
                         c->vertCount,
                         c->triCount,
                         c->indexType == FFX_INDEX_TYPE_UINT32 ? "u32" : "u16",
                         c->maxCascade,
                         c->priority);
        }
        return 1;
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
        /* Step 5: re-register the props variant buffers + recreate the stored
         * prop instance sets (no-op when no props world is loaded). */
        propsEnsureRegistered();
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

        /* Step 7.1: ensure the GI context + outputs exist. displaySize is fixed
         * at creation, so a resolution change rebuilds them (the swapchain
         * hook has already torn the old ones down). */
        u32 giW = window.renderWidth > 0 ? (u32)window.renderWidth : (u32)window.width;
        u32 giH = window.renderHeight > 0 ? (u32)window.renderHeight : (u32)window.height;
        giEnsureContext(giW, giH);

        /* Step 6.2: re-bake the environment cube after the (re)created
         * resources — one-shot compute in a fence-waiting transient command.
         * The directional light is static for v1, so the swapchain hook is
         * the only re-bake trigger. */
        if (envCubeDirty) {
            renderEnvCube();
            envCubeDirty = 0;
        }

        /* Step 4: sync the streaming heightmap tiles with the SDF (register
         * newly READY tiles, evict out-of-window ones) before this frame's
         * bake dispatch so new tiles are baked with the rest. */
        terrainSyncTiles();

        /* Step 5: drain the thread-safe props queue (tile scatters / global
         * sets pushed by the azgaar_props pass, mesh-table pushes pushed by
         * azgaarPropsInit) — budgeted instance creation before this frame's
         * bake. */
        propsSyncPending(camera->cameraUbo.renderLocation);
        /* Step 2.2: read the debug-visualization mode once (ENGINE_BRIX_SDF_DEBUG;
         * "off" disables the extra dispatch). */
        getSdfDebugMode();
        /* Step 7.3: parse the GI cache debug mode once (ENGINE_BRIXGI_DEBUG;
         * off by default — a second per-frame FFX dispatch would halve the FFX
         * view lifetime). giDebugDispatch checks giDebugEnabled. */
        getGIDebugMode();

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

        /* Step 7.2: GI dispatch — after the voxelizer wrote this frame's SDF
         * (the GI ray-march reads the atlas the update just produced). */
        giDispatch(cmd, camera);
        /* Step 7.3: GI cache debug visualization (radiance / irradiance).
         * ONE-SHOT on giFrameCount == giDebugFrame (a single extra FFX dispatch,
         * safe — see the giDebugFrame comment); reads the cache the dispatch
         * above just updated. */
        giFrameCount++;
        giDebugDispatch(cmd, camera);

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
        giHistoryCopy();
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

    /* ── Step 6: GI inputs ───────────────────────────────────────── */

    /* Deterministic PRNG for the blue-noise generation (file scope: the
     * noise mask is generated once per resource creation, not per frame). */
    static u32 noiseRandState = 0x9E3779B9u;
    static float noiseRand(void) {
        noiseRandState ^= noiseRandState << 13;
        noiseRandState ^= noiseRandState >> 17;
        noiseRandState ^= noiseRandState << 5;
        return (float)(noiseRandState >> 8) * (1.0f / 16777216.0f);
    }

    /* 6.1: 128² blue-noise rank mask (RG8). Bridson's fast Poisson-disk
     * (best-candidate) sampling on the pixel-grid torus: each new point sits
     * 1–2 px from a random existing point and is the most isolated of 30
     * darts. The generation order is the rank, stored 16-bit as r = low byte
     * / g = high byte (the sample's LDR_RG01_* masks are 8-bit RG pairs the
     * same way). The GI shader masks pixel coords with & 127 and reads .xy,
     * so 128² is the native tile size. */
    static void blueNoiseGenerate(u8* out, u32 width) {
        const u32 W     = width;
        const u32 COUNT = W * W;
        std::vector<float> px(COUNT);
        std::vector<float> py(COUNT);
        std::vector<int> pixIdx((size_t)COUNT, -1);
        std::vector<int> active;
        active.reserve(COUNT);

        u32 count = 0;
        px[count] = ((float)((u32)(noiseRand() * (float)W) % W) + 0.5f) / (float)W;
        py[count] = ((float)((u32)(noiseRand() * (float)W) % W) + 0.5f) / (float)W;
        pixIdx[(u32)(py[count] * (float)W) * W + (u32)(px[count] * (float)W)] = (int)count;
        active.push_back((int)count);
        count++;

        while (count < COUNT && !active.empty()) {
            u32 ai = (u32)(noiseRand() * (float)active.size()) % active.size();
            int  p  = active[ai];
            float bestD2 = -1.0f;
            int bestIx = -1;
            int bestIy = -1;
            for (u32 c = 0; c < 30; c++) {
                float ang = noiseRand() * 2.0f * (float)M_PI;
                float rad = 1.0f + noiseRand();
                float fx  = px[p] + cosf(ang) * rad * (1.0f / (float)W);
                float fy  = py[p] + sinf(ang) * rad * (1.0f / (float)W);
                fx -= floorf(fx);
                fy -= floorf(fy);
                int ix = (int)(fx * (float)W) % (int)W;
                int iy = (int)(fy * (float)W) % (int)W;
                if (ix < 0) ix += (int)W;
                if (iy < 0) iy += (int)W;
                if (pixIdx[(size_t)iy * W + ix] >= 0) {
                    continue;
                }
                /* Squared distance (px, toroidal) to the nearest existing
                 * point in the candidate's 5×5 pixel window — the dart
                 * radius is ≤ 2 px, so the window covers the constraint. */
                float d2min = 1e30f;
                for (int dy = -2; dy <= 2; dy++) {
                    int jy = (iy + dy + (int)W) % (int)W;
                    for (int dx = -2; dx <= 2; dx++) {
                        int jx = (ix + dx + (int)W) % (int)W;
                        int j  = pixIdx[(size_t)jy * W + jx];
                        if (j < 0) {
                            continue;
                        }
                        float ddx = fx * (float)W - (float)(jx + 0.5);
                        float ddy = fy * (float)W - (float)(jy + 0.5);
                        ddx       = fminf(fabsf(ddx), (float)W - fabsf(ddx));
                        ddy       = fminf(fabsf(ddy), (float)W - fabsf(ddy));
                        float d2  = ddx * ddx + ddy * ddy;
                        if (d2 < d2min) {
                            d2min = d2;
                        }
                    }
                }
                if (d2min > bestD2) {
                    bestD2 = d2min;
                    bestIx = ix;
                    bestIy = iy;
                }
            }
            if (bestIx < 0) {
                active[ai] = active.back();
                active.pop_back();
                continue;
            }
            px[count] = ((float)bestIx + 0.5f) / (float)W;
            py[count] = ((float)bestIy + 0.5f) / (float)W;
            pixIdx[(size_t)bestIy * W + bestIx] = (int)count;
            active.push_back((int)count);
            count++;
        }
        /* Any stragglers (Bridson can run dry before full coverage) fill the
        * remaining pixels at their later ranks. */
        while (count < COUNT) {
            u32 k = (u32)(noiseRand() * (float)COUNT) % COUNT;
            if (pixIdx[k] >= 0) {
                continue;
            }
            px[count] = ((float)(k % W) + 0.5f) / (float)W;
            py[count] = ((float)(k / W) + 0.5f) / (float)W;
            pixIdx[k] = (int)count;
            count++;
        }
        memset(out, 0, (size_t)COUNT * 2);
        for (u32 k = 0; k < COUNT; k++) {
            u32 pix = (u32)(py[k] * (float)W) * W + (u32)(px[k] * (float)W);
            out[(size_t)pix * 2 + 0] = (u8)(k & 0xFFu);
            out[(size_t)pix * 2 + 1] = (u8)(k >> 8);
        }
    }

    /* 6.2: one-shot compute bake of the env cube (face z = cube layer; the
     * shader maps each texel to its standard cube face direction). */
    static void renderEnvCube(void) {
        if (!envCubePipeReady || !envCube.img) {
            return;
        }
        u32 face = envCube.extent.width;
        VulkanCommand* cmd = vulkanTransientBegin();
        vulkanTransition(cmd, &envCube, VK_IMAGE_LAYOUT_GENERAL, 0, envCube.layers);
        vulkanBindPipe(cmd, &envCubePipe);
        u32 pc[2] = {(u32)envCube.storagePoolIndex, face};
        vulkanPush(cmd, &envCubePipe, sizeof(pc), pc);
        vulkanDispatch(cmd, &envCubePipe, (int)(face / 8), (int)(face / 8), 6);
        vulkanTransition(cmd, &envCube, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, envCube.layers);
        vulkanTransientEnd(cmd, 1);
        if (!giInputsLogged) {
            giInputsLogged = 1;
            utils::info("vulkanBrixelizerPass: env cube baked: %ux%ux6 RGBA16F (storage pool index %d)",
                        face,
                        face,
                        envCube.storagePoolIndex);
        }
    }

    /* 6.3: end-of-frame history copies. postUpdate() runs after every pass's
     * update() (the vulkanPostUpdate loop), so compositeColor already holds
     * this frame's composited color. Step 8 switches the copy point to
     * after GI compositing. */
    static void giHistoryCopy(void) {
        if (vulkan.skipFrame || !contextReady || !historyDepth.img) {
            return;
        }
        VulkanCommand* cmd   = vulkan.currentCmd;
        VulkanImage* depth   = vulkanFrameResourcesGetDepth();
        VulkanImage* wnorm   = vulkanFrameResourcesGetWorldNormal();
        VulkanImage* lit     = vulkanFrameResourcesGetCompositeColor();
        if (!cmd || !depth || !wnorm || !lit) {
            return;
        }
        if (depth->extent.width != historyDepth.extent.width ||
            depth->extent.height != historyDepth.extent.height) {
            return; /* dynamic-resolution mismatch (upscaler) — skip the frame */
        }
        /* History depth clears/copies to 0.0 = background (reverse-Z — plan
         * pitfall #3); the layout ends shader-readable for the Step-7 GI
         * context's re-wrap. */
        vulkanCopyDepthToColorImage(cmd, depth, &historyDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vulkanCopyColorImage(cmd, wnorm, &historyNormal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vulkanCopyColorImage(cmd, lit, &historyLit, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    /* ── Step 7: GI context + dispatch (plans/brixelizer-gi.md) ────────────── */

    /* Debug cache visualization mode. OFF by default (see the giDebugEnabled
     * comment — a second per-frame FFX dispatch halves the view lifetime).
     * ENGINE_BRIXGI_DEBUG=radiance|irradiance enables it; "off" disables. When
     * enabled it runs as a ONE-SHOT on frame giDebugFrame (ENGINE_BRIXGI_DEBUG_FRAME,
     * default 120, counted from GI-context creation) — a single extra FFX
     * dispatch, after which the main dispatch resumes 1-per-frame (safe). */
    static FfxBrixelizerGIDebugMode getGIDebugMode(void) {
        if (!giDebugModeSet) {
            giDebugModeSet = 1;
            const char* env = getenv("ENGINE_BRIXGI_DEBUG");
            if (env && !strcmp(env, "irradiance")) {
                giDebugMode    = FFX_BRIXELIZER_GI_DEBUG_MODE_IRRADIANCE_CACHE;
                giDebugEnabled = 1;
            } else if (env && !strcmp(env, "radiance")) {
                giDebugMode    = FFX_BRIXELIZER_GI_DEBUG_MODE_RADIANCE_CACHE;
                giDebugEnabled = 1;
            } else if (env && !strcmp(env, "off")) {
                giDebugEnabled = 0;
            }
            const char* frameEnv = getenv("ENGINE_BRIXGI_DEBUG_FRAME");
            if (frameEnv && *frameEnv) giDebugFrame = (u32)atoi(frameEnv);
            /* No env (or unrecognized) → stays disabled (giDebugEnabled = 0). */
        }
        return giDebugMode;
    }

    /* (Re)creates the three GI output images (outputDiffuseGI / outputSpecularGI
     * / the debug cache target) at render resolution. The GI context's final
     * upsample pass writes the two GI outputs as UAVs every frame; the debug
     * target is written by ffxBrixelizerGIContextDebugVisualization. All three
     * need STORAGE (UAV write) + SAMPLED (Step 8 composite / dumps) +
     * TRANSFER_SRC (vulkanSaveImage dumps). Cleared to 0 so a pre-GI dump is
     * predictable; the wait guarantees the clear lands before the first
     * dispatch on the frame command buffer. */
    static void giCreateOutputs(u32 renderW, u32 renderH) {
        if (giDiffuse.img) {
            vulkanDestroyImage(&giDiffuse, NULL);
            giDiffuse = VulkanImage{};
        }
        if (giSpecular.img) {
            vulkanDestroyImage(&giSpecular, NULL);
            giSpecular = VulkanImage{};
        }
        if (giDebug.img) {
            vulkanDestroyImage(&giDebug, NULL);
            giDebug = VulkanImage{};
        }
        static const VkImageUsageFlags GI_OUT_USAGE =
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT; /* TRANSFER_DST: vulkanClearColorImage */
        giDiffuse = vulkanCreateImage(.name   = "BrixelGiDiffuse",
                                      .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                      .usage  = GI_OUT_USAGE,
                                      .width  = (int)renderW,
                                      .height = (int)renderH);
        giSpecular = vulkanCreateImage(.name   = "BrixelGiSpecular",
                                       .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                       .usage  = GI_OUT_USAGE,
                                       .width  = (int)renderW,
                                       .height = (int)renderH);
        giDebug = vulkanCreateImage(.name   = "BrixelGiDebug",
                                    .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                    .usage  = GI_OUT_USAGE,
                                    .width  = (int)renderW,
                                    .height = (int)renderH);
        if (!giDiffuse.img || !giSpecular.img || !giDebug.img) {
            utils::error("vulkanBrixelizerPass: GI output resource creation failed");
            return;
        }
        VulkanCommand* cmd = vulkanTransientBegin();
        VkClearColorValue black = {};
        vulkanTransition(cmd, &giDiffuse, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        vulkanClearColorImage(cmd, &giDiffuse, black);
        vulkanTransition(cmd, &giDiffuse, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, &giSpecular, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        vulkanClearColorImage(cmd, &giSpecular, black);
        vulkanTransition(cmd, &giSpecular, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, &giDebug, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        vulkanClearColorImage(cmd, &giDebug, black);
        vulkanTransition(cmd, &giDebug, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransientEnd(cmd, 1);
    }

    /* Creates (or, on a resolution change, recreates) the GI context + outputs.
     * displaySize is fixed at creation, so a render-size mismatch destroys and
     * rebuilds both (FSR-pass pattern). The GI context holds a raw pointer to
     * the voxelizer context, so the voxelizer must exist first. */
    static char giEnsureContext(u32 renderW, u32 renderH) {
        if (!contextReady || renderW == 0 || renderH == 0) {
            return 0;
        }
        if (giContextReady && giDiffuse.img && (u32)giDiffuse.extent.width == renderW &&
            (u32)giDiffuse.extent.height == renderH) {
            return 1;
        }
        giDestroyContext();
        giCreateOutputs(renderW, renderH);
        if (!giDiffuse.img) {
            return 0;
        }

        FfxBrixelizerGIContextDescription desc = {};
        /* The engine is reverse-Z (1 = near, 0 = far/background) — plan
         * pitfall #3: the flag flips ffxIsBackground / BackgroundDepth / the
         * closer-op in the GI shaders to the reverse-Z convention. */
        desc.flags              = FFX_BRIXELIZER_GI_FLAG_DEPTH_INVERTED;
        desc.internalResolution = FFX_BRIXELIZER_GI_INTERNAL_RESOLUTION_50_PERCENT;
        desc.displaySize.width  = renderW;
        desc.displaySize.height = renderH;
        desc.backendInterface   = backendInterface; /* shared with the voxelizer */

        FfxErrorCode result = ffxBrixelizerGIContextCreate(&giContext, &desc);
        if (result != FFX_OK) {
            if ((u32)result == FFX_ERROR_OUT_OF_MEMORY || (u32)result == FFX_ERROR_INSUFFICIENT_MEMORY) {
                utils::terminate(
                    "vulkanBrixelizerPass: not enough GPU memory to create the GI context. "
                    "Free VRAM by closing other GPU applications and try again.");
            }
            utils::error("vulkanBrixelizerPass: ffxBrixelizerGIContextCreate failed: %d", result);
            giContext = FfxBrixelizerGIContext{};
            return 0;
        }
        giContextReady = 1;
        giHavePrev     = 0; /* temporal history restarts with the context */
        giFrameCount   = 0; /* one-shot debug-viz frame counter restarts too */
        giDebugDone    = 0;
        if (!giContextLogged) {
            giContextLogged = 1;
            utils::info("vulkanBrixelizerPass: created GI context (%ux%u display, 50%% internal, reverse-Z)",
                        renderW,
                        renderH);
        }
        return 1;
    }

    /* Per-frame GI dispatch (plan Step 7.2). Runs on the frame command buffer
     * AFTER the voxelizer update (which wrote this frame's SDF). All wrapped
     * images are staged by hand — the FFX backend does not manage engine
     * layouts (plan pitfall #11): inputs → SHADER_READ_ONLY, the two GI
     * outputs → GENERAL (UAV write), then back to SHADER_READ_ONLY for the
     * Step-8 composite / dumps. */
    static void giDispatch(VulkanCommand* cmd, Camera* camera) {
        resolveGiEnabled();
        if (!giEnabled) {
            return; /* GI off (ENGINE_BRIXGI=0) — composite skips the term */
        }
        if (!giContextReady || !giDiffuse.img) {
            return;
        }
        VulkanImage* depth    = vulkanFrameResourcesGetDepth();
        VulkanImage* wnorm    = vulkanFrameResourcesGetWorldNormal();
        VulkanImage* material = vulkanFrameResourcesGetMaterial();
        VulkanImage* velocity = vulkanFrameResourcesGetVelocity();
        if (!depth || !wnorm || !material || !velocity) {
            return;
        }
        u32 renderW = (u32)depth->extent.width;
        u32 renderH = (u32)depth->extent.height;
        if (renderW != (u32)giDiffuse.extent.width || renderH != (u32)giDiffuse.extent.height) {
            return; /* size change in flight — giEnsureContext rebuilds next frame */
        }

        vulkanTransition(cmd, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, wnorm, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, material, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, velocity, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, &historyDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, &historyNormal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, &historyLit, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, &blueNoise, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, &envCube, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, envCube.layers);
        vulkanTransition(cmd, &sdfAtlas, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, &giDiffuse, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        vulkanTransition(cmd, &giSpecular, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

        FfxBrixelizerGIDispatchDescription desc = {};
        /* view / projection are the JITTERED matrices (matching the depth
         * buffer). The engine's cglm mat4 is COLUMN-major — memcpy verbatim
         * (plan pitfall #1; the GI host inverts them with a column-major GLU
         * formula and the GLSL UBO is column-major). Do NOT transpose. */
        memcpy(desc.view, camera->cameraUbo.view, sizeof(desc.view));
        memcpy(desc.projection, camera->cameraUbo.projection, sizeof(desc.projection));
        /* prevView / prevProjection: the engine only keeps the prev view*proj
         * product, so the pass holds its own previous-frame copies of the same
         * jittered matrices. On the first frame they equal the current ones
         * (no motion). */
        if (giHavePrev) {
            memcpy(desc.prevView, giPrevView, sizeof(desc.prevView));
            memcpy(desc.prevProjection, giPrevProjection, sizeof(desc.prevProjection));
        } else {
            memcpy(desc.prevView, camera->cameraUbo.view, sizeof(desc.prevView));
            memcpy(desc.prevProjection, camera->cameraUbo.projection, sizeof(desc.prevProjection));
        }
        memcpy(&giPrevView, &camera->cameraUbo.view, sizeof(mat4));
        memcpy(&giPrevProjection, &camera->cameraUbo.projection, sizeof(mat4));
        giHavePrev = 1;

        desc.cameraPosition[0] = camera->cameraUbo.renderLocation[0];
        desc.cameraPosition[1] = camera->cameraUbo.renderLocation[1];
        desc.cameraPosition[2] = camera->cameraUbo.renderLocation[2];
        /* Static-only cascade layout: the SDF trace loop is
         * `for (c = start; c <= end; c++)` — INCLUSIVE, so [0, 7] covers all
         * 8 static cascades (matches the sample's 8-level merged range). */
        desc.startCascade = 0;
        desc.endCascade   = BRIX_NUM_CASCADES - 1;
        desc.rayPushoff           = 0.25f;
        desc.sdfSolveEps          = 0.5f;
        desc.specularRayPushoff   = 0.25f;
        desc.specularSDFSolveEps  = 0.5f;
        desc.tMin                 = 0.0f;
        desc.tMax                 = 10000.0f;

        desc.normalsUnpackMul      = 1.0f; /* worldNormal is world-space (pitfall #4) */
        desc.normalsUnpackAdd      = 0.0f;
        desc.isRoughnessPerceptual = 1;    /* material .r is perceptual (pitfall #6) */
        desc.roughnessChannel      = 0;
        desc.roughnessThreshold    = 0.9f;
        desc.environmentMapIntensity = 0.1f;
        /* Engine velocity is (current - previous) in pixels (y-flipped) →
         * history_uv = uv + mv * scale needs scale = (-1/W, -1/H) (pitfall #5).
         * ENGINE_BRIX_GI_MV_SCALE multiplies the scale (default 1.0) so a wrong
         * scale can be forced to test the temporal reprojection (a wrong scale
         * breaks the temporal filter and shows up as GI noise / smearing when
         * the camera moves — see the Step 7 mv-scale gate). */
        static float   giMvScaleOverride = 1.0f;
        static char    giMvScaleInit     = 0;
        if (!giMvScaleInit) {
            giMvScaleInit = 1;
            const char* s = getenv("ENGINE_BRIX_GI_MV_SCALE");
            if (s && *s) giMvScaleOverride = (float)atof(s);
        }
        desc.motionVectorScale.x = (-1.0f / (float)renderW) * giMvScaleOverride;
        desc.motionVectorScale.y = (-1.0f / (float)renderH) * giMvScaleOverride;

        /* Current-frame G-buffer inputs (sampled). */
        desc.depth         = vulkanFfxWrapImageResource(depth, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"gi_depth");
        desc.normal        = vulkanFfxWrapImageResource(wnorm, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"gi_normal");
        desc.roughness     = vulkanFfxWrapImageResource(material, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"gi_roughness");
        desc.motionVectors = vulkanFfxWrapImageResource(velocity, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"gi_motion");
        /* Previous-frame (history) inputs — Step 6.3 copies at end of frame. */
        desc.historyDepth  = vulkanFfxWrapImageResource(&historyDepth, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"gi_hist_depth");
        desc.historyNormal = vulkanFfxWrapImageResource(&historyNormal, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"gi_hist_normal");
        desc.prevLitOutput = vulkanFfxWrapImageResource(&historyLit, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"gi_prev_lit");
        /* Static inputs. */
        desc.noiseTexture   = vulkanFfxWrapImageResource(&blueNoise, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"gi_noise");
        desc.environmentMap = vulkanFfxWrapImageResource(&envCube, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"gi_env");

        /* SDF resources — the voxelizer's, passed COMPUTE_READ (the GI
         * ray-march samples the atlas + walks the cascade trees). */
        desc.sdfAtlas    = vulkanFfxWrapImageResource(&sdfAtlas, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"gi_sdf_atlas");
        desc.bricksAABBs = vulkanFfxWrapBufferResource(&brickAABBs, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, FFX_RESOURCE_STATE_COMPUTE_READ, L"gi_brick_aabbs");
        wchar_t cascadeName[64];
        for (u32 i = 0; i < FFX_BRIXELIZER_MAX_CASCADES; i++) {
            swprintf(cascadeName, 64, L"gi_cascade%uAabbTree", i);
            desc.cascadeAABBTrees[i] =
                vulkanFfxWrapBufferResource(&cascadeAABBTrees[i], VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, FFX_RESOURCE_STATE_COMPUTE_READ, cascadeName);
            swprintf(cascadeName, 64, L"gi_cascade%uBrickMap", i);
            desc.cascadeBrickMaps[i] =
                vulkanFfxWrapBufferResource(&cascadeBrickMaps[i], VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, FFX_RESOURCE_STATE_COMPUTE_READ, cascadeName);
        }

        /* Outputs — written by the GI context's final upsample pass (UAV). */
        desc.outputDiffuseGI  = vulkanFfxWrapImageResource(&giDiffuse, FFX_RESOURCE_USAGE_UAV, FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"gi_out_diffuse");
        desc.outputSpecularGI = vulkanFfxWrapImageResource(&giSpecular, FFX_RESOURCE_USAGE_UAV, FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"gi_out_specular");

        /* The GI context traces the voxelizer's SDF — pass its raw context
         * (stable for the lifetime of the voxelizer context). */
        FfxErrorCode rawResult = ffxBrixelizerGetRawContext(&brixelizerContext, &desc.brixelizerContext);
        if (rawResult != FFX_OK) {
            utils::error("vulkanBrixelizerPass: ffxBrixelizerGetRawContext failed: %d", rawResult);
            vulkanTransition(cmd, &giDiffuse, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
            vulkanTransition(cmd, &giSpecular, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
            return;
        }

        if (giProfileReady) {
            vulkanBeginProfile(cmd, &giProfile, 1);
        }
        FfxErrorCode result = ffxBrixelizerGIContextDispatch(&giContext, &desc, ffxGetCommandListVK(cmd->cmd));
        if (giProfileReady) {
            vulkanEndProfile(cmd, &giProfile, 1);
        }
        if (result != FFX_OK) {
            utils::error("vulkanBrixelizerPass: ffxBrixelizerGIContextDispatch failed: %d", result);
        }

        /* Leave the outputs shader-readable (Step 8 composites them; dumps
         * sample them). The SDF atlas stays SHADER_READ_ONLY from the
         * voxelizer transition above. */
        vulkanTransition(cmd, &giDiffuse, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, &giSpecular, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

        if (frameIndex % 120 == 0 && giProfileReady) {
            utils::info("vulkanBrixelizerPass: GI dispatch gpu=%.3f ms (19 passes, 50%% internal)",
                        giProfile.elapsed / MILLION);
        }
    }

    /* Step 7.3: GI cache debug visualization — ray-march / sample the internal
     * radiance (debug_type 0) or irradiance (debug_type 1) cache into giDebug
     * at render resolution. Runs after the main dispatch (it reads the cache
     * the dispatch just updated). */
    static void giDebugDispatch(VulkanCommand* cmd, Camera* camera) {
        if (!giContextReady || !giDebug.img || !giDebugEnabled) {
            return;
        }
        /* ONE-SHOT: run the extra FFX dispatch on exactly one frame. A per-frame
         * second dispatch would keep the FFX per-effect-context frame index at
         * 2x the engine frame rate, halving the dynamic-view lifetime below the
         * in-flight window and tripping a fatal view-in-use validation error.
         * A single extra dispatch advances the index once; the main dispatch
         * resumes 1-per-frame and the one-shot frame's views are destroyed 4
         * FFX frames (2 engine frames) later — well after its command buffer
         * has completed. The written giDebug image persists for later dumps. */
        if (giDebugDone || giFrameCount != giDebugFrame) {
            return;
        }
        VulkanImage* depth = vulkanFrameResourcesGetDepth();
        VulkanImage* wnorm = vulkanFrameResourcesGetWorldNormal();
        if (!depth || !wnorm) {
            return;
        }
        if ((u32)depth->extent.width != (u32)giDebug.extent.width ||
            (u32)depth->extent.height != (u32)giDebug.extent.height) {
            return;
        }
        vulkanTransition(cmd, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, wnorm, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, &sdfAtlas, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, &giDebug, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

        FfxBrixelizerGIDebugDescription desc = {};
        memcpy(desc.view, camera->cameraUbo.view, sizeof(desc.view));
        memcpy(desc.projection, camera->cameraUbo.projection, sizeof(desc.projection));
        desc.startCascade = 0;
        desc.endCascade   = BRIX_NUM_CASCADES - 1;
        desc.outputSize[0] = (u32)giDebug.extent.width;
        desc.outputSize[1] = (u32)giDebug.extent.height;
        desc.debugMode         = getGIDebugMode();
        desc.normalsUnpackMul  = 1.0f;
        desc.normalsUnpackAdd  = 0.0f;
        desc.depth             = vulkanFfxWrapImageResource(depth, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"gi_dbg_depth");
        desc.normal            = vulkanFfxWrapImageResource(wnorm, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"gi_dbg_normal");
        desc.sdfAtlas          = vulkanFfxWrapImageResource(&sdfAtlas, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"gi_dbg_sdf_atlas");
        desc.bricksAABBs       = vulkanFfxWrapBufferResource(&brickAABBs, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, FFX_RESOURCE_STATE_COMPUTE_READ, L"gi_dbg_brick_aabbs");
        wchar_t cascadeName[64];
        for (u32 i = 0; i < FFX_BRIXELIZER_MAX_CASCADES; i++) {
            swprintf(cascadeName, 64, L"gi_dbg_cascade%uAabbTree", i);
            desc.cascadeAABBTrees[i] =
                vulkanFfxWrapBufferResource(&cascadeAABBTrees[i], VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, FFX_RESOURCE_STATE_COMPUTE_READ, cascadeName);
            swprintf(cascadeName, 64, L"gi_dbg_cascade%uBrickMap", i);
            desc.cascadeBrickMaps[i] =
                vulkanFfxWrapBufferResource(&cascadeBrickMaps[i], VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, FFX_RESOURCE_STATE_COMPUTE_READ, cascadeName);
        }
        desc.outputDebug = vulkanFfxWrapImageResource(&giDebug, FFX_RESOURCE_USAGE_UAV, FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"gi_dbg_out");

        FfxErrorCode rawResult = ffxBrixelizerGetRawContext(&brixelizerContext, &desc.brixelizerContext);
        if (rawResult != FFX_OK) {
            utils::error("vulkanBrixelizerPass: ffxBrixelizerGetRawContext (gi debug) failed: %d", rawResult);
            vulkanTransition(cmd, &giDebug, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
            return;
        }
        FfxErrorCode result = ffxBrixelizerGIContextDebugVisualization(&giContext, &desc, ffxGetCommandListVK(cmd->cmd));
        if (result != FFX_OK) {
            utils::error("vulkanBrixelizerPass: ffxBrixelizerGIContextDebugVisualization failed: %d", result);
        }
        vulkanTransition(cmd, &giDebug, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        giDebugDone = 1;
        utils::info(
            "vulkanBrixelizerPass: GI cache debug viz (one-shot, %s cache) written at giFrame %u",
            getGIDebugMode() == FFX_BRIXELIZER_GI_DEBUG_MODE_IRRADIANCE_CACHE ? "irradiance" : "radiance",
            giFrameCount);
    }

    struct VulkanImage* vulkanBrixelizerPassGetBlueNoise(void) {
        return blueNoise.img ? &blueNoise : NULL;
    }

    struct VulkanImage* vulkanBrixelizerPassGetGiDiffuse(void) {
        return giDiffuse.img ? &giDiffuse : NULL;
    }

    struct VulkanImage* vulkanBrixelizerPassGetGiSpecular(void) {
        return giSpecular.img ? &giSpecular : NULL;
    }

    char vulkanBrixelizerPassIsGiEnabled(void) {
        resolveGiEnabled();
        return giEnabled && giContextReady;
    }

    struct VulkanImage* vulkanBrixelizerPassGetGiDebug(void) {
        return giDebug.img ? &giDebug : NULL;
    }

    struct VulkanImage* vulkanBrixelizerPassGetHistoryDepth(void) {
        return historyDepth.img ? &historyDepth : NULL;
    }

    struct VulkanImage* vulkanBrixelizerPassGetHistoryNormal(void) {
        return historyNormal.img ? &historyNormal : NULL;
    }

    struct VulkanImage* vulkanBrixelizerPassGetHistoryLit(void) {
        return historyLit.img ? &historyLit : NULL;
    }

    struct VulkanImage* vulkanBrixelizerPassGetEnvFace(u32 face) {
        if (!envCube.img || face > 5u) {
            return NULL;
        }
        if (!envFaceDump.img) {
            envFaceDump = vulkanCreateImage(.name   = "BrixelEnvFaceDump",
                                            .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                            .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                            .width  = (int)envCube.extent.width,
                                            .height = (int)envCube.extent.height);
        }
        if (!envFaceDump.img) {
            return NULL;
        }
        VulkanCommand* cmd = vulkanTransientBegin();
        vulkanTransition(cmd, &envCube, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 6);
        vulkanTransition(cmd, &envFaceDump, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);
        VkImageCopy region = {};
        region.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.baseArrayLayer = face;
        region.srcSubresource.layerCount     = 1;
        region.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        region.dstSubresource.baseArrayLayer = 0;
        region.dstSubresource.layerCount     = 1;
        region.extent                        = {envCube.extent.width, envCube.extent.height, 1};
        vkCmdCopyImage(cmd->cmd,
                       envCube.img,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       envFaceDump.img,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1,
                       &region);
        vulkanTransition(cmd, &envFaceDump, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, &envCube, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 6);
        vulkanTransientEnd(cmd, 1);
        return &envFaceDump;
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

    /* ── Step 5: props SDF public API ─────────────────────────────────────────
     * All of these are thread-safe (worker-thread callers from the azgaar
     * scatter / cull paths): state is queued under propsLock and applied on
     * the render thread in update() (propsSyncPending), once the voxelizer
     * context exists. */

    void vulkanBrixelizerPassSetPropsMeshes(const void* verts, u32 vertCount,
                                            const void* idx, u32 idxCount,
                                            const PropVariantRange* variants, u32 variantCount) {
        utils::threadLock(&propsLock);
        BrixelPropsPending p = {};
        if (verts && vertCount && idx && idxCount && variants && variantCount) {
            p.kind = BRIX_PROPS_MESH_SET;
            /* Extract the per-variant position-only sub-buffers NOW (the merged
             * arrays are caller-owned and transient); the GPU upload + FFX
             * registration happens in the render-thread drain. */
            propsExtractMeshes(verts, vertCount, idx, idxCount, variants, variantCount);
        } else {
            p.kind = BRIX_PROPS_MESH_CLEAR;
            propsVariantCpu.clear();
            propsVariantTable.clear();
        }
        propsPending.push_back(p);
        utils::threadUnlock(&propsLock);
    }

    void vulkanBrixelizerPassPropsTileSet(i32 tileX, i32 tileZ, u64 readyStamp,
                                          const PropInstance* instances, u32 instanceCount) {
        if (!instances || instanceCount == 0) {
            return;
        }
        utils::threadLock(&propsLock);
        BrixelPropsPending p = {};
        p.kind       = BRIX_PROPS_TILE_SET;
        p.tileX      = tileX;
        p.tileZ      = tileZ;
        p.readyStamp = readyStamp;
        p.instances.assign(instances, instances + instanceCount);
        propsPending.push_back(p);
        utils::threadUnlock(&propsLock);
    }

    void vulkanBrixelizerPassPropsTileClear(i32 tileX, i32 tileZ) {
        utils::threadLock(&propsLock);
        BrixelPropsPending p = {};
        p.kind  = BRIX_PROPS_TILE_CLEAR;
        p.tileX = tileX;
        p.tileZ = tileZ;
        propsPending.push_back(p);
        utils::threadUnlock(&propsLock);
    }

    void vulkanBrixelizerPassPropsGlobalSet(const PropInstance* instances, u32 instanceCount) {
        if (!instances || instanceCount == 0) {
            return;
        }
        utils::threadLock(&propsLock);
        BrixelPropsPending p = {};
        p.kind      = BRIX_PROPS_GLOBAL_SET;
        p.instances.assign(instances, instances + instanceCount);
        propsPending.push_back(p);
        utils::threadUnlock(&propsLock);
    }

    void vulkanBrixelizerPassPropsGlobalClear(void) {
        utils::threadLock(&propsLock);
        BrixelPropsPending p = {};
        p.kind = BRIX_PROPS_GLOBAL_CLEAR;
        propsPending.push_back(p);
        utils::threadUnlock(&propsLock);
    }

    void vulkanBrixelizerPassPropsLandmarksSet(const PropInstance* instances, u32 instanceCount) {
        if (!instances || instanceCount == 0) {
            return;
        }
        utils::threadLock(&propsLock);
        BrixelPropsPending p = {};
        p.kind      = BRIX_PROPS_LANDMARKS_SET;
        p.instances.assign(instances, instances + instanceCount);
        propsPending.push_back(p);
        utils::threadUnlock(&propsLock);
    }

    void vulkanBrixelizerPassPropsLandmarksClear(void) {
        utils::threadLock(&propsLock);
        BrixelPropsPending p = {};
        p.kind = BRIX_PROPS_LANDMARKS_CLEAR;
        propsPending.push_back(p);
        utils::threadUnlock(&propsLock);
    }
}  // namespace engine