#include "renderer/vulkan/pass/azgaar_props/VulkanAzgaarPropsPass.h"
#include "renderer/vulkan/pass/brixelizer/VulkanBrixelizerPass.h"
#include <iterator>
#include "renderer/Renderer.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/resources/VulkanBuffer.h"
#include "renderer/vulkan/scene/VulkanScene.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "thread/Thread.h"

namespace engine {

VulkanAzgaarPropsPass vulkanAzgaarPropsPass;

VulkanAzgaarPropsPass::VulkanAzgaarPropsPass() : System("azgaar_props") {}

// Must match the GLSL `PropPush` block (std430).
typedef struct PropPushConstants {
    float boundsMin[4]; // xyz = local AABB min (metres), w unused
    float boundsMax[4]; // xyz = local AABB max (metres), w unused
    float swayFactor;   // 0..1, how much this species sways
    float lodRole;      // 0 = near LOD, 1 = far LOD, 2 = no LOD (always visible)
} PropPushConstants;

// Same data as PropPushConstants plus the active cascade index, for the CSM
// shadow pipe.  Must match the GLSL `PropShadowPush` block (std430).
typedef struct PropShadowPushConstants {
    float boundsMin[4]; // xyz = local AABB min (metres), w unused
    float boundsMax[4]; // xyz = local AABB max (metres), w unused
    float swayFactor;   // 0..1, how much this species sways
    float lodRole;      // 0 = near LOD, 1 = far LOD, 2 = no LOD (always visible)
    u32   cascadeIndex; // selects sceneBuffer.shadow.shadowViewProjection[i]
} PropShadowPushConstants;

// ── State ─────────────────────────────────────────────────────────────────

static VulkanPipe pipe;
static VulkanPipe prepassPipe;
static VulkanPipe shadowPipe;
static bool pipeRecreated = false;

// Merged species-mesh buffer (all species + variants concatenated; per-variant
// sub-ranges selected by PropVariantRange.indexOffset/indexCount).
static VulkanBuffer meshVbo;
static VulkanBuffer meshIbo;
static u32 meshVertCount = 0;
static u32 meshIdxCount  = 0;

// Per-(species, variant) metadata table (owned copy).
static std::vector<PropVariantRange> variants;

// Per-tile GPU instance buffers (one entry per resident props tile).
typedef struct PropGpuTile {
    i32   tileX, tileZ;
    u64   readyStamp;
    bool  inUse;
    VulkanBuffer ibo;
    u32     instanceCount;
    std::vector<PropTileRange> ranges = {};
    u32     rangeCount;
} PropGpuTile;
static std::vector<PropGpuTile>   gpuTiles   = {};

// World-space Y range of the per-tile frustum-cull AABBs (the pass only knows
// the tile XZ footprint; the terrain height span is scene-specific).  A box
// whose top sits below the actual ground is wrongly culled whenever the bottom
// frustum plane passes between the two, so the owning scene must set a range
// that covers its terrain (vulkanAzgaarPropsSetTileYBounds).
static float tileAabbYMin = -20.0f;
static float tileAabbYMax = 40.0f;

// Pending tile uploads.  The instance buffer is BUILT (VMA alloc + staging
// copy) on the enqueueing thread (props pool worker, or the main thread at
// world load); the copy is submitted non-blocking (vulkanTransientEndAsync)
// so workers never stall behind the graphics queue.  The render thread polls
// the entry's fence in preUpdate and adopts the buffer only once the GPU
// copy has completed; unfinished entries are re-queued for the next frame.
struct PendingTileUpload {
    i32   tileX = 0, tileZ = 0;
    u64   readyStamp = 0;
    bool  clear = false; // clear == drop this tile's buffer
    VulkanBuffer ibo = {};      // pre-built (zero when clear / count 0)
    VulkanCommand* cmd = nullptr;    // owns the in-flight transient copy (NULL once adopted)
    u32     instanceCount = 0;
    std::vector<PropTileRange> ranges = {};
    u32     rangeCount = 0;
};
static std::vector<PendingTileUpload> pendingTiles = {};
static bool                     clearAllFlag = false; // drained on render thread
static utils::Thread uploadLock = {.mutex = PTHREAD_MUTEX_INITIALIZER};

// IBOs displaced by a newer upload (or a tile clear): the GPU may still be
// drawing them for up to FRAMES_IN_FLIGHT frames (the render loop does not
// fence per frame), so destruction is deferred by that many preUpdates.
static std::vector<VulkanBuffer> retiredIbos = {};

static void retireIbo(VulkanBuffer* ibo) {
    if (ibo->buf) {
        retiredIbos.push_back(*ibo);
        *ibo = VulkanBuffer{};
    }
}

static void retireFlush(void) {
    while (static_cast<i32>(retiredIbos.size()) > FRAMES_IN_FLIGHT) {
        VulkanBuffer b  = retiredIbos[0];
        retiredIbos.erase(retiredIbos.begin() + 0u);
        vulkanDestroyBuffer(&b, VK_NULL_HANDLE);
    }
}

// Whole-map global instance buffer (settlement buildings, workstream D).
typedef struct PropGpuGlobal {
    bool  inUse;
    VulkanBuffer ibo;
    u32  instanceCount;
    std::vector<PropTileRange> ranges = {};
    u32  rangeCount;
    float aabbMin[3];
    float aabbMax[3];
} PropGpuGlobal;
static PropGpuGlobal gpuGlobal = {};

// Pending global uploads (pre-built instance buffer, see PendingTileUpload).
struct PendingGlobalUpload {
    bool  clear = false;
    VulkanBuffer ibo = {};         // pre-built (zero when clear / count 0)
    u32     instanceCount = 0;
    std::vector<PropTileRange> ranges = {};
    u32     rangeCount = 0;
    float   aabbMin[3] = {};
    float   aabbMax[3] = {};
};
static std::vector<PendingGlobalUpload> pendingGlobals = {};

// Second whole-map slot (landmark props, workstream E): same layout as the
// settlements' global slot.
static PropGpuGlobal gpuLandmarks = {};
static std::vector<PendingGlobalUpload> pendingLandmarks = {};

static bool enabled = true; // master switch (vulkanAzgaarPropsSetEnabled)

// ── Pipeline management ───────────────────────────────────────────────────

// Mesh vertices (PropsVertex layout, 60 B) carry position (loc 0), normal
// (loc 1), uv (loc 8, for the flower alpha test) and a per-vertex texture-array
// index (loc 9, selects textures[id] in the global set 0).  Instances
// (PropInstance, 44 B) are binding 1 at instance rate: pos (loc 2), yaw (loc 3),
// scale (loc 4), color (loc 5), phase (loc 6), species (loc 7).  The `variant`
// field (offset 40) is a CPU-side grouping key only — the pass binds the matching
// variant's index sub-range, so no new GPU vertex attribute is added.
static VkVertexInputBindingDescription vertexBindings[] = {
    {.binding = 0, .stride = sizeof(PropsVertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX},
    {.binding = 1, .stride = sizeof(PropInstance), .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE},
};
static VkVertexInputAttributeDescription vertexAttrs[] = {
    {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0},
    {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 12},
    {.location = 8, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 40},
    {.location = 9, .binding = 0, .format = VK_FORMAT_R32_UINT, .offset = 56},
    {.location = 10, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 60},
    {.location = 2, .binding = 1, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0},
    {.location = 3, .binding = 1, .format = VK_FORMAT_R32_SFLOAT, .offset = 12},
    {.location = 4, .binding = 1, .format = VK_FORMAT_R32_SFLOAT, .offset = 16},
    {.location = 5, .binding = 1, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 20},
    {.location = 6, .binding = 1, .format = VK_FORMAT_R32_SFLOAT, .offset = 32},
    {.location = 7, .binding = 1, .format = VK_FORMAT_R32_UINT, .offset = 36},
};

// Per-pipeline subsets of the full attribute list, matching each vertex
// shader's actual interface.  Extra attributes are legal but the driver's
// performance layer warns about every attribute not consumed by the shader,
// so each pipe only declares what its .vert actually reads.
static VkVertexInputAttributeDescription depthPrepassAttrs[] = {
    {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0},
    {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 12},
    {.location = 8, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 40},
    {.location = 9, .binding = 0, .format = VK_FORMAT_R32_UINT, .offset = 56},
    {.location = 2, .binding = 1, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0},
    {.location = 3, .binding = 1, .format = VK_FORMAT_R32_SFLOAT, .offset = 12},
    {.location = 4, .binding = 1, .format = VK_FORMAT_R32_SFLOAT, .offset = 16},
    {.location = 5, .binding = 1, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 20},
    {.location = 6, .binding = 1, .format = VK_FORMAT_R32_SFLOAT, .offset = 32},
    {.location = 7, .binding = 1, .format = VK_FORMAT_R32_UINT, .offset = 36}, // no loc 10 (inVertColor)
};
static VkVertexInputAttributeDescription shadowAttrs[] = { // only what azgaar_props_shadow.vert reads
    {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0},
    {.location = 8, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 40},
    {.location = 9, .binding = 0, .format = VK_FORMAT_R32_UINT, .offset = 56},
    {.location = 2, .binding = 1, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0},
    {.location = 3, .binding = 1, .format = VK_FORMAT_R32_SFLOAT, .offset = 12},
    {.location = 4, .binding = 1, .format = VK_FORMAT_R32_SFLOAT, .offset = 16},
    {.location = 6, .binding = 1, .format = VK_FORMAT_R32_SFLOAT, .offset = 32},
};

static void recreatePipelines(void) {
    if (pipe.pipe) vulkanDestroyPipe(&pipe);
    if (prepassPipe.pipe) vulkanDestroyPipe(&prepassPipe);
    if (shadowPipe.pipe) vulkanDestroyPipe(&shadowPipe);

    // Opaque, depth-write on (like the terrain pass) so props occlude against
    // and are occluded by the heightmap terrain.  Scene colour + depth +
    // G-buffer (normals + material) attachments are bound so the props
    // overwrite the terrain's stale G-buffer data (fixes road decals bleeding
    // onto tree trunks via the decal pass' GROUND_ONLY normal check).
    pipe = vulkanCreatePipe(
        .name                 = "azgaar_props",
        .vs                   = "shaders/pass/azgaar_props/spv/azgaar_props.vert.spv",
        .fs                   = "shaders/pass/azgaar_props/spv/azgaar_props.frag.spv",
        .colorFormat1         = VK_FORMAT_R16G16B16A16_SFLOAT,
        .colorFormat2         = VK_FORMAT_R16G16_SFLOAT,
        .colorFormat3         = VK_FORMAT_R8G8B8A8_UNORM,
        .depthFormat          = VK_FORMAT_D32_SFLOAT,
        .noCull               = 1, // low-poly props: avoid back-face pops
        .blend                = 0, // opaque: LOD is a hard distance switch, no cross-fade blend
        .clearColor1          = {0, 0, 0, 0}, .clearColor1Enabled = 0,
        .clearColor2          = {0, 0, 0, 0}, .clearColor2Enabled = 0, // terrain pass clears; props load on top
        .clearColor3          = {0, 0, 0, 0}, .clearColor3Enabled = 0,
        .clearDepth           = {0, 0}, .clearDepthEnabled = 0,
        .vertexAttributes     = vertexAttrs,
        .vertexAttributeCount = 11,
        .vertexBindings       = vertexBindings,
        .vertexBindingCount   = 2);

    // Depth/velocity pre-pass pipe: writes motion vectors (velocity) and
    // view-space normal XY into the same attachments the depth pass owns,
    // so FSR gets valid per-pixel motion vectors for animated props.
    //
    // It must ALSO write depth (not depth-test-only): contact shadow and
    // HiZ all run BEFORE this pass' colour draws and consume the main
    // depth buffer.  Without prop depth in that buffer they reconstruct a
    // surface that ignores the props (a building's pixels read as the grass
    // behind it, while viewNormal carries the building's normal) and emit
    // phantom occlusion blobs shaped like the props themselves; the
    // geometry shaders then multiply those blobs into their shading and the
    // props look see-through (e.g. a character silhouette shows up on a
    // building wall behind the character).  Safe to write here: the
    // pre-pass rasterizes with the exact same jittered projection, wind
    // sway and LOD switch as the colour pass, so the colour pass' depth
    // test (GREATER_OR_EQUAL) passes on equal values.
    prepassPipe = vulkanCreatePipe(
        .name                 = "azgaar_props_depth_prepass",
        .vs                   = "shaders/pass/azgaar_props/spv/azgaar_props_depth.vert.spv",
        .fs                   = "shaders/pass/azgaar_props/spv/azgaar_props_depth.frag.spv",
        .colorFormat1         = VK_FORMAT_R16G16_SFLOAT,
        .colorFormat2         = VK_FORMAT_R16G16_SNORM,
        .colorFormat3         = VK_FORMAT_R16G16B16A16_SFLOAT,
        .depthFormat          = VK_FORMAT_D32_SFLOAT,
        .noCull               = 1,
        .vertexAttributes     = depthPrepassAttrs,
        .vertexAttributeCount = 10,
        .vertexBindings       = vertexBindings,
        .vertexBindingCount   = 2);

    // Sun-shadow (CSM) pipe: draws the props into the shadow pass' D32 cascade
    // maps so vegetation / buildings / landmarks cast shadows.  Mirrors the
    // shadow CSM pipes' depth setup (reverse-Z 1.0 clear, LESS_OR_EQUAL,
    // clamp, slope bias, double-sided) so prop casters behave like scene
    // casters.  It is drawn INSIDE the shadow pass' own render pass (see
    // vulkanAzgaarPropsDrawShadow) and never begins a render itself.
    shadowPipe = vulkanCreatePipe(
        .name                    = "azgaar_props_shadow",
        .vs                      = "shaders/pass/azgaar_props/spv/azgaar_props_shadow.vert.spv",
        .fs                      = "shaders/pass/azgaar_props/spv/azgaar_props_shadow.frag.spv",
        .depthFormat             = VK_FORMAT_D32_SFLOAT,
        .clearDepth              = {1.0f, 0},
        .clearDepthEnabled       = 1,
        .depthClamp              = 1,
        .noCull                  = 1, // low-poly props: avoid back-face pops
        .depthCompareOp          = VK_COMPARE_OP_LESS_OR_EQUAL,
        .depthBiasEnable         = 1,
        .depthBiasConstantFactor = 0.0f,
        .depthBiasSlopeFactor    = 0.8f,
        .depthBiasClamp          = 0.0f,
        .vertexAttributes        = shadowAttrs,
        .vertexAttributeCount    = 7,
        .vertexBindings          = vertexBindings,
        .vertexBindingCount      = 2);
    pipeRecreated = true;
}

static void swapchainCreated(void*) {
    recreatePipelines();
}

// ── Mesh + species table (game thread, once per world load) ───────────────

void vulkanAzgaarPropsSetMeshes(const void* verts, u32 vertCount,
                                const void* idx, u32 idxCount) {
    utils::threadLock(&uploadLock);
    if (meshVbo.buf) vulkanDestroyBuffer(&meshVbo, VK_NULL_HANDLE);
    if (meshIbo.buf) vulkanDestroyBuffer(&meshIbo, VK_NULL_HANDLE);
    meshVertCount = vertCount;
    meshIdxCount  = idxCount;
    if (vertCount > 0 && idxCount > 0) {
        meshVbo = vulkanCreateGpuBuffer(
            utils::strtmp("azgaar_props_mesh_vbo"),
            vertCount * sizeof(PropsVertex),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        meshIbo = vulkanCreateGpuBuffer(
            utils::strtmp("azgaar_props_mesh_ibos"),
            idxCount * sizeof(u32),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        VulkanCommand* cmd = vulkanTransientBegin();
        vulkanCopy(.cmd = cmd, .source.data = (void*)verts, .target.buf = &meshVbo,
                   .size = static_cast<u32>(vertCount * sizeof(PropsVertex)));
        vulkanCopy(.cmd = cmd, .source.data = (void*)idx, .target.buf = &meshIbo,
                   .size = static_cast<u32>(idxCount * sizeof(u32)));
        vulkanTransientEnd(cmd, 1);

        // [TEMP DEBUG] read the device buffers back to verify the upload.
        if (getenv("ENGINE_PROPS_READBACK")) {
            VulkanBuffer rbIbo = vulkanCreateCpuBuffer(utils::strtmp("azgaar_props_rb_ibo"), idxCount * sizeof(u32),
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            VulkanBuffer rbVbo = vulkanCreateCpuBuffer(utils::strtmp("azgaar_props_rb_vbo"), vertCount * sizeof(PropsVertex),
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            VulkanCommand* rc = vulkanTransientBegin();
            vulkanCopy(.cmd = rc, .source.buf = &meshIbo, .target.buf = &rbIbo, .size = static_cast<u32>(idxCount * sizeof(u32)));
            vulkanCopy(.cmd = rc, .source.buf = &meshVbo, .target.buf = &rbVbo, .size = static_cast<u32>(vertCount * sizeof(PropsVertex)));
            vulkanTransientEnd(rc, 1);  // waits: data now in host memory
            FILE* fi = fopen("/tmp/gpu_readback_ibo.bin", "wb");
            if (fi) {
                fwrite(rbIbo.vmaInfo.pMappedData, 1, idxCount * sizeof(u32), fi);
                fclose(fi);
            }
            FILE* fv = fopen("/tmp/gpu_readback_vbo.bin", "wb");
            if (fv) {
                fwrite(rbVbo.vmaInfo.pMappedData, 1, vertCount * sizeof(PropsVertex), fv);
                fclose(fv);
            }
            utils::info("azgaarProps: READBACK wrote /tmp/gpu_readback_{ibo,vbo}.bin (%u idx, %u verts)", idxCount, vertCount);
            vulkanDestroyBuffer(&rbIbo, NULL);
            vulkanDestroyBuffer(&rbVbo, NULL);
        }
    } else {
        meshVbo = VulkanBuffer{};
        meshIbo = VulkanBuffer{};
    }
    utils::threadUnlock(&uploadLock);
}

void vulkanAzgaarPropsSetVariants(const PropVariantRange* table, u32 count) {
    utils::threadLock(&uploadLock);
    variants.clear();
    if (table && count > 0) {
        variants.assign(table, table + count);
    }
    utils::threadUnlock(&uploadLock);
}

// ── Per-tile instance buffers (built on the caller thread, adopted on the
// ── render thread) ────────────────────────────────────────────────────────
// The VMA alloc + staging copy run HERE on the calling thread (a props pool
// worker in steady state, the main thread at world load).  The copy is
// submitted NON-BLOCKING: the worker returns immediately and the render
// thread adopts the buffer in preUpdate once its fence signals, so neither
// workers nor the main thread ever stall behind the graphics queue.

void vulkanAzgaarPropsSetTile(i32 tileX, i32 tileZ, u64 readyStamp,
                              const PropInstance* instances, u32 instanceCount,
                              const PropTileRange* ranges, u32 rangeCount) {
    PendingTileUpload p = {};
    p.tileX       = tileX;
    p.tileZ       = tileZ;
    p.readyStamp  = readyStamp;
    p.clear       = false;
    p.instanceCount = instanceCount;
    p.rangeCount  = rangeCount;
    if (instances && instanceCount > 0) {
        static int hitchOn = -1;
        if (hitchOn < 0) hitchOn = getenv("ENGINE_HITCH_DEBUG") != NULL;
        double t0 = utils::nanos();
        u64 need = (u64)instanceCount * sizeof(PropInstance);
        p.ibo = vulkanCreateGpuBuffer("azgaar_props_inst",
                                      need,
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        VulkanCommand* cmd = vulkanTransientBegin();
        vulkanCopy(.cmd = cmd, .source.data = (void*)instances, .target.buf = &p.ibo,
                   .size = static_cast<u32>(need));
        vulkanTransientEndAsync(cmd);
        p.cmd = cmd;
        if (hitchOn) utils::info("HITCH: props tile(%d,%d) IBO build %u insts %.1f MB in %.1f ms (worker, async)",
                           tileX, tileZ, instanceCount, (double)need / 1048576.0, (utils::nanos() - t0) / 1e6);
    }
    if (ranges && rangeCount > 0) {
        p.ranges.assign(ranges, ranges + rangeCount);
    }
    utils::threadLock(&uploadLock);
    pendingTiles.push_back(p);
    utils::threadUnlock(&uploadLock);

    /* Step 5 (plans/brixelizer-gi.md): mirror this tile's scatter into the
     * Brixelizer SDF (thread-safe; the brixelizer pass budgets the instances
     * on the render thread). A count-0 push drops the tile, so clear there. */
    if (instanceCount > 0) {
        vulkanBrixelizerPassPropsTileSet(tileX, tileZ, readyStamp, instances, instanceCount);
    } else {
        vulkanBrixelizerPassPropsTileClear(tileX, tileZ);
    }
}

void vulkanAzgaarPropsClearTile(i32 tileX, i32 tileZ) {
    PendingTileUpload p = {.tileX = tileX, .tileZ = tileZ, .clear = true};
    utils::threadLock(&uploadLock);
    pendingTiles.push_back(p);
    utils::threadUnlock(&uploadLock);

    vulkanBrixelizerPassPropsTileClear(tileX, tileZ);
}

void vulkanAzgaarPropsClearAll(void) {
    // Flag drained by the render thread (avoids touching gpuTiles off-thread).
    utils::threadLock(&uploadLock);
    clearAllFlag = true;
    utils::threadUnlock(&uploadLock);
}

void vulkanAzgaarPropsSetTileYBounds(float yMin, float yMax) {
    if (yMin >= yMax) return;
    utils::threadLock(&uploadLock);
    tileAabbYMin = yMin;
    tileAabbYMax = yMax;
    utils::threadUnlock(&uploadLock);
}

// ── Whole-map global instance buffers (workstream D + E) ──────────────────

// Shared enqueue for the two whole-map slots (settlements, landmarks): builds
// the instance buffer on the calling thread (see vulkanAzgaarPropsSetTile) and
// copies only the ranges to the heap under the upload lock.
static void enqueueGlobal(std::vector<PendingGlobalUpload>* queue,
                          const PropInstance* instances, u32 instanceCount,
                          const PropTileRange* ranges, u32 rangeCount,
                          const float aabbMin[3], const float aabbMax[3]) {
    PendingGlobalUpload p = {};
    p.instanceCount = instanceCount;
    p.rangeCount    = rangeCount;
    memcpy(p.aabbMin, aabbMin, sizeof(p.aabbMin));
    memcpy(p.aabbMax, aabbMax, sizeof(p.aabbMax));
    if (instances && instanceCount > 0) {
        u64 need = (u64)instanceCount * sizeof(PropInstance);
        p.ibo = vulkanCreateGpuBuffer("azgaar_props_inst",
                                      need,
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        VulkanCommand* cmd = vulkanTransientBegin();
        vulkanCopy(.cmd = cmd, .source.data = (void*)instances, .target.buf = &p.ibo,
                   .size = static_cast<u32>(need));
        vulkanTransientEnd(cmd, 1);

        // [TEMP DEBUG] read back the instance buffer.
        if (getenv("ENGINE_PROPS_READBACK")) {
            const char* slot = (queue == &pendingLandmarks) ? "landmark" : "settle";
            VulkanBuffer rb = vulkanCreateCpuBuffer(utils::strtmp("azgaar_props_rb_inst"), need,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            VulkanCommand* rc = vulkanTransientBegin();
            vulkanCopy(.cmd = rc, .source.buf = &p.ibo, .target.buf = &rb, .size = static_cast<u32>(need));
            vulkanTransientEnd(rc, 1);
            char path[128];
            snprintf(path, sizeof(path), "/tmp/gpu_readback_%s_inst.bin", slot);
            FILE* f = fopen(path, "wb");
            if (f) {
                fwrite(rb.vmaInfo.pMappedData, 1, need, f);
                fclose(f);
            }
            utils::info("azgaarProps: READBACK wrote %s (%u instances)", path, instanceCount);
            vulkanDestroyBuffer(&rb, NULL);
        }
    }
    if (ranges && rangeCount > 0) {
        p.ranges.assign(ranges, ranges + rangeCount);
    }
    utils::threadLock(&uploadLock);
    queue->push_back(p);
    utils::threadUnlock(&uploadLock);
}

void vulkanAzgaarPropsSetGlobal(const PropInstance* instances, u32 instanceCount,
                                  const PropTileRange* ranges, u32 rangeCount,
                                  const float aabbMin[3], const float aabbMax[3]) {
    enqueueGlobal(&pendingGlobals, instances, instanceCount, ranges, rangeCount, aabbMin, aabbMax);
    if (instanceCount > 0) {
        vulkanBrixelizerPassPropsGlobalSet(instances, instanceCount);
    } else {
        vulkanBrixelizerPassPropsGlobalClear();
    }
}

void vulkanAzgaarPropsClearGlobal(void) {
    utils::threadLock(&uploadLock);
    pendingGlobals.push_back(PendingGlobalUpload{.clear = true});
    utils::threadUnlock(&uploadLock);

    vulkanBrixelizerPassPropsGlobalClear();
}

void vulkanAzgaarPropsSetLandmarks(const PropInstance* instances, u32 instanceCount,
                                    const PropTileRange* ranges, u32 rangeCount,
                                    const float aabbMin[3], const float aabbMax[3]) {
    enqueueGlobal(&pendingLandmarks, instances, instanceCount, ranges, rangeCount, aabbMin, aabbMax);
    if (instanceCount > 0) {
        vulkanBrixelizerPassPropsLandmarksSet(instances, instanceCount);
    } else {
        vulkanBrixelizerPassPropsLandmarksClear();
    }
}

void vulkanAzgaarPropsClearLandmarks(void) {
    utils::threadLock(&uploadLock);
    pendingLandmarks.push_back(PendingGlobalUpload{.clear = true});
    utils::threadUnlock(&uploadLock);

    vulkanBrixelizerPassPropsLandmarksClear();
}

void vulkanAzgaarPropsSetEnabled(bool e) {
    enabled = e;
}

// ── Render-thread upload consumption ──────────────────────────────────────

static PropGpuTile* gpuTileFind(i32 tileX, i32 tileZ) {
    for (u32 i = 0; i < gpuTiles.size(); i++) {
        if (gpuTiles[i].inUse && gpuTiles[i].tileX == tileX && gpuTiles[i].tileZ == tileZ) {
            return &gpuTiles[i];
        }
    }
    return NULL;
}

static void gpuTileDestroy(PropGpuTile* e) {
    retireIbo(&e->ibo);
    e->ranges.clear();
    *e = PropGpuTile{};
}

static void applyPendingTiles(void) {
    // Drain the pending queue (buffers already built on the game/pool threads)
    // and adopt them into the per-tile GPU table.  Runs on the render thread.
    utils::threadLock(&uploadLock);
    bool     doClearAll = clearAllFlag;
    clearAllFlag        = false;
    std::vector<PendingTileUpload> batch = {};
    while (!pendingTiles.empty()) {
        batch.push_back(pendingTiles[0]);
        pendingTiles.erase(pendingTiles.begin() + 0);
    }
    utils::threadUnlock(&uploadLock);

    if (doClearAll) {
        for (u32 i = 0; i < gpuTiles.size(); i++) {
            if (gpuTiles[i].inUse) gpuTileDestroy(&gpuTiles[i]);
        }
    }

    for (u32 i = 0; i < batch.size(); i++) {
        PendingTileUpload* p = &batch[i];
        if (p->cmd) {
            if (vkGetFenceStatus(vulkan.device, p->cmd->fence) != VK_SUCCESS) {
                // GPU copy still in flight: keep the entry queued and retry on
                // the next preUpdate (ownership moves to the re-queued copy).
                utils::threadLock(&uploadLock);
                pendingTiles.push_back(*p);
                p->cmd   = NULL;
                p->ranges.clear();
                p->ibo   = VulkanBuffer{};
                utils::threadUnlock(&uploadLock);
                continue;
            }
            vulkanTransientFinish(p->cmd);
            p->cmd = NULL;
        }
        PropGpuTile*       e = gpuTileFind(p->tileX, p->tileZ);
        if (!e) {
            if (!p->clear && p->instanceCount > 0) {
                gpuTiles.push_back(PropGpuTile{});
                e = &gpuTiles[static_cast<i32>(gpuTiles.size()) - 1u];
                e->tileX = p->tileX;
                e->tileZ = p->tileZ;
            } else {
                continue; // clear of an absent tile: nothing to do
            }
        }

        if (p->clear) {
            gpuTileDestroy(e);
            u32 idx = static_cast<u32>(std::distance(gpuTiles.data(), e));
            gpuTiles[idx] = gpuTiles.back();
            gpuTiles.pop_back();
            p->ranges.clear();
            continue;
        }

        // Adopt the pre-built buffer; retire the previous one (the GPU may
        // still be drawing it — see retiredIbos).
        if (e->ibo.buf) retireIbo(&e->ibo);
        e->ranges.clear();
        e->rangeCount     = 0;
        e->instanceCount  = 0;
        e->readyStamp     = p->readyStamp;
        e->inUse          = true;

        if (p->instanceCount > 0) {
            e->ibo           = p->ibo;
            p->ibo           = VulkanBuffer{};
            e->instanceCount = p->instanceCount;
            e->ranges        = std::move(p->ranges); // takes ownership
            e->rangeCount    = p->rangeCount;
            p->ranges.clear();            // consumed (a later clear in this batch frees it via gpuTileDestroy)
        }
    }

    // Free batch entries whose payload was NOT handed to a tile (count-0
    // uploads, clears of absent tiles).
    for (u32 i = 0; i < batch.size(); i++) {
        if (batch[i].cmd) vulkanTransientFinish(batch[i].cmd); // defensive: normally NULL here
        if (batch[i].ibo.buf) vulkanDestroyBuffer(&batch[i].ibo, VK_NULL_HANDLE);
    }
}

static void gpuSetDestroy(PropGpuGlobal* g) {
    retireIbo(&g->ibo);
    g->ranges.clear();
    *g = PropGpuGlobal{};
}

// Drain a pending global-upload queue (game/pool thread -> render thread),
// adopting the slot's pre-built whole-map instance buffer.  Called in preUpdate.
static void drainGlobalQueue(std::vector<PendingGlobalUpload>* queue, PropGpuGlobal* target,
                             const char* bufferName) {
    (void)bufferName;
    utils::threadLock(&uploadLock);
    std::vector<PendingGlobalUpload> gbatch = {};
    while (!queue->empty()) {
        gbatch.push_back((*queue)[0]);
        queue->erase(queue->begin());
    }
    utils::threadUnlock(&uploadLock);

    for (u32 i = 0; i < gbatch.size(); i++) {
        PendingGlobalUpload* p = &gbatch[i];
        if (p->clear) {
            gpuSetDestroy(target);
            p->ranges.clear();
            continue;
        }
        if (p->instanceCount > 0) {
            if (target->ibo.buf) retireIbo(&target->ibo);
            target->ibo          = p->ibo;
            p->ibo               = VulkanBuffer{};
            target->ranges.clear();
            target->inUse         = true;
            target->instanceCount = p->instanceCount;
            target->ranges        = std::move(p->ranges); // takes ownership
            target->rangeCount    = p->rangeCount;
            p->ranges.clear();
            memcpy(target->aabbMin, p->aabbMin, sizeof(target->aabbMin));
            memcpy(target->aabbMax, p->aabbMax, sizeof(target->aabbMax));
        } else {
            gpuSetDestroy(target);
            p->ranges.clear();
        }
        if (p->ibo.buf) vulkanDestroyBuffer(&p->ibo, VK_NULL_HANDLE); // zero unless adopted above
    }
}

// ── Frustum culling ───────────────────────────────────────────────────────

static bool aabbOutsideFrustum(const vec3 min, const vec3 max, vec4* planes) {
    for (i32 i = 0; i < 6; i++) {
        vec4 p = {planes[i][0], planes[i][1], planes[i][2], planes[i][3]};
        vec3 positive = {
            p[0] >= 0.0f ? max[0] : min[0],
            p[1] >= 0.0f ? max[1] : min[1],
            p[2] >= 0.0f ? max[2] : min[2],
        };
        if ((p[0] * positive[0]) + (p[1] * positive[1]) + (p[2] * positive[2]) + p[3] < 0.0f) {
            return true;
        }
    }
    return false;
}

// ── System callbacks ──────────────────────────────────────────────────────

void VulkanAzgaarPropsPass::added() {
    utils::signalSubscribe("swapchainCreated", swapchainCreated);
    recreatePipelines();
}

void VulkanAzgaarPropsPass::preUpdate() {
    // Retire IBOs that are past their FRAMES_IN_FLIGHT lifetime (runs even on
    // skipped frames so the list stays bounded).
    retireFlush();
    if (vulkan.skipFrame) return;
    applyPendingTiles();
    drainGlobalQueue(&pendingGlobals, &gpuGlobal, "azgaar_props_inst_global");
    drainGlobalQueue(&pendingLandmarks, &gpuLandmarks, "azgaar_props_inst_landmarks");
}

// Find the PropVariantRange row for a (species, variant) pair.  The table is
// small (a few dozen rows), so a linear scan per range is cheap.  The table is
// stable once set (only replaced at world teardown), so it is read directly on
// the render thread.
static const PropVariantRange* findVariant(u32 species, u32 variant) {
    for (u32 i = 0; i < variants.size(); i++) {
        if (variants[i].species == species && variants[i].variant == variant) {
            return &variants[i];
        }
    }
    return nullptr;
}

// Draw one whole-map instance set (settlement buildings, landmarks), culled by
// the map AABB the game supplied.  Shared by the two global slots and by the
// colour / pre-pass / shadow draws (`pipe` selects the pipeline receiving the
// push constants; `forShadow` switches to the cascade-indexed PC block).
static void drawGlobalSet(VulkanCommand* cmd, PropGpuGlobal* g, const vec4* planes,
                          bool* loggedOnce, const char* label,
                          VulkanPipe* pipe, bool forShadow, u32 cascadeIndex) {
    utils::threadLock(&uploadLock);
    bool draw = g->inUse && g->instanceCount > 0;
    vec3 gmin = {g->aabbMin[0], g->aabbMin[1], g->aabbMin[2]};
    vec3 gmax = {g->aabbMax[0], g->aabbMax[1], g->aabbMax[2]};
    utils::threadUnlock(&uploadLock);
    if (!draw) return;

    bool culled = aabbOutsideFrustum(gmin, gmax, (vec4*)planes);
    if (culled) return;

    if (!*loggedOnce) {
        *loggedOnce = true;
        utils::info("azgaar_props: %s: %u instances in %u ranges (AABB %.0f..%.0f x, %.0f..%.0f y, %.0f..%.0f z)",
             label, g->instanceCount, g->rangeCount,
             (double)gmin[0], (double)gmax[0],
             (double)gmin[1], (double)gmax[1],
             (double)gmin[2], (double)gmax[2]);
    }
    for (u32 r = 0u; r < g->rangeCount; r++) {
        PropTileRange* range = &g->ranges[r];
        if (range->count == 0) continue;
        const PropVariantRange* v = findVariant(range->species, range->variant);
        if (!v || v->indexCount == 0) continue;

        vulkanBindVertex(cmd, &meshVbo, 0, &g->ibo,
                         (u64)range->start * sizeof(PropInstance), NULL, 0);
        vulkanBindIndex(cmd, &meshIbo, (u64)v->indexOffset * sizeof(u32),
                         VK_INDEX_TYPE_UINT32);

        if (!forShadow) {
            PropPushConstants pc = {
                .boundsMin  = {v->boundsMin[0], v->boundsMin[1], v->boundsMin[2], 0.0f},
                .boundsMax  = {v->boundsMax[0], v->boundsMax[1], v->boundsMax[2], 0.0f},
                .swayFactor = v->swayFactor,
                .lodRole    = (float)v->lodRole,
            };
            vulkanPush(cmd, pipe, sizeof(pc), &pc);
        } else {
            PropShadowPushConstants spc = {
                .boundsMin    = {v->boundsMin[0], v->boundsMin[1], v->boundsMin[2], 0.0f},
                .boundsMax    = {v->boundsMax[0], v->boundsMax[1], v->boundsMax[2], 0.0f},
                .swayFactor   = v->swayFactor,
                .lodRole      = (float)v->lodRole,
                .cascadeIndex = cascadeIndex,
            };
            vulkanPush(cmd, pipe, sizeof(spc), &spc);
        }

        vkCmdDrawIndexed(cmd->cmd, v->indexCount, (u32)range->count, 0, 0, 0);
        renderer.drawCalls++;
        renderer.instanceCount += range->count;
        renderer.triangleCount += (v->indexCount / 3u) * range->count;
    }
}

void VulkanAzgaarPropsPass::update() {
    if (vulkan.skipFrame) return;
    if (!enabled) return;

    utils::threadLock(&uploadLock);
    bool hasMesh = meshVbo.buf && meshIbo.buf && meshVertCount > 0 && meshIdxCount > 0;
    bool hasVariants = !variants.empty();
    u32  tileCount  = static_cast<i32>(gpuTiles.size());
    bool hasGlobal  = gpuGlobal.inUse && gpuGlobal.instanceCount > 0;
    bool hasLandmarks = gpuLandmarks.inUse && gpuLandmarks.instanceCount > 0;
    float tileYMin   = tileAabbYMin, tileYMax = tileAabbYMax;  // per-tile cull AABB Y range
    utils::threadUnlock(&uploadLock);

    if (!hasMesh || !hasVariants || (tileCount == 0 && !hasGlobal && !hasLandmarks)) return;

    Entity* camEntity = cameraGetEntity();
    Camera* cam       = camEntity ? getComponent(camEntity->scene, Camera, camEntity->id) : NULL;
    if (!cam) return;

    VulkanImage* sceneColor     = vulkanFrameResourcesGetSceneColor();
    VulkanImage* normals        = vulkanFrameResourcesGetNormals();
    VulkanImage* material       = vulkanFrameResourcesGetMaterial();
    VulkanImage* depthImage     = vulkanFrameResourcesGetDepth();
    if (!sceneColor || !normals || !material || !depthImage) return;

    const vec4* planes = (const vec4*)cam->cameraUbo.frustumPlanes;

    VulkanCommand* cmd = vulkan.currentCmd;
    if (!cmd) return;

    vulkanBeginRender(.cmd     = cmd,
                      .pipe    = &pipe,
                      .color1  = sceneColor,
                      .color2  = normals,
                      .color3  = material,
                      .depth   = depthImage);

    vulkanViewport(cmd, 0, sceneColor->extent.height, sceneColor->extent.width,
                   -((i32)sceneColor->extent.height));
    vulkanScissor(cmd, 0, 0, sceneColor->extent.width, sceneColor->extent.height);

    vulkanBindPipe(cmd, &pipe);
    vulkanBindVertex(cmd, &meshVbo, 0, NULL, 0, NULL, 0);

    // Snapshot the tile table (the render thread owns it here; applyPending ran
    // in preUpdate, so no concurrent mutation).  We read it directly.
    for (u32 t = 0u; t < tileCount; t++) {
        PropGpuTile* e = &gpuTiles[t];
        if (!e->inUse || e->instanceCount == 0) continue;

        // Tile world AABB (y range is loose; props are small).
        float half = 64.0f; // prop reach beyond the tile origin (trees/rocks)
        vec3 bmin = {e->tileX * 2048.0f - half, tileYMin, e->tileZ * 2048.0f - half};
        vec3 bmax = {bmin[0] + 2048.0f + 2.0f * half, tileYMax, bmin[2] + 2048.0f + 2.0f * half};
        if (aabbOutsideFrustum(bmin, bmax, (vec4*)planes)) continue;

        for (u32 r = 0u; r < e->rangeCount; r++) {
            PropTileRange* range = &e->ranges[r];
            if (range->count == 0) continue;
            const PropVariantRange* v = findVariant(range->species, range->variant);
            if (!v || v->indexCount == 0) continue;

            // Bind the instance buffer at this range's offset; the merged mesh
            // index range selects the (species, variant) geometry.
            vulkanBindVertex(cmd, &meshVbo, 0, &e->ibo,
                             (u64)range->start * sizeof(PropInstance), NULL, 0);
            vulkanBindIndex(cmd, &meshIbo, (u64)v->indexOffset * sizeof(u32),
                            VK_INDEX_TYPE_UINT32);

            PropPushConstants pc = {
                .boundsMin  = {v->boundsMin[0], v->boundsMin[1], v->boundsMin[2], 0.0f},
                .boundsMax  = {v->boundsMax[0], v->boundsMax[1], v->boundsMax[2], 0.0f},
                .swayFactor = v->swayFactor,
                .lodRole    = (float)v->lodRole,
            };
            vulkanPush(cmd, &pipe, sizeof(pc), &pc);

            vkCmdDrawIndexed(cmd->cmd, v->indexCount, (u32)range->count, 0, 0, 0);
            renderer.drawCalls++;
            renderer.instanceCount += range->count;
            renderer.triangleCount += (v->indexCount / 3u) * range->count;
        }
    }

    // Whole-map sets (workstream D + E): one instance buffer each, culled by
    // the map AABB the game supplied.
    static bool globalLogged = false;
    static bool landmarksLogged = false;
    drawGlobalSet(cmd, &gpuGlobal, planes, &globalLogged, "global settlement buildings",
                  &pipe, false, 0);
    drawGlobalSet(cmd, &gpuLandmarks, planes, &landmarksLogged, "landmarks",
                  &pipe, false, 0);

    vulkanEndRender(cmd);
}

// ── Shadow (CSM) hook (called by VulkanShadowPass once per cascade) ───────
// Draws every frustum-visible instance range into the sun shadow map with the
// same transform logic as the colour pass (LOD hard switch + wind sway),
// projected by the cascade's light view-projection.  Runs INSIDE the shadow
// pass' render pass, so this must not begin/end a render or change viewports.
void vulkanAzgaarPropsDrawShadow(VulkanCommand* cmd, u32 cascadeIndex) {
    if (!enabled || !cmd) return;

    utils::threadLock(&uploadLock);
    bool hasMesh = meshVbo.buf && meshIbo.buf && meshVertCount > 0 && meshIdxCount > 0;
    bool hasVariants = !variants.empty();
    u32  tileCount  = static_cast<i32>(gpuTiles.size());
    bool hasGlobal  = gpuGlobal.inUse && gpuGlobal.instanceCount > 0;
    bool hasLandmarks = gpuLandmarks.inUse && gpuLandmarks.instanceCount > 0;
    float tileYMin   = tileAabbYMin, tileYMax = tileAabbYMax;  // per-tile cull AABB Y range
    utils::threadUnlock(&uploadLock);

    if (!hasMesh || !hasVariants || (tileCount == 0 && !hasGlobal && !hasLandmarks)) return;

    Entity* camEntity = cameraGetEntity();
    Camera* cam       = camEntity ? getComponent(camEntity->scene, Camera, camEntity->id) : NULL;
    if (!cam) return;

    const vec4* planes = (const vec4*)cam->cameraUbo.frustumPlanes;

    vulkanBindPipe(cmd, &shadowPipe);
    vulkanBindVertex(cmd, &meshVbo, 0, NULL, 0, NULL, 0);

    for (u32 t = 0u; t < tileCount; t++) {
        PropGpuTile* e = &gpuTiles[t];
        if (!e->inUse || e->instanceCount == 0) continue;

        float half = 64.0f;
        vec3 bmin = {e->tileX * 2048.0f - half, tileYMin, e->tileZ * 2048.0f - half};
        vec3 bmax = {bmin[0] + 2048.0f + 2.0f * half, tileYMax, bmin[2] + 2048.0f + 2.0f * half};
        if (aabbOutsideFrustum(bmin, bmax, (vec4*)planes)) continue;

        for (u32 r = 0u; r < e->rangeCount; r++) {
            PropTileRange* range = &e->ranges[r];
            if (range->count == 0) continue;
            const PropVariantRange* v = findVariant(range->species, range->variant);
            if (!v || v->indexCount == 0) continue;

            vulkanBindVertex(cmd, &meshVbo, 0, &e->ibo,
                             (u64)range->start * sizeof(PropInstance), NULL, 0);
            vulkanBindIndex(cmd, &meshIbo, (u64)v->indexOffset * sizeof(u32),
                            VK_INDEX_TYPE_UINT32);

            PropShadowPushConstants pc = {
                .boundsMin    = {v->boundsMin[0], v->boundsMin[1], v->boundsMin[2], 0.0f},
                .boundsMax    = {v->boundsMax[0], v->boundsMax[1], v->boundsMax[2], 0.0f},
                .swayFactor   = v->swayFactor,
                .lodRole      = (float)v->lodRole,
                .cascadeIndex = cascadeIndex,
            };
            vulkanPush(cmd, &shadowPipe, sizeof(pc), &pc);

            vkCmdDrawIndexed(cmd->cmd, v->indexCount, (u32)range->count, 0, 0, 0);
            renderer.drawCalls++;
            renderer.instanceCount += range->count;
            renderer.triangleCount += (v->indexCount / 3u) * range->count;
        }
    }

    static bool shadowGlobalLogged = false;
    static bool shadowLandmarksLogged = false;
    drawGlobalSet(cmd, &gpuGlobal, planes, &shadowGlobalLogged,
                  "shadow: global settlement buildings", &shadowPipe, true, cascadeIndex);
    drawGlobalSet(cmd, &gpuLandmarks, planes, &shadowLandmarksLogged, "shadow: landmarks",
                  &shadowPipe, true, cascadeIndex);
}

// ── Depth/velocity pre-pass hook (called by VulkanDepthPass) ─────────────

// Drawn INSIDE the depth pass' own render pass (same velocity / view-normal /
// depth attachments), so it binds its pipe and draws — it must never begin a
// render of its own: a nested vkCmdBeginRendering leaves the caller's
// vkCmdEndRendering orphaned, which faults the NVIDIA driver (SIGSEGV in
// libnvidia-glcore.so on vkCmdEndRendering). Same idiom as the heightmap
// terrain prepass, the water prepass and the shadow pass above.
void vulkanAzgaarPropsDrawPrepass(void) {
    utils::threadLock(&uploadLock);
    bool hasMesh = meshVbo.buf && meshIbo.buf && meshVertCount > 0 && meshIdxCount > 0;
    bool hasVariants = !variants.empty();
    u32  tileCount  = static_cast<i32>(gpuTiles.size());
    bool hasGlobal  = gpuGlobal.inUse && gpuGlobal.instanceCount > 0;
    bool hasLandmarks = gpuLandmarks.inUse && gpuLandmarks.instanceCount > 0;
    float tileYMin   = tileAabbYMin, tileYMax = tileAabbYMax;  // per-tile cull AABB Y range
    utils::threadUnlock(&uploadLock);

    if (!hasMesh || !hasVariants || (tileCount == 0 && !hasGlobal && !hasLandmarks)) return;

    Entity* camEntity = cameraGetEntity();
    Camera* cam       = camEntity ? getComponent(camEntity->scene, Camera, camEntity->id) : NULL;
    if (!cam) return;

    VulkanImage* velocityImage   = vulkanFrameResourcesGetVelocity();
    VulkanImage* viewNormalImage = vulkanFrameResourcesGetViewNormal();
    VulkanImage* depthImage      = vulkanFrameResourcesGetDepth();
    if (!velocityImage || !viewNormalImage || !depthImage) return;

    VulkanCommand* cmd = vulkan.currentCmd;
    if (!cmd) return;

    // Attachments are already owned by the depth pass' active render pass;
    // just (re)state the viewport for the draws below.
    vulkanViewport(cmd, 0, velocityImage->extent.height, velocityImage->extent.width,
                   -((i32)velocityImage->extent.height));
    vulkanScissor(cmd, 0, 0, velocityImage->extent.width, velocityImage->extent.height);

    const vec4* planes = (const vec4*)cam->cameraUbo.frustumPlanes;

    vulkanBindPipe(cmd, &prepassPipe);
    vulkanBindVertex(cmd, &meshVbo, 0, NULL, 0, NULL, 0);

    for (u32 t = 0u; t < tileCount; t++) {
        PropGpuTile* e = &gpuTiles[t];
        if (!e->inUse || e->instanceCount == 0) continue;

        float half = 64.0f;
        vec3 bmin = {e->tileX * 2048.0f - half, tileYMin, e->tileZ * 2048.0f - half};
        vec3 bmax = {bmin[0] + 2048.0f + 2.0f * half, tileYMax, bmin[2] + 2048.0f + 2.0f * half};
        if (aabbOutsideFrustum(bmin, bmax, (vec4*)planes)) continue;

        for (u32 r = 0u; r < e->rangeCount; r++) {
            PropTileRange* range = &e->ranges[r];
            if (range->count == 0) continue;
            const PropVariantRange* v = findVariant(range->species, range->variant);
            if (!v || v->indexCount == 0) continue;

            vulkanBindVertex(cmd, &meshVbo, 0, &e->ibo,
                             (u64)range->start * sizeof(PropInstance), NULL, 0);
            vulkanBindIndex(cmd, &meshIbo, (u64)v->indexOffset * sizeof(u32),
                            VK_INDEX_TYPE_UINT32);

            PropPushConstants pc = {
                .boundsMin  = {v->boundsMin[0], v->boundsMin[1], v->boundsMin[2], 0.0f},
                .boundsMax  = {v->boundsMax[0], v->boundsMax[1], v->boundsMax[2], 0.0f},
                .swayFactor = v->swayFactor,
                .lodRole    = (float)v->lodRole,
            };
            vulkanPush(cmd, &prepassPipe, sizeof(pc), &pc);

            vkCmdDrawIndexed(cmd->cmd, v->indexCount, (u32)range->count, 0, 0, 0);
            renderer.drawCalls++;
            renderer.instanceCount += range->count;
            renderer.triangleCount += (v->indexCount / 3u) * range->count;
        }
    }

    static bool prepassGlobalLogged = false;
    static bool prepassLandmarksLogged = false;
    drawGlobalSet(cmd, &gpuGlobal, planes, &prepassGlobalLogged, "prepass global",
                  &prepassPipe, false, 0);
    drawGlobalSet(cmd, &gpuLandmarks, planes, &prepassLandmarksLogged, "prepass landmarks",
                  &prepassPipe, false, 0);
}

void VulkanAzgaarPropsPass::removed() {
    vulkanAzgaarPropsClearAll();
    utils::threadLock(&uploadLock);
    if (meshVbo.buf) vulkanDestroyBuffer(&meshVbo, VK_NULL_HANDLE);
    if (meshIbo.buf) vulkanDestroyBuffer(&meshIbo, VK_NULL_HANDLE);
    meshVbo = VulkanBuffer{};
    meshIbo = VulkanBuffer{};
    for (u32 i = 0; i < gpuTiles.size(); i++) {
        if (gpuTiles[i].inUse) gpuTileDestroy(&gpuTiles[i]);
    }
    // Pending entries carry pre-built instance buffers (never drained after
    // removal) — destroy them here.  Async uploads hand their transient
    // command to the garbage collector; the final cleanup pass below reaps
    // those (and their staging buffers) before the device is destroyed.
    while (!pendingTiles.empty()) {
        if (pendingTiles[0].cmd) vulkanTransientFinish(pendingTiles[0].cmd);
        if (pendingTiles[0].ibo.buf) vulkanDestroyBuffer(&pendingTiles[0].ibo, VK_NULL_HANDLE);
        pendingTiles.erase(pendingTiles.begin() + 0);
    }
    gpuSetDestroy(&gpuGlobal);
    while (!pendingGlobals.empty()) {
        if (pendingGlobals[0].ibo.buf) vulkanDestroyBuffer(&pendingGlobals[0].ibo, VK_NULL_HANDLE);
        pendingGlobals.erase(pendingGlobals.begin() + 0);
    }
    gpuSetDestroy(&gpuLandmarks);
    while (!pendingLandmarks.empty()) {
        if (pendingLandmarks[0].ibo.buf)
            vulkanDestroyBuffer(&pendingLandmarks[0].ibo, VK_NULL_HANDLE);
        pendingLandmarks.erase(pendingLandmarks.begin() + 0);
    }
    while (!retiredIbos.empty()) {
        vulkanDestroyBuffer(&retiredIbos[0], VK_NULL_HANDLE);
        retiredIbos.erase(retiredIbos.begin() + 0);
    }
    variants.clear();
    utils::threadUnlock(&uploadLock);
    // Reap the garbage registered above (finished transient commands, their
    // staging buffers, retired IBOs): the engine's teardown cleanup may not
    // run again before vkDestroyDevice.
    vulkanCleanupGarbage();
    vulkanDestroyPipe(&pipe);
    vulkanDestroyPipe(&prepassPipe);
    vulkanDestroyPipe(&shadowPipe);
}}  // namespace engine
