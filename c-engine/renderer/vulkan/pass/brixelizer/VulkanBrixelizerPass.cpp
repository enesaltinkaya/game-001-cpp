#include "VulkanBrixelizerPass.h"
#include "renderer/vulkan/utils/VulkanFfxUtils.h"
#include "renderer/vulkan/utils/VulkanUtils.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanIbl.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/pass/azgaar_props/VulkanAzgaarPropsPass.h"
#include "ecs/Ecs.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/window/WindowSystem.h"
#include "events/Events.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanProfile.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#include <FidelityFX/gpu/brixelizer/ffx_brixelizer_host_gpu_shared.h>
#pragma GCC diagnostic pop
#include <math.h>
#include <string.h>
#include <wchar.h>
#include <stdlib.h>

namespace engine {
static void swapchainCreated(void* _);
static void onPropsMesh(const PropsMeshInfo* mesh, void* user);
static void onPropsTile(i32 tileX, i32 tileZ, u64 readyStamp,
                        const PropInstance* instances, u32 instanceCount,
                        const PropTileRange* ranges, u32 rangeCount,
                        char removed, void* user);
static void destroyContext(char keepGI, char keepSdf);
static void destroyResources(char keepSdf);
static char ensureBackend(void);
static char ensureContext(const float center[3]);
static char registerMeshBuffers(char meshDirty);
static u32 maxCascadeForExtent(float extent);
// GI stage (task 3)
static void destroyGI(void);
static char ensureGI(u32 width, u32 height);
static char createNoiseTexture(void);
static void mat4ToRowMajor(const mat4 m, float out[16]);
static void dispatchGI(VulkanCommand* cmd, Camera* camera);

// ── Configuration ──────────────────────────────────────────────────────────
// 8 STATIC-only cascades: the raw cascade index equals the level index (the
// layout the GI sample's BRIXGI_STATIC_ONLY A/B matched, and the one the
// GI dispatch's start/end cascade range will point at).
#define BRIX_NUM_CASCADES          8
#define BRIX_BASE_VOXEL_SIZE       2.0f
#define BRIX_CENTER_SNAP           64.0f  // clipmap-center snap (m); keeps cascade 0 (64 m radius) covering the camera
#define BRIX_MAX_REFERENCES        (32 * (1 << 20))
#define BRIX_TRIANGLE_SWAP_SIZE    (300 * (1 << 20))
#define BRIX_MAX_BRICKS_PER_BAKE   (1 << 14)
#define BRIX_STATS_LOG_STRIDE      30

// ── GI stage configuration (task 3) ────────────────────────────────────────
// The GI traces the same 8 STATIC cascades the voxelizer built (raw cascade
// index == level, so the trace range is 0..BRIX_NUM_CASCADES-1 — no dynamic/
// merged offset, unlike the stock sample).  The pushoff/eps/tMax/ray-pushoff
// scalars mirror the FFX reference sample's defaults (world-space metres).
#define BRIX_GI_START_CASCADE      0
#define BRIX_GI_END_CASCADE        (BRIX_NUM_CASCADES - 1)
#define BRIX_GI_RAY_PUSHOFF        0.25f   // metres along the normal (diffuse + specular origin)
#define BRIX_GI_SDF_EPS            0.5f    // ray-march epsilon
#define BRIX_GI_TMIN               0.0f
#define BRIX_GI_TMAX               10000.0f
#define BRIX_GI_ENV_INTENSITY      0.1f    // environment (sky) radiance scale
#define BRIX_GI_ROUGH_THRESHOLD    0.9f
#define BRIX_GI_NOISE_SIZE         128
#define BRIX_GI_SAVE_EVERY_DEFAULT 30

// ── FFX backend ────────────────────────────────────────────────────────────
static void* scratchBuffer;
static size_t scratchBufferSize;
static FfxInterface backendInterface;
static char backendReady;

// ── Voxelizer context + FFX-owned GPU resources ───────────────────────────
static FfxBrixelizerContext context;
static char contextReady;
static FfxBrixelizerBakedUpdateDescription bakedUpdateDesc;

static VulkanImage sdfAtlas;
static VulkanBuffer brickAABBs;
static VulkanBuffer cascadeAabbTrees[FFX_BRIXELIZER_MAX_CASCADES];
static VulkanBuffer cascadeBrickMaps[FFX_BRIXELIZER_MAX_CASCADES];
static VulkanBuffer gpuScratch;   // per-update scratch (size from the first bake)
static u64 gpuScratchSize;

static u32 frameIndex;             // monotonic update counter (feeds the cascade rotation)
static float sdfCenter[3];

// ── Registered props mesh buffers (per-context; re-registered on rebuild) ─
static u32 meshVboIdx = 0xFFFFFFFF;
static u32 meshIboIdx = 0xFFFFFFFF;
static char meshBuffersRegistered;
static char meshDirty;              // mesh callback saw a SetMeshes/SetVariants
static u32 meshVertCount;
static std::vector<PropVariantRange> variants;

// ── Static instance registry (per props tile) ─────────────────────────────
static std::vector<FfxBrixelizerInstanceID> instanceIDs;  // flat pool
typedef struct TileReg {
    i32  tileX, tileZ;
    u64  stamp;
    u32  offset;   // into instanceIDs
    u32  count;
} TileReg;
static std::vector<TileReg> tileRegs;
static char instanceCapWarned;

// ── Game-thread → render-thread event queue ───────────────────────────────
typedef struct PendingEvent {
    char removed;
    i32  tileX, tileZ;
    u64  stamp;
    std::vector<PropInstance>    instances;
    std::vector<PropTileRange>   ranges;
} PendingEvent;
static std::vector<PendingEvent> pendingEvents;
static utils::Thread pendingLock = {.mutex = PTHREAD_MUTEX_INITIALIZER};
static char pendingContextDestroy;  // mesh buffers died under the FFX context

// ── GI context + FFX-owned GPU resources (task 3) ───────────────────────────
static FfxBrixelizerGIContext giContext;
static char giContextReady;
static u32 giWidth, giHeight;   // GI display (= render) resolution
static float giPrevViewRow[16];      // previous frame view (row-major), for reprojection
static float giPrevProjectionRow[16]; // previous frame projection (row-major)
static char giHasPrevMatrices;       // false until the first dispatch has stored them

// The GI reads the depth the same way the CACAO AO pass does: the engine's D32
// depth attachment is wrapped directly (the FFX VK backend samples it as a
// float texture2D, which RADV accepts — same convention the AO pass relies on).
// The history depth is a matching D32 image so it can be filled by a same-aspect
// depth-to-depth copy (a D32->R32 colour copy is a validation error without
// maintenance8).
static VulkanImage giHistoryDepth;      // D32_SFLOAT, previous-frame depth
static VulkanImage giHistoryNormal;     // R16G16B16A16_SFLOAT, previous-frame world normal
static VulkanImage giPrevLitOutput;     // R16G16B16A16_SFLOAT, previous-frame composite
static VulkanImage giDiffuse;           // R16G16B16A16_SFLOAT, GI output
static VulkanImage giSpecular;          // R16G16B16A16_SFLOAT, GI output
static VulkanImage giDebugVisualization; // R16G16B16A16_SFLOAT, FFX radiance/irradiance cache
static VulkanImage giDebugTarget;        // R16G16B16A16_SFLOAT, probe size, per-pixel debug (red = weight_sum<1e-3 history reset)
static VulkanImage giDisocclusionMask;   // R8_UNORM, internal size, per-pixel temporal-rejection mask
static VulkanImage giNoiseTexture;      // R8G8_UNORM, 128^2 two-channel blue noise
static char giNoiseCreated;

// Per-frame debug/save knobs (read once from the environment).
static char giDebugMode;  // 0=off, 1=radiance cache, 2=irradiance cache (ENGINE_BRIX_GI_DEBUG)
static u32 giSaveEvery;   // frames between GI debug saves (ENGINE_BRIX_GI_SAVE_EVERY)
static char giSaveEnabled;
static char giMaskSaveEnabled; // raw per-frame dumps of the debug target + disocclusion mask (ENGINE_BRIX_GI_MASK_SAVE)
static char giLogInit;
static u32 giFrameIndex;   // monotonic GI dispatch counter (drives the save cadence)

static VulkanProfile profile;
static char profileReady;

VulkanBrixelizerPass vulkanBrixelizerPass;

VulkanBrixelizerPass::VulkanBrixelizerPass() : System("brixelizer") {}

void VulkanBrixelizerPass::added() {
    utils::signalSubscribe("swapchainCreated", swapchainCreated);
    profile    = vulkanCreateProfile("brixelizer");
    profileReady = 1;
    // The props pass fires these on the game/pool threads; the handlers only
    // copy data into pendingEvents (own lock) — no GPU work off the render thread.
    vulkanAzgaarPropsSetMeshCallback(onPropsMesh, NULL);
    vulkanAzgaarPropsSetTileCallback(onPropsTile, NULL);
}

void VulkanBrixelizerPass::removed() {
    vulkanAzgaarPropsSetMeshCallback(NULL, NULL);
    vulkanAzgaarPropsSetTileCallback(NULL, NULL);
    destroyContext(0, 0);
    if (scratchBuffer) {
        free(scratchBuffer);
        scratchBuffer     = NULL;
        scratchBufferSize = 0;
    }
    backendInterface = FfxInterface{};
    backendReady     = 0;
    if (profileReady) {
        vulkanDestroyProfile(&profile);
        profile      = VulkanProfile{};
        profileReady = 0;
    }
    utils::threadLock(&pendingLock);
    pendingEvents.clear();
    utils::threadUnlock(&pendingLock);
}

void VulkanBrixelizerPass::preUpdate() {
    if (profileReady) {
        vulkanResetProfile(vulkan.currentCmd, &profile, 0);
    }
}

// ── Props change hooks (game / pool threads) ─────────────────────────────

static void onPropsMesh(const PropsMeshInfo* mesh, void* _) {
    (void)_;
    utils::threadLock(&pendingLock);
    meshVertCount = mesh->vertCount;
    if (mesh->variants && mesh->variantCount > 0) {
        variants.assign(mesh->variants, mesh->variants + mesh->variantCount);
    } else {
        variants.clear();
    }
    if (contextReady) {
        // SetMeshes destroys the registered VkBuffers; the context (and its
        // descriptor references) must die before the next FFX dispatch.
        pendingContextDestroy = 1;
    }
    meshDirty = 1;
    utils::threadUnlock(&pendingLock);
}

static void onPropsTile(i32 tileX, i32 tileZ, u64 readyStamp,
                         const PropInstance* instances, u32 instanceCount,
                         const PropTileRange* ranges, u32 rangeCount,
                         char removed, void* _) {
    (void)_;
    PendingEvent e = {};
    e.removed   = removed;
    e.tileX     = tileX;
    e.tileZ     = tileZ;
    e.stamp     = readyStamp;
    if (!removed && instances && instanceCount > 0) {
        e.instances.assign(instances, instances + instanceCount);
        if (ranges && rangeCount > 0) {
            e.ranges.assign(ranges, ranges + rangeCount);
        }
    }
    utils::threadLock(&pendingLock);
    pendingEvents.push_back(std::move(e));
    utils::threadUnlock(&pendingLock);
}

// ── Context / resource management (render thread) ─────────────────────────

static void destroyResources(char keepSdf) {
    if (!keepSdf && sdfAtlas.img) {
        vulkanDestroyImage(&sdfAtlas, NULL);
        sdfAtlas = VulkanImage{};
    }
    if (brickAABBs.buf) {
        vulkanDestroyBuffer(&brickAABBs, VK_NULL_HANDLE);
        brickAABBs = VulkanBuffer{};
    }
    for (u32 i = 0; i < FFX_BRIXELIZER_MAX_CASCADES; i++) {
        if (cascadeAabbTrees[i].buf) {
            vulkanDestroyBuffer(&cascadeAabbTrees[i], VK_NULL_HANDLE);
            cascadeAabbTrees[i] = VulkanBuffer{};
        }
        if (cascadeBrickMaps[i].buf) {
            vulkanDestroyBuffer(&cascadeBrickMaps[i], VK_NULL_HANDLE);
            cascadeBrickMaps[i] = VulkanBuffer{};
        }
    }
    if (gpuScratch.buf) {
        vulkanDestroyBuffer(&gpuScratch, VK_NULL_HANDLE);
        gpuScratch = VulkanBuffer{};
    }
    gpuScratchSize = 0;
}

static void destroyContext(char keepGI, char keepSdf) {
    // The FFX backend destroys the VkImageViews it created for our resources
    // (voxelizer + GI, both effect contexts).  A re-create (mesh rebuild /
    // resolution change) can happen while the previous frame's FFX dispatch is
    // still in flight, so wait for the GPU to finish BEFORE any view is
    // destroyed — regardless of which of the two contexts is being torn down.
    if (contextReady || giContextReady) {
        vulkanWaitIdle("destroy brixelizer context");
    }
    if (contextReady) {
        ffxBrixelizerContextDestroy(&context);
        // Do NOT value-reset the ~24 MB opaque context here: `context = FfxBrixelizerContext{}`
        // builds a stack temporary that overflows the 8 MB thread stack. ensureContext fully
        // re-initializes it before any use, so clearing the readiness flag is sufficient.
        contextReady = 0;
    }
    if (!keepGI) {
        destroyGI();
    }
    destroyResources(keepSdf);
    instanceIDs.clear();
    tileRegs.clear();
    meshBuffersRegistered = 0;
    instanceCapWarned     = 0;
}

static void swapchainCreated(void* _) {
    (void)_;
    destroyContext(0, 1);
}

static char ensureBackend(void) {
    if (backendReady) {
        return 1;
    }
    // The single FFX backend interface serves BOTH the voxelizer context and
    // the GI context (the reference sample does the same), so the scratch
    // backing store must be sized for the sum of their context counts.
    const size_t totalContexts = FFX_BRIXELIZER_CONTEXT_COUNT + FFX_BRIXELIZER_GI_CONTEXT_COUNT;
    scratchBufferSize = ffxGetScratchMemorySizeVK(vulkan.physicalDevice, totalContexts);
    scratchBuffer     = calloc(1, scratchBufferSize);
    if (!scratchBuffer) {
        utils::error("vulkanBrixelizerPass: failed to allocate %zu bytes of FFX backend scratch memory",
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
        ffxGetInterfaceVK(&backendInterface, device, scratchBuffer, scratchBufferSize, totalContexts);
    if (backendResult != FFX_OK) {
        utils::error("vulkanBrixelizerPass: ffxGetInterfaceVK failed: %d", backendResult);
        free(scratchBuffer);
        scratchBuffer     = NULL;
        scratchBufferSize = 0;
        return 0;
    }
    backendReady = 1;
    return 1;
}

static char ensureContext(const float center[3]) {
    if (contextReady) {
        return 1;
    }
    if (!ensureBackend()) {
        return 0;
    }

    // The SDF atlas has a fixed voxel-based size (independent of the render
    // resolution and of which props are registered), so it is created once and
    // re-used across re-creates: destroying it would auto-destroy the FFX image
    // views that reference it (GI + voxelizer), leaving dangling handles in the
    // backend's dynamic view pool.
    if (!sdfAtlas.img) {
        sdfAtlas = vulkanCreateImage(.name        = "BrixelizerSdfAtlas",
                                     .format      = VK_FORMAT_R8_UNORM,
                                     .usage       = VK_IMAGE_USAGE_STORAGE_BIT |
                                                   VK_IMAGE_USAGE_SAMPLED_BIT |
                                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                     .type        = VK_IMAGE_TYPE_3D,
                                     .width       = FFX_BRIXELIZER_STATIC_CONFIG_SDF_ATLAS_SIZE,
                                     .height      = FFX_BRIXELIZER_STATIC_CONFIG_SDF_ATLAS_SIZE,
                                     .layers      = FFX_BRIXELIZER_STATIC_CONFIG_SDF_ATLAS_SIZE);  // layers = depth for 3D
    }
    brickAABBs = vulkanCreateGpuBuffer("brixelizer_brick_aabbs",
                                       FFX_BRIXELIZER_BRICK_AABBS_SIZE,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    for (u32 i = 0; i < FFX_BRIXELIZER_MAX_CASCADES; i++) {
        cascadeAabbTrees[i] =
            vulkanCreateGpuBuffer("brixelizer_cascade_aabb_tree",
                                  FFX_BRIXELIZER_CASCADE_AABB_TREE_SIZE,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        cascadeBrickMaps[i] =
            vulkanCreateGpuBuffer("brixelizer_cascade_brick_map",
                                  FFX_BRIXELIZER_CASCADE_BRICK_MAP_SIZE,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }

    FfxBrixelizerContextDescription desc = {};
    for (u32 i = 0; i < 3; i++) {
        desc.sdfCenter[i] = center[i];
    }
    desc.numCascades = BRIX_NUM_CASCADES;
    desc.flags       = FFX_BRIXELIZER_CONTEXT_FLAG_ALL_DEBUG;  // outStats readback needs the debug buffers
    for (u32 i = 0; i < BRIX_NUM_CASCADES; i++) {
        desc.cascadeDescs[i].flags     = FFX_BRIXELIZER_CASCADE_STATIC;
        desc.cascadeDescs[i].voxelSize  = BRIX_BASE_VOXEL_SIZE * (1 << i);
    }
    desc.backendInterface = backendInterface;

    FfxErrorCode r = ffxBrixelizerContextCreate(&desc, &context);
    if (r != FFX_OK) {
        utils::error("vulkanBrixelizerPass: ffxBrixelizerContextCreate failed: %d", r);
        destroyResources(0);
        return 0;
    }
    contextReady = 1;
    for (u32 i = 0; i < 3; i++) {
        sdfCenter[i] = center[i];
    }
    utils::info("vulkanBrixelizerPass: voxelizer context (%u static cascades, voxel %.0f..%.0f m, "
                "center %.0f, %.0f, %.0f)",
                BRIX_NUM_CASCADES,
                BRIX_BASE_VOXEL_SIZE,
                BRIX_BASE_VOXEL_SIZE * (1 << (BRIX_NUM_CASCADES - 1)),
                center[0],
                center[1],
                center[2]);
    return 1;
}

// (Re)register the props merged mesh VBO/IBO with the voxelizer context.
// Called on every frame the mesh is dirty (SetMeshes rebuilt the buffers —
// including a same-size re-upload, which recreates the VkBuffers) or not yet
// registered.  Requires contextReady.
static char registerMeshBuffers(char meshDirty) {
    if (!contextReady) {
        return 0;
    }
    if (meshBuffersRegistered && !meshDirty) {
        return 1;
    }
    VulkanBuffer vbo = {};
    VulkanBuffer ibo = {};
    u32 vCount = 0, iCount = 0;
    if (!vulkanAzgaarPropsGetMeshes(&vbo, &ibo, &vCount, &iCount)) {
        if (meshBuffersRegistered) {
            u32 old[2] = {meshVboIdx, meshIboIdx};
            ffxBrixelizerUnregisterBuffers(&context, old, 2);
            meshBuffersRegistered = 0;
        }
        return 0;
    }
    if (meshBuffersRegistered) {
        u32 old[2] = {meshVboIdx, meshIboIdx};
        ffxBrixelizerUnregisterBuffers(&context, old, 2);
        meshBuffersRegistered = 0;
    }
    FfxBrixelizerBufferDescription descs[2] = {};
    descs[0].buffer   = vulkanFfxWrapBufferResource(
        &vbo, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, FFX_RESOURCE_STATE_COMPUTE_READ, L"brixelizer_props_vbo");
    descs[0].outIndex = &meshVboIdx;
    descs[1].buffer   = vulkanFfxWrapBufferResource(
        &ibo, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, FFX_RESOURCE_STATE_COMPUTE_READ, L"brixelizer_props_ibo");
    descs[1].outIndex = &meshIboIdx;
    FfxErrorCode r = ffxBrixelizerRegisterBuffers(&context, descs, 2);
    if (r != FFX_OK) {
        utils::error("vulkanBrixelizerPass: ffxBrixelizerRegisterBuffers failed: %d", r);
        return 0;
    }
    meshBuffersRegistered = 1;
    utils::info("vulkanBrixelizerPass: registered props mesh buffers (vbo=%u, ibo=%u, verts=%u, idx=%u)",
                meshVboIdx, meshIboIdx, vCount, iCount);
    return 1;
}

// ── Static instance registration (render thread) ──────────────────────────

static u32 maxCascadeForExtent(float extent) {
    u32 m = 0;
    while (m + 1 < BRIX_NUM_CASCADES) {
        float nextVoxel = BRIX_BASE_VOXEL_SIZE * (1u << (m + 1));
        if (nextVoxel <= extent) {
            m++;
        } else {
            break;
        }
    }
    return m;
}

static TileReg* tileRegFind(i32 tileX, i32 tileZ) {
    for (u32 i = 0; i < tileRegs.size(); i++) {
        if (tileRegs[i].tileX == tileX && tileRegs[i].tileZ == tileZ) {
            return &tileRegs[i];
        }
    }
    return NULL;
}

static void tileRegDelete(TileReg* reg) {
    if (reg->count > 0) {
        FfxErrorCode r =
            ffxBrixelizerDeleteInstances(&context, &instanceIDs[reg->offset], reg->count);
        if (r != FFX_OK) {
            utils::error("vulkanBrixelizerPass: ffxBrixelizerDeleteInstances failed: %d", r);
        }
    }
    *reg = tileRegs[tileRegs.size() - 1];
    tileRegs.pop_back();
}

// Build FFX static instances for one tile's full PropInstance set: each
// instance = the (species, variant) mesh sub-range transformed by the
// instance's pos/yaw/scale.  The transform is row-major (element r,c lives
// at t[4r+c]) — the [0]/[5]/[10] diagonal; the old [0]/[4]/[8] column-major
// misread collapsed every tile to 2 bricks.
static void registerTile(const PendingEvent& e) {
    std::vector<FfxBrixelizerInstanceDescription> descs;
    std::vector<FfxBrixelizerInstanceID> outIds;
    descs.reserve(e.instances.size());

    for (u32 i = 0; i < e.instances.size(); i++) {
        const PropInstance* inst = &e.instances[i];
        const PropTileRange* range = NULL;
        for (u32 r = 0; r < e.ranges.size(); r++) {
            if (i >= e.ranges[r].start && i < e.ranges[r].start + e.ranges[r].count) {
                range = &e.ranges[r];
                break;
            }
        }
        if (!range) {
            continue;
        }
        const PropVariantRange* vr = NULL;
        for (u32 v = 0; v < variants.size(); v++) {
            if (variants[v].species == inst->species && variants[v].variant == inst->variant) {
                vr = &variants[v];
                break;
            }
        }
        if (!vr) {
            continue;
        }
        if (instanceIDs.size() + descs.size() >= FFX_BRIXELIZER_MAX_INSTANCES) {
            if (!instanceCapWarned) {
                instanceCapWarned = 1;
                utils::warn("vulkanBrixelizerPass: FFX instance cap (%u) hit — remaining tiles stay un-voxelized",
                           FFX_BRIXELIZER_MAX_INSTANCES);
            }
            break;
        }

        FfxBrixelizerInstanceDescription d = {};
        d.aabb.min[0] = inst->pos[0] + inst->scale * vr->boundsMin[0];
        d.aabb.min[1] = inst->pos[1] + inst->scale * vr->boundsMin[1];
        d.aabb.min[2] = inst->pos[2] + inst->scale * vr->boundsMin[2];
        d.aabb.max[0] = inst->pos[0] + inst->scale * vr->boundsMax[0];
        d.aabb.max[1] = inst->pos[1] + inst->scale * vr->boundsMax[1];
        d.aabb.max[2] = inst->pos[2] + inst->scale * vr->boundsMax[2];

        float c    = cosf(inst->yaw);
        float s    = sinf(inst->yaw);
        float k    = inst->scale;
        float t[12] = {
            c * k, 0.0f, s * k, inst->pos[0],
            0.0f, k, 0.0f, inst->pos[1],
            -s * k, 0.0f, c * k, inst->pos[2],
        };
        memcpy(d.transform, t, sizeof(t));

        d.indexFormat       = FFX_INDEX_TYPE_UINT32;
        d.indexBuffer       = meshIboIdx;
        d.indexBufferOffset = vr->indexOffset;
        d.triangleCount     = vr->indexCount / 3;
        d.vertexBuffer      = meshVboIdx;
        d.vertexStride      = (u32)sizeof(PropsVertex);
        d.vertexBufferOffset = 0;
        d.vertexCount       = meshVertCount;
        d.vertexFormat      = FFX_SURFACE_FORMAT_R32G32B32_FLOAT;

        float dx   = vr->boundsMax[0] - vr->boundsMin[0];
        float dy   = vr->boundsMax[1] - vr->boundsMin[1];
        float dz   = vr->boundsMax[2] - vr->boundsMin[2];
        float ext = fmaxf(fmaxf(dx, dy), dz) * inst->scale;
        d.maxCascade = maxCascadeForExtent(ext);

        d.flags = FFX_BRIXELIZER_INSTANCE_FLAG_NONE;  // static: baked once, re-submitted by the per-frame update
        d.outInstanceID = NULL;                       // filled in the second pass
        descs.push_back(d);
    }

    if (descs.empty()) {
        return;
    }
    outIds.resize(descs.size());
    for (u32 i = 0; i < descs.size(); i++) {
        descs[i].outInstanceID = &outIds[i];
    }
    FfxErrorCode r = ffxBrixelizerCreateInstances(&context, descs.data(), (u32)descs.size());
    if (r != FFX_OK) {
        utils::error("vulkanBrixelizerPass: ffxBrixelizerCreateInstances failed for tile (%d,%d): %d",
                     e.tileX, e.tileZ, r);
        return;
    }
    u32 base = (u32)instanceIDs.size();
    instanceIDs.insert(instanceIDs.end(), outIds.begin(), outIds.end());
    tileRegs.push_back(TileReg{.tileX   = e.tileX,
                               .tileZ   = e.tileZ,
                               .stamp   = e.stamp,
                               .offset  = base,
                               .count   = (u32)outIds.size()});
    utils::info("vulkanBrixelizerPass: registered tile (%d,%d) stamp=%llu — %u instances "
                "(%zu total, %zu tiles)",
                e.tileX,
                e.tileZ,
                (unsigned long long)e.stamp,
                (u32)outIds.size(),
                instanceIDs.size(),
                tileRegs.size());
}

// Drain the pending events into a local batch and apply them.  The whole
// process runs under pendingLock: the game/pool threads write `variants`,
// `meshDirty` and the queue from the props callbacks, and a props pool worker
// may be mid-SetTile while the render thread registers a tile.
// Returns 0 when the context is not ready yet: the events stay queued for
// the next update (order matters — a remove must not overtake a create of
// the same tile).
static char processPendingEvents(void) {
    std::vector<PendingEvent> batch = {};
    utils::threadLock(&pendingLock);
    if (!contextReady || !meshBuffersRegistered || variants.empty()) {
        utils::threadUnlock(&pendingLock);
        return 0;
    }
    batch = std::move(pendingEvents);
    pendingEvents.clear();

    for (u32 i = 0; i < batch.size(); i++) {
        PendingEvent* e = &batch[i];
        TileReg*      reg = tileRegFind(e->tileX, e->tileZ);
        if (e->removed) {
            if (reg) {
                utils::info("vulkanBrixelizerPass: evicted tile (%d,%d) — deleted %u instances",
                           e->tileX, e->tileZ, reg->count);
                tileRegDelete(reg);
            }
            continue;
        }
        if (reg) {
            if (reg->stamp == e->stamp) {
                continue;  // cull re-upload of the same build — keep the full registration
            }
            tileRegDelete(reg);  // tile rebuilt under a new stamp
        }
        registerTile(*e);
    }
    utils::threadUnlock(&pendingLock);
    return 1;
}

// ── GI stage (task 3) ────────────────────────────────────────────────────
// Convert a cglm mat4 (column-major, m[col][row]) into the FFX row-major
// float[16] the GI dispatch expects (element (r,c) at index 4*r+c):
// out[4*r+c] = m[c][r].
static void mat4ToRowMajor(const mat4 m, float out[16]) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            out[4 * r + c] = m[c][r];
}

static void giReadEnvKnobs(void) {
    if (giLogInit) {
        return;
    }
    giLogInit = 1;
    giDebugMode = 0;
    const char* d = getenv("ENGINE_BRIX_GI_DEBUG");
    if (d && d[0]) {
        if (strcmp(d, "irradiance") == 0) giDebugMode = 2;
        else giDebugMode = 1;  // "radiance" (or any other value) -> radiance cache
        utils::info("vulkanBrixelizerPass: GI debug visualization = %s",
                    giDebugMode == 1 ? "radiance cache" : "irradiance cache");
    }
    giSaveEnabled = 0;
    const char* s = getenv("ENGINE_BRIX_GI_SAVE");
    if (s && s[0]) {
        giSaveEnabled = 1;
    }
    giSaveEvery = BRIX_GI_SAVE_EVERY_DEFAULT;
    const char* e = getenv("ENGINE_BRIX_GI_SAVE_EVERY");
    if (e && e[0]) {
        giSaveEvery = (u32)atoi(e);
        if (giSaveEvery < 1) giSaveEvery = 1;
    }
    if (giSaveEnabled) {
        utils::info("vulkanBrixelizerPass: GI debug save every %u frames (ENGINE_BRIX_GI_SAVE)", giSaveEvery);
    }
    const char* m = getenv("ENGINE_BRIX_GI_MASK_SAVE");
    if (m && m[0] && strcmp(m, "0") != 0) {
        giMaskSaveEnabled = 1;
        utils::info("vulkanBrixelizerPass: GI mask save enabled (ENGINE_BRIX_GI_MASK_SAVE)");
    }
}

// A 128^2 two-channel blue-noise tile, generated on the CPU (two independent
// per-pixel hashes) and uploaded once.  Not true blue noise (the FFX sample
// ships precomputed LDR_RG01 tiles) but provides uncorrelated [0,1) RG values,
// which is all the GI's per-ray jitter consumes (SampleBlueNoise reads .xy of
// a 128-tile-masked coordinate).
static char createNoiseTexture(void) {
    if (giNoiseCreated) {
        return 1;
    }
    const u32 n = BRIX_GI_NOISE_SIZE;
    std::vector<u8> px((size_t)n * n * 2);
    for (u32 y = 0; y < n; y++) {
        for (u32 x = 0; x < n; x++) {
            u64 h = (u64)x * 0x9E3779B97F4A7C15ULL + (u64)y * 0x65D25D4B1B2F5D73ULL + (u64)n * 0xC2B2AE3D27D4EB4FULL;
            h ^= h >> 33;
            h *= 0xFF51AFD7ED558CCDULL;
            h ^= h >> 33;
            u64 g = h ^ 0x85EBCA6B;
            g ^= g >> 31;
            g *= 0xC2B2AE3D27D4EB4FULL;
            g ^= g >> 31;
            size_t i = ((size_t)y * n + x) * 2;
            px[i]     = (u8)(h & 0xFFu);
            px[i + 1] = (u8)(g & 0xFFu);
        }
    }
    giNoiseTexture = vulkanCreateImage(.name        = "BrixelizerGINoise",
                                       .format      = VK_FORMAT_R8G8_UNORM,
                                       .usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                       .width       = (int)n,
                                       .height      = (int)n);
    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanTransition(cmd, &giNoiseTexture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);
    vulkanCopy(.cmd         = cmd,
               .target.img  = &giNoiseTexture,
               .source.data  = px.data(),
               .size         = (u32)px.size());
    vulkanTransition(cmd, &giNoiseTexture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransientEnd(cmd, 1);
    giNoiseCreated = 1;
    return 1;
}

static void destroyGI(void) {
    if (giContextReady) {
        ffxBrixelizerGIContextDestroy(&giContext);
        // Do NOT value-reset the opaque GI context (same stack-overflow hazard
        // as the voxelizer context); ensureGI fully re-initializes it before use.
        giContextReady = 0;
    }
    giWidth           = 0;
    giHeight          = 0;
    giHasPrevMatrices = 0;
    giFrameIndex      = 0;
    if (giHistoryDepth.img) {
        vulkanDestroyImage(&giHistoryDepth, NULL);
        giHistoryDepth = VulkanImage{};
    }
    if (giHistoryNormal.img) {
        vulkanDestroyImage(&giHistoryNormal, NULL);
        giHistoryNormal = VulkanImage{};
    }
    if (giPrevLitOutput.img) {
        vulkanDestroyImage(&giPrevLitOutput, NULL);
        giPrevLitOutput = VulkanImage{};
    }
    if (giDiffuse.img) {
        vulkanDestroyImage(&giDiffuse, NULL);
        giDiffuse = VulkanImage{};
    }
    if (giSpecular.img) {
        vulkanDestroyImage(&giSpecular, NULL);
        giSpecular = VulkanImage{};
    }
    if (giDebugVisualization.img) {
        vulkanDestroyImage(&giDebugVisualization, NULL);
        giDebugVisualization = VulkanImage{};
    }
    if (giDebugTarget.img) {
        vulkanDestroyImage(&giDebugTarget, NULL);
        giDebugTarget = VulkanImage{};
    }
    if (giDisocclusionMask.img) {
        vulkanDestroyImage(&giDisocclusionMask, NULL);
        giDisocclusionMask = VulkanImage{};
    }
    if (giNoiseTexture.img) {
        vulkanDestroyImage(&giNoiseTexture, NULL);
        giNoiseTexture = VulkanImage{};
        giNoiseCreated = 0;
    }
}

static char ensureGI(u32 width, u32 height) {
    if (giContextReady) {
        if (giWidth == width && giHeight == height) {
            return 1;
        }
        destroyGI();  // render resolution changed
    }
    if (!contextReady || width == 0 || height == 0) {
        return 0;
    }
    if (!ensureBackend()) {
        return 0;
    }
    if (!createNoiseTexture()) {
        return 0;
    }

    const u32 w = width, h = height;
    const VkImageUsageFlags histUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags outUsage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    giHistoryDepth       = vulkanCreateImage(.name    = "BrixelizerGIHistoryDepth",
                                              .format  = VK_FORMAT_D32_SFLOAT,
                                              .aspect  = VK_IMAGE_ASPECT_DEPTH_BIT,
                                              .usage   = histUsage,
                                              .width   = (int)w,
                                              .height  = (int)h);
    giHistoryNormal      = vulkanCreateImage(.name = "BrixelizerGIHistoryNormal",
                                              .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                              .usage = histUsage,
                                              .width = (int)w,
                                              .height = (int)h);
    giPrevLitOutput      = vulkanCreateImage(.name = "BrixelizerGIPrevLitOutput",
                                              .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                              .usage = histUsage,
                                              .width = (int)w,
                                              .height = (int)h);
    giDiffuse            = vulkanCreateImage(.name = "BrixelizerGIDiffuse",
                                              .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                              .usage = outUsage,
                                              .width = (int)w,
                                              .height = (int)h);
    giSpecular           = vulkanCreateImage(.name = "BrixelizerGISpecular",
                                              .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                              .usage = outUsage,
                                              .width = (int)w,
                                              .height = (int)h);
    giDebugVisualization = vulkanCreateImage(.name = "BrixelizerGIDebugVis",
                                              .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                              .usage = outUsage,
                                              .width = (int)w,
                                              .height = (int)h);
    giWidth               = w;
    giHeight              = h;
    giHasPrevMatrices     = 0;
    giFrameIndex          = 0;

    // One-time init in a transient submit (completes before the main buffer runs
    // the GI dispatch): zero the color history + output images so the first
    // frame's reproject starts from a clean miss instead of sampling
    // uninitialized VRAM.  The D32 history depth is left un-cleared: an
    // uninitialized depth just fails the reproject's depth comparison and the
    // cache converges from frame 2 (and a depth image can't be vkCmdClearColorImage'd).
    {
        VulkanCommand* cmd = vulkanTransientBegin();
        const VkClearColorValue black = {};
        VkImageSubresourceRange range = {};
        range.aspectMask   = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = 0;
        range.levelCount   = 1;
        range.baseArrayLayer = 0;
        range.layerCount   = 1;
        VulkanImage* zero[] = {&giHistoryNormal,
                               &giPrevLitOutput};
        for (VulkanImage* im : zero) {
            vulkanTransition(cmd, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);
            vkCmdClearColorImage(cmd->cmd, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
            vulkanTransition(cmd, im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        }
        // The GI writes its outputs as UAVs (GENERAL).  They are not cleared (the
        // GI overwrites them fully) but are moved to a concrete GENERAL layout and
        // this transient is submitted+waited below, so any read-back (save) sees
        // them in GENERAL rather than the UNDEFINED layout a fresh image starts in.
        VulkanImage* out[] = {&giDiffuse,
                              &giSpecular,
                              &giDebugVisualization};
        for (VulkanImage* im : out) {
            vulkanTransition(cmd, im, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        }
        vulkanTransientEnd(cmd, 1);
    }

    FfxBrixelizerGIContextDescription desc = {};
    // DEPTH_INVERTED: the engine depth buffer is reverse-Z (0=far, 1=near).
    // Internal 50% resolution (the sample's default) upsamples to the full
    // render resolution inside the dispatch.
    desc.flags              = FFX_BRIXELIZER_GI_FLAG_DEPTH_INVERTED;
    desc.internalResolution = FFX_BRIXELIZER_GI_INTERNAL_RESOLUTION_50_PERCENT;
    desc.displaySize        = {w, h};
    desc.backendInterface   = backendInterface;

    FfxErrorCode r = ffxBrixelizerGIContextCreate(&giContext, &desc);
    if (r != FFX_OK) {
        utils::error("vulkanBrixelizerPass: ffxBrixelizerGIContextCreate failed: %d", r);
        destroyGI();
        return 0;
    }
    giContextReady = 1;
    utils::info("vulkanBrixelizerPass: GI context created (%ux%u, 50%% internal, DEPTH_INVERTED)", w, h);

    // Mask-save diagnostic images (fork patch): the GI dispatch can redirect the
    // per-pixel debug target (red = weight_sum<1e-3 history reset) and the
    // disocclusion mask to caller-owned images.  The sizes come from the fork
    // getter so they match the component's float internal-size arithmetic
    // exactly (probe buffer for the debug target, internal for the mask).
    if (giMaskSaveEnabled) {
        u32 debugSize[2]   = {0, 0};
        u32 disoccSize[2]  = {0, 0};
        if (ffxBrixelizerGIGetDebugOutputSizes(&giContext, debugSize, disoccSize) == FFX_OK &&
            debugSize[0] && debugSize[1] && disoccSize[0] && disoccSize[1]) {
            giDebugTarget    = vulkanCreateImage(.name   = "BrixelizerGIDebugTarget",
                                                 .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                                 .usage  = outUsage,
                                                 .width  = (int)debugSize[0],
                                                 .height = (int)debugSize[1]);
            giDisocclusionMask = vulkanCreateImage(.name   = "BrixelizerGIDisocclusionMask",
                                                   .format = VK_FORMAT_R8_UNORM,
                                                   .usage  = outUsage,
                                                   .width  = (int)disoccSize[0],
                                                   .height = (int)disoccSize[1]);
            // Same layout discipline as the other GI outputs: the FFX backend
            // expects UAV-registered resources to sit in a concrete GENERAL
            // layout (a fresh image starts UNDEFINED), so move them there in a
            // submitted transient before any dispatch can bind them.
            VulkanCommand* cmd = vulkanTransientBegin();
            vulkanTransition(cmd, &giDebugTarget, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
            vulkanTransition(cmd, &giDisocclusionMask, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
            vulkanTransientEnd(cmd, 1);
            utils::info("vulkanBrixelizerPass: GI mask save images %ux%u (debug) + %ux%u (disocclusion)",
                       debugSize[0], debugSize[1], disoccSize[0], disoccSize[1]);
        } else {
            utils::warn("vulkanBrixelizerPass: ffxBrixelizerGIGetDebugOutputSizes failed — mask save disabled");
            giMaskSaveEnabled = 0;
        }
    }
    return 1;
}

// Per-frame GI: feed the SDF the voxelizer just updated plus the current
// GBuffer and trace diffuse/specular GI into the output images.  The history
// copies are ordered around the dispatch so each input the GI reads is the
// previous frame's value and each copy targets the next frame's value:
//   * the composite image still holds frame N-1's composite at this point in
//     the pipeline (the composite pass runs AFTER us), so copying it to
//     prevLitOutput before the dispatch yields the correct previous lit output;
//   * depth/normals for frame N are already written, so they are copied into
//     the history AFTER the dispatch (the GI reads the stale history first).
static void dispatchGI(VulkanCommand* cmd, Camera* camera) {
    if (!giContextReady) {
        return;
    }

    VulkanImage* depth   = vulkanFrameResourcesGetDepth();
    VulkanImage* worldNormal = vulkanFrameResourcesGetWorldNormal();
    VulkanImage* material = vulkanFrameResourcesGetMaterial();
    VulkanImage* velocity = vulkanFrameResourcesGetVelocity();
    VulkanImage* composite = vulkanFrameResourcesGetCompositeColor();
    VulkanImage* envMap   = vulkanIblGetEnvironmentPrefilter();
    VulkanImage* sdfAtlas = vulkanBrixelizerPassGetSdfAtlas();
    VulkanBuffer* brickAABBs = vulkanBrixelizerPassGetBrickAABBs();
    if (!depth || !worldNormal || !material || !velocity || !envMap || !sdfAtlas || !brickAABBs) {
        return;
    }

    // ── Pre-dispatch: bring the read-only inputs to the layout the FFX backend
    // will sample them in (SHADER_READ_ONLY).  The backend inserts no barrier
    // on its first wrap of a resource per frame, so the engine must leave them
    // in exactly the target layout (same convention as the FSR/AO FFX passes).
    vulkanTransition(cmd, worldNormal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, material, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, velocity, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, envMap, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    // The D32 depth (both the live and history) is sampled directly by the GI the
    // way the CACAO AO pass does — transition to the sampled layout.
    vulkanTransition(cmd, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, &giHistoryDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    // The SDF atlas is written by the voxelizer (a separate FFX effect context) and
    // read by the GI (another context) — the FFX backend does not share layout state
    // across contexts, so it will NOT transition the atlas from the voxelizer's
    // UNORDERED_ACCESS (GENERAL) to the sampled layout.  Do it here so the GI's
    // SHADER_READ_ONLY binding matches the actual layout.  (The GI's outputs are
    // already in GENERAL from the ensureGI transient.)
    vulkanTransition(cmd, sdfAtlas, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    // prevLitOutput <- the composite image (currently holding frame N-1's data,
    // since the composite pass runs after us).  Same-format color-to-color copy.
    if (composite) {
        vulkanCopyColorImage(cmd, composite, &giPrevLitOutput, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    // ── Build the dispatch description.
    FfxBrixelizerGIDispatchDescription desc = {};
    mat4ToRowMajor(camera->cameraUbo.view, desc.view);
    mat4ToRowMajor(camera->cameraUbo.projection, desc.projection);
    if (giHasPrevMatrices) {
        memcpy(desc.prevView, giPrevViewRow, sizeof(giPrevViewRow));
        memcpy(desc.prevProjection, giPrevProjectionRow, sizeof(giPrevProjectionRow));
    } else {
        // First frame after (re)creation: the reproject rejects the (wrong) prev
        // matrices via the depth/normal mismatch and the cache converges from
        // frame 2.  Falling back to the current matrices keeps it finite.
        mat4ToRowMajor(camera->cameraUbo.view, desc.prevView);
        mat4ToRowMajor(camera->cameraUbo.projection, desc.prevProjection);
    }
    // World camera position = column 3 of the inverse view.
    desc.cameraPosition[0] = camera->cameraUbo.invView[3][0];
    desc.cameraPosition[1] = camera->cameraUbo.invView[3][1];
    desc.cameraPosition[2] = camera->cameraUbo.invView[3][2];

    desc.startCascade       = BRIX_GI_START_CASCADE;
    desc.endCascade         = BRIX_GI_END_CASCADE;
    desc.rayPushoff         = BRIX_GI_RAY_PUSHOFF;
    desc.sdfSolveEps        = BRIX_GI_SDF_EPS;
    desc.specularRayPushoff = BRIX_GI_RAY_PUSHOFF;
    desc.specularSDFSolveEps = BRIX_GI_SDF_EPS;
    desc.tMin               = BRIX_GI_TMIN;
    desc.tMax               = BRIX_GI_TMAX;

    // The engine world normal is a raw [-1,1] unit direction (R16G16B16A16 rgb),
    // so it unpacks with mul=1/add=0 (NOT the sample's 2/-1, which assumes a
    // [0,1] re-encode).  Roughness is the material R channel.
    desc.normalsUnpackMul      = 1.0f;
    desc.normalsUnpackAdd      = 0.0f;
    desc.isRoughnessPerceptual = false;
    desc.roughnessChannel      = 0;
    desc.roughnessThreshold    = BRIX_GI_ROUGH_THRESHOLD;
    desc.environmentMapIntensity = BRIX_GI_ENV_INTENSITY;
    // Engine motion vectors are per-pixel displacements in PIXEL units (the GBuffer
    // writes (ndcCurrent - ndcPrev) * viewport * 0.5, y-flipped so +y is screen-down).
    // The GI reprojection does `history_uv = uv + texelFetch(MV).xy * scale` in
    // [0,1] UV space and needs the offset to point from the CURRENT pixel to its
    // PREVIOUS position (prev - current).  The engine's velocity is (current - prev)
    // in pixels, so scale must both flip it to (prev - current) and convert pixels to
    // UV.  Using -1,-1 (pixel units) pushes history_uv off-screen for any non-zero
    // motion, so the disocclusion mask rejects every pixel and the temporal history
    // never converges (per-frame stochastic GI noise).  Scaling by 1/render-res fixes
    // it; giWidth/giHeight == the velocity texture's render resolution.
    desc.motionVectorScale.x   = -1.0f / (float)giWidth;
    desc.motionVectorScale.y   = -1.0f / (float)giHeight;

    desc.depth         = vulkanFfxWrapImageResource(depth, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"brix_gi_depth");
    desc.historyDepth  = vulkanFfxWrapImageResource(&giHistoryDepth, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"brix_gi_history_depth");
    desc.normal        = vulkanFfxWrapImageResource(worldNormal, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"brix_gi_normal");
    desc.historyNormal = vulkanFfxWrapImageResource(&giHistoryNormal, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"brix_gi_history_normal");
    desc.roughness     = vulkanFfxWrapImageResource(material, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"brix_gi_roughness");
    desc.motionVectors = vulkanFfxWrapImageResource(velocity, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"brix_gi_velocity");
    desc.prevLitOutput = vulkanFfxWrapImageResource(&giPrevLitOutput, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"brix_gi_prev_lit");
    desc.noiseTexture  = vulkanFfxWrapImageResource(&giNoiseTexture, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"brix_gi_noise");
    desc.environmentMap = vulkanFfxWrapImageResource(envMap, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"brix_gi_env");

    desc.sdfAtlas    = vulkanFfxWrapImageResource(sdfAtlas, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"brix_gi_sdf_atlas");
    desc.bricksAABBs = vulkanFfxWrapBufferResource(brickAABBs, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, FFX_RESOURCE_STATE_COMPUTE_READ, L"brix_gi_brick_aabbs");
    for (u32 i = 0; i < FFX_BRIXELIZER_MAX_CASCADES; i++) {
        VulkanBuffer* tree = vulkanBrixelizerPassGetCascadeAabbTree(i);
        VulkanBuffer* bmap = vulkanBrixelizerPassGetCascadeBrickMap(i);
        wchar_t       name[64];
        swprintf(name, 64, L"brix_gi_cascade_aabb_tree_%u", i);
        desc.cascadeAABBTrees[i] = tree ? vulkanFfxWrapBufferResource(tree, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, FFX_RESOURCE_STATE_COMPUTE_READ, name) : FfxResource{};
        swprintf(name, 64, L"brix_gi_cascade_brick_map_%u", i);
        desc.cascadeBrickMaps[i] = bmap ? vulkanFfxWrapBufferResource(bmap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, FFX_RESOURCE_STATE_COMPUTE_READ, name) : FfxResource{};
    }

    desc.outputDiffuseGI  = vulkanFfxWrapImageResource(&giDiffuse, FFX_RESOURCE_USAGE_UAV, FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"brix_gi_diffuse");
    desc.outputSpecularGI = vulkanFfxWrapImageResource(&giSpecular, FFX_RESOURCE_USAGE_UAV, FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"brix_gi_specular");
    if (giMaskSaveEnabled) {
        desc.outputDebugTarget      = vulkanFfxWrapImageResource(&giDebugTarget, FFX_RESOURCE_USAGE_UAV, FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"brix_gi_debug_target");
        desc.outputDisocclusionMask = vulkanFfxWrapImageResource(&giDisocclusionMask, FFX_RESOURCE_USAGE_UAV, FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"brix_gi_disocc_mask");
    }

    FfxBrixelizerRawContext* rawContext = NULL;
    FfxErrorCode             rc = ffxBrixelizerGetRawContext(&context, &rawContext);
    if (rc != FFX_OK || !rawContext) {
        utils::error("vulkanBrixelizerPass: ffxBrixelizerGetRawContext failed: %d", rc);
        return;
    }
    desc.brixelizerContext = rawContext;

    FfxErrorCode result = ffxBrixelizerGIContextDispatch(&giContext, &desc, ffxGetCommandListVK(cmd->cmd));
    if (result != FFX_OK) {
        utils::error("vulkanBrixelizerPass: ffxBrixelizerGIContextDispatch failed: %d", result);
        destroyGI();
        return;
    }

    // ── Post-dispatch: capture this frame's depth/normal into the history for
    // the next frame's reprojection.
    vulkanCopyDepthImage(cmd, depth, &giHistoryDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vulkanCopyColorImage(cmd, worldNormal, &giHistoryNormal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Store the current view/projection as next frame's prev matrices.
    mat4ToRowMajor(camera->cameraUbo.view, giPrevViewRow);
    mat4ToRowMajor(camera->cameraUbo.projection, giPrevProjectionRow);
    giHasPrevMatrices = 1;

    // ── Optional FFX debug visualization (radiance/irradiance cache) into the
    // dedicated debug image — the "debug-vis output" for pre-composite check.
    if (giDebugMode > 0) {
        FfxBrixelizerGIDebugDescription ddesc = {};
        mat4ToRowMajor(camera->cameraUbo.view, ddesc.view);
        mat4ToRowMajor(camera->cameraUbo.projection, ddesc.projection);
        ddesc.startCascade   = BRIX_GI_START_CASCADE;
        ddesc.endCascade     = BRIX_GI_END_CASCADE;
        ddesc.outputSize[0]  = giWidth;
        ddesc.outputSize[1]  = giHeight;
        ddesc.normalsUnpackMul = 1.0f;
        ddesc.normalsUnpackAdd = 0.0f;
        ddesc.debugMode = giDebugMode == 1 ? FFX_BRIXELIZER_GI_DEBUG_MODE_RADIANCE_CACHE
                                           : FFX_BRIXELIZER_GI_DEBUG_MODE_IRRADIANCE_CACHE;
        ddesc.depth    = vulkanFfxWrapImageResource(depth, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"brix_gi_debug_depth");
        ddesc.normal   = vulkanFfxWrapImageResource(worldNormal, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"brix_gi_debug_normal");
        ddesc.sdfAtlas = vulkanFfxWrapImageResource(sdfAtlas, FFX_RESOURCE_USAGE_READ_ONLY, FFX_RESOURCE_STATE_COMPUTE_READ, L"brix_gi_debug_sdf_atlas");
        ddesc.bricksAABBs = vulkanFfxWrapBufferResource(brickAABBs, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, FFX_RESOURCE_STATE_COMPUTE_READ, L"brix_gi_debug_brick_aabbs");
        for (u32 i = 0; i < FFX_BRIXELIZER_MAX_CASCADES; i++) {
            VulkanBuffer* tree = vulkanBrixelizerPassGetCascadeAabbTree(i);
            VulkanBuffer* bmap = vulkanBrixelizerPassGetCascadeBrickMap(i);
            wchar_t       name[64];
            swprintf(name, 64, L"brix_gi_dbg_cascade_aabb_tree_%u", i);
            ddesc.cascadeAABBTrees[i] = tree ? vulkanFfxWrapBufferResource(tree, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, FFX_RESOURCE_STATE_COMPUTE_READ, name) : FfxResource{};
            swprintf(name, 64, L"brix_gi_dbg_cascade_brick_map_%u", i);
            ddesc.cascadeBrickMaps[i] = bmap ? vulkanFfxWrapBufferResource(bmap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, FFX_RESOURCE_STATE_COMPUTE_READ, name) : FfxResource{};
        }
        ddesc.outputDebug = vulkanFfxWrapImageResource(&giDebugVisualization, FFX_RESOURCE_USAGE_UAV, FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"brix_gi_debug_out");
        FfxBrixelizerRawContext* dRaw = NULL;
        if (ffxBrixelizerGetRawContext(&context, &dRaw) == FFX_OK && dRaw) {
            ddesc.brixelizerContext = dRaw;
            FfxErrorCode drc = ffxBrixelizerGIContextDebugVisualization(&giContext, &ddesc, ffxGetCommandListVK(cmd->cmd));
            if (drc != FFX_OK) {
                utils::error("vulkanBrixelizerPass: ffxBrixelizerGIContextDebugVisualization failed: %d", drc);
            }
        }
    }

    // ── Optional on-disk save of the GI outputs for pre-composite verification.
    if (giSaveEnabled && giFrameIndex % giSaveEvery == 0) {
        char path[1024];
        snprintf(path, sizeof(path), "/tmp/brix_gi_diffuse_%u.jpg", giFrameIndex);
        vulkanSaveImage(&giDiffuse, path);
        snprintf(path, sizeof(path), "/tmp/brix_gi_specular_%u.jpg", giFrameIndex);
        vulkanSaveImage(&giSpecular, path);
        if (giDebugMode > 0) {
            snprintf(path, sizeof(path), "/tmp/brix_gi_debug_%u.jpg", giFrameIndex);
            vulkanSaveImage(&giDebugVisualization, path);
        }
        utils::info("vulkanBrixelizerPass: saved GI debug images (frame %u)", giFrameIndex);
    }

    // ── Optional raw per-frame dumps of the per-pixel debug target and the
    // disocclusion mask (fork patch) — raw bytes so the exact 0/1 flag values
    // survive (the auto-normalized jpg path would rescale the flags).
    if (giMaskSaveEnabled && giSaveEnabled && giFrameIndex % giSaveEvery == 0) {
        char path[1024];
        snprintf(path, sizeof(path), "/tmp/brix_gi_mask_debug_%u.raw", giFrameIndex);
        vulkanSaveImageRaw(&giDebugTarget, path);
        snprintf(path, sizeof(path), "/tmp/brix_gi_mask_disocc_%u.raw", giFrameIndex);
        vulkanSaveImageRaw(&giDisocclusionMask, path);
        utils::info("vulkanBrixelizerPass: saved GI mask dumps (frame %u)", giFrameIndex);
    }
    giFrameIndex++;
}

// ── Per-frame voxelizer update (render thread) ────────────────────────────

void VulkanBrixelizerPass::update() {
    if (vulkan.skipFrame) {
        return;
    }

    // Mesh rebuilds destroy the buffers the FFX context references — the
    // context (and its registered-buffer indices / instance table) has to
    // die before the next FFX dispatch touches them.
    char destroy = 0;
    char dirty   = 0;
    {
        utils::threadLock(&pendingLock);
        destroy              = pendingContextDestroy;
        dirty                = meshDirty;
        pendingContextDestroy = 0;
        meshDirty             = 0;
        utils::threadUnlock(&pendingLock);
        if (destroy) {
            // A props mesh rebuild (new props streaming in) invalidates the
            // voxelizer context, but NOT the SDF atlas (fixed voxel-sized) or the
            // GI context (re-wraps the atlas every frame).  Keep both alive to
            // avoid destroying their FFX image views while the previous frame's
            // dispatch is still in flight (a validation CRIT).
            destroyContext(1, 1);
        }
    }

    Entity* camEntity = cameraGetEntity();
    Camera* camera    = camEntity ? getComponent(camEntity->scene, Camera, camEntity->id) : NULL;
    if (!camera) {
        return;
    }

    // Camera-following clipmap center (cglm mat4 = vec4 columns; column 3 of
    // the inverse view is the camera position), snapped so a cascade only
    // scrolls when the camera crosses a snap cell (64 m keeps cascade 0's
    // 64 m radius always covering the camera).
    float center[3] = {camera->cameraUbo.invView[3][0],
                       camera->cameraUbo.invView[3][1],
                       camera->cameraUbo.invView[3][2]};
    for (u32 i = 0; i < 3; i++) {
        center[i] = floorf(center[i] / BRIX_CENTER_SNAP + 0.5f) * BRIX_CENTER_SNAP;
    }

    if (!ensureContext(center)) {
        return;
    }
    if (!registerMeshBuffers(dirty)) {
        return;
    }
    if (!processPendingEvents()) {
        return;  // no props meshes yet: nothing to update (stats would read zero)
    }

    VulkanCommand* cmd = vulkan.currentCmd;
    // The FFX backend maps FFX_RESOURCE_STATE_UNORDERED_ACCESS to
    // VK_IMAGE_LAYOUT_GENERAL and inserts its own barriers; keep the atlas
    // out of UNDEFINED and in a layout the backend understands.
    vulkanTransition(cmd, &sdfAtlas, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

    FfxBrixelizerStats stats = {};
    FfxBrixelizerUpdateDescription desc = {};
    desc.resources.sdfAtlas =
        vulkanFfxWrapImageResource(&sdfAtlas,
                                   FFX_RESOURCE_USAGE_UAV,
                                   FFX_RESOURCE_STATE_UNORDERED_ACCESS,
                                   L"brixelizer_sdf_atlas");
    desc.resources.brickAABBs =
        vulkanFfxWrapBufferResource(&brickAABBs,
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                    FFX_RESOURCE_STATE_UNORDERED_ACCESS,
                                    L"brixelizer_brick_aabbs");
    for (u32 i = 0; i < FFX_BRIXELIZER_MAX_CASCADES; i++) {
        wchar_t name[64];
        swprintf(name, 64, L"brixelizer_cascade_aabb_tree_%u", i);
        desc.resources.cascadeResources[i].aabbTree =
            vulkanFfxWrapBufferResource(&cascadeAabbTrees[i],
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        FFX_RESOURCE_STATE_UNORDERED_ACCESS,
                                        name);
        swprintf(name, 64, L"brixelizer_cascade_brick_map_%u", i);
        desc.resources.cascadeResources[i].brickMap =
            vulkanFfxWrapBufferResource(&cascadeBrickMaps[i],
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        FFX_RESOURCE_STATE_UNORDERED_ACCESS,
                                        name);
    }

    desc.frameIndex = frameIndex;
    for (u32 i = 0; i < 3; i++) {
        desc.sdfCenter[i] = center[i];
    }
    desc.populateDebugAABBsFlags  = FFX_BRIXELIZER_POPULATE_AABBS_NONE;
    desc.debugVisualizationDesc    = NULL;
    desc.maxReferences            = BRIX_MAX_REFERENCES;
    desc.triangleSwapSize          = BRIX_TRIANGLE_SWAP_SIZE;
    desc.maxBricksPerBake          = BRIX_MAX_BRICKS_PER_BAKE;
    size_t scratchNeed             = 0;
    desc.outScratchBufferSize      = &scratchNeed;
    desc.outStats                  = &stats;

    vulkanBeginProfile(cmd, &profile, 0);
    FfxErrorCode r = ffxBrixelizerBakeUpdate(&context, &desc, &bakedUpdateDesc);
    if (r != FFX_OK) {
        utils::error("vulkanBrixelizerPass: ffxBrixelizerBakeUpdate failed: %d", r);
        vulkanEndProfile(cmd, &profile, 0);
        destroyContext(0, 0);
        return;
    }
    if (scratchNeed > gpuScratchSize) {
        if (gpuScratch.buf) {
            vulkanDestroyBuffer(&gpuScratch, VK_NULL_HANDLE);
            gpuScratch = VulkanBuffer{};
        }
        gpuScratchSize = (scratchNeed + 4095) & ~(size_t)4095;
        gpuScratch     = vulkanCreateGpuBuffer("brixelizer_scratch",
                                               gpuScratchSize,
                                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        utils::info("vulkanBrixelizerPass: allocated %zu B of update scratch", gpuScratchSize);
    }
    FfxResource scratchRes =
        vulkanFfxWrapBufferResource(&gpuScratch,
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                    FFX_RESOURCE_STATE_UNORDERED_ACCESS,
                                    L"brixelizer_scratch");
    r = ffxBrixelizerUpdate(&context, &bakedUpdateDesc, scratchRes, ffxGetCommandListVK(cmd->cmd));
    vulkanEndProfile(cmd, &profile, 0);
    if (r != FFX_OK) {
        utils::error("vulkanBrixelizerPass: ffxBrixelizerUpdate failed: %d", r);
        destroyContext(0, 0);
        return;
    }

    frameIndex++;
    for (u32 i = 0; i < 3; i++) {
        sdfCenter[i] = center[i];
    }

    // Lagged readback (~3 frames behind). The health signal is the RESIDENT
    // brick count: totalBricks - freeBricks. The per-cascade stats below and
    // brickAllocationsAttempted are per-UPDATE deltas — they legitimately read
    // 0 in steady state (a frozen static cascade is only re-submitted, not
    // re-allocated), so a 0 there does NOT mean "no geometry": that was the
    // original misread of this task. A real collapse (the old transform bug)
    // shows as a TINY resident count against a large registered instance
    // population, not as 0 deltas.
    if (frameIndex % BRIX_STATS_LOG_STRIDE == 0) {
        u32 freeBricks   = stats.contextStats.freeBricks;
        u32 bricksInUse  = (u32)FFX_BRIXELIZER_MAX_BRICKS_X8 - freeBricks;
        utils::info("brixelizer: f=%u updCasc=%u bricksInUse=%u freeBricks=%u (total=%u) "
                    "attempted=%u cleared=%u merged=%u "
                    "static(tri=%u ref=%u brick=%u) dynamic(tri=%u ref=%u brick=%u) "
                    "instances=%u tiles=%u",
                    frameIndex,
                    stats.cascadeIndex,
                    bricksInUse,
                    freeBricks,
                    (u32)FFX_BRIXELIZER_MAX_BRICKS_X8,
                    stats.contextStats.brickAllocationsAttempted,
                    stats.contextStats.bricksCleared,
                    stats.contextStats.bricksMerged,
                    stats.staticCascadeStats.trianglesAllocated,
                    stats.staticCascadeStats.referencesAllocated,
                    stats.staticCascadeStats.bricksAllocated,
                    stats.dynamicCascadeStats.trianglesAllocated,
                    stats.dynamicCascadeStats.referencesAllocated,
                    stats.dynamicCascadeStats.bricksAllocated,
                    (u32)instanceIDs.size(),
                    (u32)tileRegs.size());
    }

    // ── GI stage (task 3) ────────────────────────────────────────────────────
    // The voxelizer refreshed the SDF this frame; trace GI against it.  Reached
    // only when the context is ready and props are registered (the early returns
    // above guard that).  ensureGI (re)creates the GI context + resources on first
    // use or a render-resolution change; dispatchGI feeds the SDF + current
    // GBuffer, produces the diffuse/specular GI, and copies the frame into the
    // temporal history.  The output is not yet consumed by the composite (task 4);
    // ENGINE_BRIX_GI_DEBUG/SAVE expose it for pre-composite verification.
    giReadEnvKnobs();
    if (window.renderWidth > 0 && window.renderHeight > 0) {
        if (ensureGI((u32)window.renderWidth, (u32)window.renderHeight)) {
            dispatchGI(cmd, camera);
        }
    }
}

// ── Accessors (the GI pass consumes these) ─────────────────────────────────

FfxBrixelizerContext* vulkanBrixelizerPassGetContext(void) {
    return contextReady ? &context : NULL;
}

char vulkanBrixelizerPassIsEnabled(void) {
    return contextReady;
}

char vulkanBrixelizerPassGetSdfCenter(float out[3]) {
    if (!contextReady) {
        return 0;
    }
    for (u32 i = 0; i < 3; i++) {
        out[i] = sdfCenter[i];
    }
    return 1;
}

u32 vulkanBrixelizerPassGetNumCascades(void) {
    return BRIX_NUM_CASCADES;
}

struct VulkanImage* vulkanBrixelizerPassGetSdfAtlas(void) {
    return contextReady ? &sdfAtlas : NULL;
}

struct VulkanBuffer* vulkanBrixelizerPassGetBrickAABBs(void) {
    return contextReady ? &brickAABBs : NULL;
}

struct VulkanBuffer* vulkanBrixelizerPassGetCascadeAabbTree(u32 cascade) {
    if (!contextReady || cascade >= FFX_BRIXELIZER_MAX_CASCADES) {
        return NULL;
    }
    return &cascadeAabbTrees[cascade];
}

struct VulkanBuffer* vulkanBrixelizerPassGetCascadeBrickMap(u32 cascade) {
    if (!contextReady || cascade >= FFX_BRIXELIZER_MAX_CASCADES) {
        return NULL;
    }
    return &cascadeBrickMaps[cascade];
}

struct VulkanImage* vulkanBrixelizerPassGetDiffuseGI(void) {
    return giContextReady && giDiffuse.img ? &giDiffuse : NULL;
}

struct VulkanImage* vulkanBrixelizerPassGetSpecularGI(void) {
    return giContextReady && giSpecular.img ? &giSpecular : NULL;
}

struct VulkanImage* vulkanBrixelizerPassGetDebugVisualization(void) {
    return giContextReady && giDebugVisualization.img ? &giDebugVisualization : NULL;
}

char vulkanBrixelizerPassGIReady(void) {
    return giContextReady;
}

char vulkanBrixelizerPassGetGIResolution(u32* outWidth, u32* outHeight) {
    if (!giContextReady) {
        return 0;
    }
    if (outWidth) {
        *outWidth = giWidth;
    }
    if (outHeight) {
        *outHeight = giHeight;
    }
    return 1;
}

char vulkanBrixelizerPassGIResolutionMatches(const struct VulkanImage* gi) {
    if (!giContextReady || !gi || !gi->img) {
        return 0;
    }
    return gi->extent.width == giWidth && gi->extent.height == giHeight;
}
}  // namespace engine
