#include "renderer/vulkan/pass/heightmap_terrain/VulkanHeightmapTerrainPass.h"
#include "events/Events.h"
#include "renderer/Renderer.h"
#include "renderer/texture/TextureManager.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanBuffer.h"
#include "renderer/vulkan/resources/VulkanDesc.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/utils/VulkanUtils.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/heightmap/HeightmapTerrain.h"
#include "ecs/system/transform/TransformComponent.h"

namespace engine {

VulkanHeightmapTerrainPass vulkanHeightmapTerrainPass;

VulkanHeightmapTerrainPass::VulkanHeightmapTerrainPass() : System("heightmap_terrain") {}

// ── Push constants ────────────────────────────────────────────────────────
// Layout must match the GLSL `HeightmapPC` (tile vec4, flags vec4).
typedef struct HeightmapTerrainPushConstants {
    float tileOriginX;
    float tileOriginZ;
    float tileSizeMeters;
    float gridSegments;
    float heightScale; // 1.0 (heights are already final metres)
    float texDim;      // HEIGHTMAP_TEX (512)
    float wireFrame;   // 0/1 (scene pipe only)
    float debugHeightRamp; // 0/1 (scene pipe only)
} HeightmapTerrainPushConstants;

// ── State ─────────────────────────────────────────────────────────────────

static VulkanPipe scenePipe;
static VulkanPipe sceneWireFramePipe;
static VulkanPipe prepassPipe;

// Descriptor-set layout used by the pipeline layouts (set1 = height).
// Per-tile descriptor sets are created separately with an identical structure
// and bound at draw time.
static VulkanDesc layoutHeightDesc;

static bool wireFrameEnabled       = false;
static bool debugHeightRampEnabled = false;
static bool terrainDefaultsSet     = false;

#define HEIGHTMAP_PASS_UPLOADS_PER_FRAME 3 // GPU uploads budgeted per frame

// Heights stay f32 end to end (CPU grid -> R32F texture): R32F linear
// filtering is guaranteed by core Vulkan, so there is no driver format
// probing and no f16 conversion.  Costs 1 MB/tile vs 512 KB for R16F —
// accepted (the 5x5 window's VRAM budget covers it).
static const VkFormat heightFormat = VK_FORMAT_R32_SFLOAT;

// Per-tile GPU data (height texture + descriptor set). Managed on the
// main renderer thread; the CPU side (grids) lives in HeightmapTerrain.
typedef struct HeightmapGpuTile {
    bool        inUse;
    i32         tileX, tileZ;
    u64         readyStamp;
    VulkanImage heightTex;
    VulkanDesc  heightDesc;
} HeightmapGpuTile;

static std::vector<HeightmapGpuTile> gpuTiles;
static HeightmapTerrain*       cachedHt = NULL;

// Descriptor pools of evicted tiles may still be referenced by in-flight
// command buffers, so destruction is deferred a few frames.
typedef struct DeferredDescDestroy {
    VulkanDesc desc;
    u32        framesLeft;
} DeferredDescDestroy;

static std::vector<DeferredDescDestroy> deferredDescs;

// ── Public API ────────────────────────────────────────────────────────────

bool vulkanHeightmapTerrainIsWireFrameEnabled(void) {
    return wireFrameEnabled;
}

void vulkanHeightmapTerrainSetWireFrameEnabled(bool enabled) {
    wireFrameEnabled = enabled;
}

bool vulkanHeightmapTerrainIsDebugHeightRampEnabled(void) {
    return debugHeightRampEnabled;
}

void vulkanHeightmapTerrainSetDebugHeightRampEnabled(bool enabled) {
    debugHeightRampEnabled = enabled;
}

// ── Helpers ───────────────────────────────────────────────────────────────

static bool aabbOutsideFrustum(const vec3 min, const vec3 max, vec4* planes) {
    for (i32 i = 0; i < 6; i++) {
        vec4 p = {planes[i][0], planes[i][1], planes[i][2], planes[i][3]};
        vec3 positive = {
            p[0] >= 0.0f ? max[0] : min[0],
            p[1] >= 0.0f ? max[1] : min[1],
            p[2] >= 0.0f ? max[2] : min[2],
        };
        if ((p[0] * positive[0]) + (p[1] * positive[1]) + (p[2] * positive[2]) + p[3] < 0.0f) return true;
    }
    return false;
}

static void setTerrainDefaults(void) {
    if (terrainDefaultsSet) return;

    Texture* grassAlbedo = getTextureByName("images/terrain/grass_default/albedo.ktx2");
    Texture* grassNormal = getTextureByName("images/terrain/grass_default/normal.ktx2");
    Texture* cliffAlbedo = getTextureByName("images/terrain/cliff_side_default/albedo.ktx2");
    Texture* cliffNormal = getTextureByName("images/terrain/cliff_side_default/normal.ktx2");

    vulkanResourceSetTerrainDefaults(grassAlbedo ? grassAlbedo->id : 0,
                                     grassNormal ? grassNormal->id : 0,
                                     cliffAlbedo ? cliffAlbedo->id : 0,
                                     cliffNormal ? cliffNormal->id : 0);

    // Climate blend textures (workstream A).  The per-world biome colour /
    // climate textures are registered by the game at world load; the shared
    // snow / sand default albedos live in the engine pak and never change.
    Texture* snowAlbedo = getTextureByName("images/terrain/snow_default/albedo.ktx2");
    Texture* sandAlbedo = getTextureByName("images/terrain/sand_default/albedo.ktx2");
    vulkanResourceSetTerrainSnowSand(snowAlbedo ? snowAlbedo->id : 0,
                                     sandAlbedo ? sandAlbedo->id : 0);

    terrainDefaultsSet = true;
}

// ── Pipeline management ───────────────────────────────────────────────────

static void recreatePipelines(void) {
    if (scenePipe.pipe) vulkanDestroyPipe(&scenePipe);
    if (sceneWireFramePipe.pipe) vulkanDestroyPipe(&sceneWireFramePipe);
    if (prepassPipe.pipe) vulkanDestroyPipe(&prepassPipe);

    if (layoutHeightDesc.set) vulkanDestroyDesc(&layoutHeightDesc);
    layoutHeightDesc = vulkanCreateDesc(.name = "heightmap_layout_height", .combinedImageSamplers = 1);

    scenePipe = vulkanCreatePipe(
        .name                 = "heightmap_terrain",
        .vs                   = "shaders/pass/heightmap_terrain/spv/heightmap_terrain.vert.spv",
        .fs                   = "shaders/pass/heightmap_terrain/spv/heightmap_terrain.frag.spv",
        .set1                 = &layoutHeightDesc,
        .colorFormat1         = VK_FORMAT_R16G16B16A16_SFLOAT,
        .colorFormat2         = VK_FORMAT_R16G16_SFLOAT,
        .colorFormat3         = VK_FORMAT_R8G8B8A8_UNORM,
        .depthFormat          = VK_FORMAT_D32_SFLOAT,
        .clearColor1          = {0, 0, 0, 0}, .clearColor1Enabled = 1,
        .clearColor2          = {0, 0, 0, 0}, .clearColor2Enabled = 1,
        .clearColor3          = {0, 0, 0, 0}, .clearColor3Enabled = 1);

    sceneWireFramePipe = vulkanCreatePipe(
        .name                 = "heightmap_terrain_wireframe",
        .vs                   = "shaders/pass/heightmap_terrain/spv/heightmap_terrain.vert.spv",
        .fs                   = "shaders/pass/heightmap_terrain/spv/heightmap_terrain.frag.spv",
        .set1                 = &layoutHeightDesc,
        .colorFormat1         = VK_FORMAT_R16G16B16A16_SFLOAT,
        .colorFormat2         = VK_FORMAT_R16G16_SFLOAT,
        .colorFormat3         = VK_FORMAT_R8G8B8A8_UNORM,
        .depthFormat          = VK_FORMAT_D32_SFLOAT,
        .wireFrame            = 1,
        .clearColor1          = {0, 0, 0, 0}, .clearColor1Enabled = 0,
        .clearColor2          = {0, 0, 0, 0}, .clearColor2Enabled = 0,
        .clearColor3          = {0, 0, 0, 0}, .clearColor3Enabled = 0);

    // Depth/velocity pre-pass pipe: same lattice, writes depth + velocity +
    // view-normal XY (called inside VulkanDepthPass' render pass).
    prepassPipe = vulkanCreatePipe(
        .name               = "heightmap_terrain_depth_prepass",
        .vs                 = "shaders/pass/heightmap_terrain/spv/heightmap_terrain_depth.vert.spv",
        .fs                 = "shaders/pass/heightmap_terrain/spv/heightmap_terrain_depth.frag.spv",
        .set1               = &layoutHeightDesc,
        .colorFormat1       = VK_FORMAT_R16G16_SFLOAT,
        .colorFormat2       = VK_FORMAT_R16G16_SNORM,
        .depthFormat        = VK_FORMAT_D32_SFLOAT);
}

static void swapchainCreated(void*) {
    recreatePipelines();
}

// ── GPU tile cache ────────────────────────────────────────────────────────

static void heightmapGpuTileDestroy(HeightmapGpuTile* e) {
    if (e->heightTex.img) vulkanDestroyImage(&e->heightTex, VK_NULL_HANDLE);
    if (e->heightDesc.set) {
        deferredDescs.push_back((DeferredDescDestroy{.desc = e->heightDesc, .framesLeft = 3}));
        e->heightDesc = VulkanDesc{};
    }
    *e = HeightmapGpuTile{};
}

static void heightmapPassReset(void) {
    for (u32 i = 0; i < gpuTiles.size(); i++) {
        if (gpuTiles[i].inUse) heightmapGpuTileDestroy(&gpuTiles[i]);
    }
    gpuTiles.clear();
}

// Upload one tile's CPU height grid to the GPU (R32F) and (re)bind the
// entry's descriptor set. The CPU height grid is f32 and the texture is R32F,
// so the grid pointer is copied straight to the image (no staging buffer of
// our own, no conversion). The transient command's fence is waited on, so
// the texture is complete before the scene pass samples it.
static bool heightmapPassUploadTile(HeightmapGpuTile* e, const HeightmapTileView* v) {
    static int hitchOn = -1;
    if (hitchOn < 0) hitchOn = getenv("ENGINE_HITCH_DEBUG") != NULL;
    double hitchT0 = utils::nanos();

    const u32   tex = HEIGHTMAP_TEX;
    const size_t n  = (size_t)tex * tex;

    VulkanImage heightImg = vulkanCreateImage(
        .name   = utils::strtmp("heightmap_height_%d_%d", v->tileX, v->tileZ),
        .format = heightFormat,
        .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .width  = (int)tex,
        .height = (int)tex,
        .noPool = 1);
    if (!heightImg.img) {
        utils::warn("heightmapTerrain: height image creation failed tile(%d,%d)", v->tileX, v->tileZ);
        return false;
    }

    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanTransition(cmd, &heightImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);
    vulkanCopy(.cmd = cmd,
               .source.data = (void*)v->heights,
               .target.img  = &heightImg,
               .size        = (u32)(n * sizeof(float)));
    vulkanTransition(cmd, &heightImg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    {
        double te0 = utils::nanos();
        vulkanTransientEnd(cmd, 1);
        if (hitchOn) utils::info("HITCH: heightmap upload tile(%d,%d) total=%.1f ms (transient+fence=%.1f ms)",
                          v->tileX, v->tileZ, (utils::nanos() - hitchT0) / 1e6, (utils::nanos() - te0) / 1e6);
    }

    if (!e->heightDesc.set) {
        e->heightDesc = vulkanCreateDesc(.name = "heightmap_height_set", .combinedImageSamplers = 1);
    }
    vulkanUpdateDesc(&e->heightDesc, VULKAN_BINDING_COMBINED_IMAGE_SAMPLER, &heightImg, 0, 0);

    e->heightTex    = heightImg;
    e->tileX        = v->tileX;
    e->tileZ        = v->tileZ;
    e->readyStamp   = v->readyStamp;
    e->inUse        = true;
    return true;
}

static bool gpuTileHasView(const HeightmapGpuTile* e, const HeightmapTileView* v) {
    return e->inUse && e->tileX == v->tileX && e->tileZ == v->tileZ && e->readyStamp == v->readyStamp;
}

static bool gpuTileMatchesAnyView(const HeightmapGpuTile* e, const HeightmapTileView* views, u32 count) {
    for (u32 j = 0; j < count; j++) {
        if (e->inUse && e->tileX == views[j].tileX && e->tileZ == views[j].tileZ &&
            e->readyStamp == views[j].readyStamp) {
            return true;
        }
    }
    return false;
}

static HeightmapGpuTile* gpuTileAcquireFree(void) {
    for (u32 i = 0; i < gpuTiles.size(); i++) {
        if (!gpuTiles[i].inUse) return &gpuTiles[i];
    }
    return NULL;
}

// ── Frame entry: cache maintenance + budgeted uploads ────────────────────

void VulkanHeightmapTerrainPass::preUpdate() {
    if (vulkan.skipFrame) return;

    // Tick deferred descriptor destruction (pools must survive in-flight
    // command buffers; 3 frames is well past the GPU queue depth).
    for (i32 i = (i32)static_cast<i32>(deferredDescs.size()) - 1; i >= 0; i--) {
        if (deferredDescs[i].framesLeft > 1) {
            deferredDescs[i].framesLeft--;
        } else {
            vulkanDestroyDesc(&deferredDescs[i].desc);
            deferredDescs[(u32)i] = deferredDescs.back();
            deferredDescs.pop_back();
        }
    }

    HeightmapTerrain* ht = heightmapTerrainGetActive();

    if (cachedHt && ht != cachedHt) {
        heightmapPassReset();
        cachedHt = NULL;
    }
    if (!ht || !ht->initialized) {
        if (cachedHt) {
            heightmapPassReset();
            cachedHt = NULL;
        }
        return;
    }

    if (cachedHt != ht) {
        cachedHt  = ht;
        u32 cap   = ht->windowSize * ht->windowSize;
        if (gpuTiles.size() < cap) {
            while (gpuTiles.size() < cap) gpuTiles.push_back(HeightmapGpuTile{});
        }
    }

    if (!scenePipe.pipe) recreatePipelines();
    setTerrainDefaults();

    u32 snapCap = static_cast<i32>(gpuTiles.size());
    if (snapCap == 0) return;
    std::vector<HeightmapTileView> views(snapCap);
    u32 viewCount           = heightmapTerrainSnapshotTiles(ht, views.data(), snapCap);
    if (viewCount == 0) {
        return;
    }

    // 1) Drop cache entries whose tile left the window or was regenerated.
    for (u32 i = 0; i < gpuTiles.size(); i++) {
        if (gpuTiles[i].inUse && !gpuTileMatchesAnyView(&gpuTiles[i], views.data(), viewCount)) {
            heightmapGpuTileDestroy(&gpuTiles[i]);
        }
    }

    // 2) Upload tiles the cache is missing, nearest to the camera first so
    // the visible ring fills before the distant window edge.
    Entity* cameraEntity = cameraGetEntity();
    float anchorX = 0.0f, anchorZ = 0.0f;
    if (cameraEntity) {
        Transform* t = getComponent(cameraEntity->scene, Transform, cameraEntity->id);
        if (t) {
            anchorX = t->pos[0];
            anchorZ = t->pos[2];
        }
    }
    i32 anchorTileX = (i32)floorf(anchorX / ht->tileSizeMeters);
    i32 anchorTileZ = (i32)floorf(anchorZ / ht->tileSizeMeters);

    // Stable insertion sort by (Manhattan ring, view order); n <= 25.
    for (u32 i = 1; i < viewCount; i++) {
        HeightmapTileView key = views[i];
        i32 kdx = key.tileX - anchorTileX;
        if (kdx < 0) kdx = -kdx;
        i32 kdz = key.tileZ - anchorTileZ;
        if (kdz < 0) kdz = -kdz;
        i32 kx = kdx + kdz;
        i32 j  = (i32)i - 1;
        for (; j >= 0; j--) {
            i32 dx = views[j].tileX - anchorTileX;
            if (dx < 0) dx = -dx;
            i32 dz = views[j].tileZ - anchorTileZ;
            if (dz < 0) dz = -dz;
            if (dx + dz <= kx) break;
            views[j + 1] = views[j];
        }
        views[j + 1] = key;
    }

    u32 budget = HEIGHTMAP_PASS_UPLOADS_PER_FRAME;
    for (u32 j = 0; j < viewCount && budget > 0; j++) {
        bool        have    = false;
        for (u32 i = 0; i < gpuTiles.size(); i++) {
            if (gpuTileHasView(&gpuTiles[i], &views[j])) {
                have = true;
                break;
            }
        }
        if (have) continue;

        HeightmapGpuTile* e = gpuTileAcquireFree();
        if (!e) break; // pool exhausted (shouldn't happen: cap = window^2)
        if (heightmapPassUploadTile(e, &views[j])) {
            budget--;
            utils::info("heightmapTerrain: uploaded GPU textures for tile(%d,%d) stamp=%llu",
                 views[j].tileX,
                 views[j].tileZ,
                 (unsigned long long)views[j].readyStamp);
        } else {
            // Keep retrying next frame; do not consume budget.
        }
    }

    }

// ── Scene pass ────────────────────────────────────────────────────────────

// Uniform tessellation: every visible tile is drawn with the same
// 255-segment lattice — exactly the physics heightfield's sample spacing
// (256 samples over 255 intervals = 8.03 m cells on a 2048 m tile) — so the
// rendered surface is the SAME bilinear surface the character controller
// walks on at every distance, and adjacent tiles emit bit-identical border
// vertices (same lattice rate + shared border texels): watertight, no
// cracks, no LOD pinning, no transition meshes.
//
// NOTE: a per-ring segment ladder (255/64/32) cracks at every ring
// boundary. Both tiles sample the same continuous height surface, but
// their border POLYLINES — chords over 8 m vs 32/64 m spans — are different
// curves, and the wedge between them shows up as a gap (T-junction crack).
// Making that watertight needs per-edge graded tessellation matched to each
// neighbour's ring; until then the uniform lattice trades vertex throughput
// (25 x ~130 k tris) for a seam-free surface.
static i32 heightmapRingSegments(i32 ring) {
    (void)ring;
    return 255;
}

static void heightmapPassDrawTiles(VulkanCommand* cmd,
                                   VulkanPipe*      pipe,
                                   const HeightmapTerrain* ht,
                                   const Camera* camera,
                                   i32 camTileX,
                                   i32 camTileZ,
                                   char wireFrame,
                                   char debugRamp) {
    const vec4* planes = (const vec4*)camera->cameraUbo.frustumPlanes;

    vulkanBindPipe(cmd, pipe);
    for (u32 i = 0; i < gpuTiles.size(); i++) {
        HeightmapGpuTile* e = &gpuTiles[i];
        if (!e->inUse) continue;

        i32 dx = e->tileX - camTileX;
        if (dx < 0) dx = -dx;
        i32 dz = e->tileZ - camTileZ;
        if (dz < 0) dz = -dz;
        i32 ring = dx > dz ? dx : dz;
        if (ring > 2) continue;

        i32 seg = heightmapRingSegments(ring);

        vec3 bmin = {e->tileX * ht->tileSizeMeters, -80.0f, e->tileZ * ht->tileSizeMeters};
        vec3 bmax = {bmin[0] + ht->tileSizeMeters, 2560.0f, bmin[2] + ht->tileSizeMeters};
        if (aabbOutsideFrustum(bmin, bmax, (vec4*)planes)) continue;

        HeightmapTerrainPushConstants pc = {
            .tileOriginX     = (float)e->tileX * ht->tileSizeMeters,
            .tileOriginZ     = (float)e->tileZ * ht->tileSizeMeters,
            .tileSizeMeters  = ht->tileSizeMeters,
            .gridSegments    = (float)seg,
            .heightScale     = 1.0f,
            .texDim          = (float)HEIGHTMAP_TEX,
            .wireFrame       = (float)wireFrame,
            .debugHeightRamp = (float)debugRamp,
        };

        vulkanBindDesc(cmd, pipe, &e->heightDesc, 1);
        vulkanPush(cmd, pipe, sizeof(pc), &pc);
        vulkanDraw(cmd, 6 * seg * seg, 1); // 3 corners per triangle patch, 2 * seg^2 patches

        renderer.drawCalls++;
        renderer.instanceCount++;
        renderer.triangleCount += 2u * (u32)seg * (u32)seg;
    }
}

void VulkanHeightmapTerrainPass::update() {
    if (vulkan.skipFrame) {
        utils::warn("heightmapTerrain: skipFrame");
        return;
    }

    HeightmapTerrain* ht = heightmapTerrainGetActive();
    if (!ht || !ht->initialized) return;
    if (!scenePipe.pipe) recreatePipelines();
    if (static_cast<i32>(gpuTiles.size()) == 0) return;

    VulkanCommand* cmd = vulkan.currentCmd;
    if (!cmd) {
        utils::warn("heightmapTerrain: no currentCmd");
        return;
    }

    Entity* cameraEntity = cameraGetEntity();
    Camera* camera       = cameraEntity ? getComponent(cameraEntity->scene, Camera, cameraEntity->id) : NULL;
    if (!camera) {
        utils::warn("heightmapTerrain: no camera entity");
        return;
    }

    VulkanImage* sceneColor     = vulkanFrameResourcesGetSceneColor();
    VulkanImage* normals        = vulkanFrameResourcesGetNormals();
    VulkanImage* material       = vulkanFrameResourcesGetMaterial();
    VulkanImage* depthImage     = vulkanFrameResourcesGetDepth();
    if (!sceneColor || !normals || !material || !depthImage) return;

    Transform* cameraTransform = getComponent(cameraEntity->scene, Transform, cameraEntity->id);
    if (!cameraTransform) return;
    i32 camTileX = (i32)floorf(cameraTransform->pos[0] / ht->tileSizeMeters);
    i32 camTileZ = (i32)floorf(cameraTransform->pos[2] / ht->tileSizeMeters);

    vulkanBeginRender(.cmd     = cmd,
                      .pipe    = &scenePipe,
                      .color1  = sceneColor,
                      .color2  = normals,
                      .color3  = material,
                      .depth   = depthImage);

    vulkanViewport(cmd, 0, sceneColor->extent.height, sceneColor->extent.width,
                   -((i32)sceneColor->extent.height));
    vulkanScissor(cmd, 0, 0, sceneColor->extent.width, sceneColor->extent.height);

    heightmapPassDrawTiles(cmd,
                           &scenePipe,
                           ht,
                           camera,
                           camTileX,
                           camTileZ,
                           0,
                           debugHeightRampEnabled ? 1 : 0);

    if (wireFrameEnabled && sceneWireFramePipe.pipe) {
        heightmapPassDrawTiles(cmd,
                               &sceneWireFramePipe,
                               ht,
                               camera,
                               camTileX,
                               camTileZ,
                               1,
                               debugHeightRampEnabled ? 1 : 0);
    }

    vulkanEndRender(cmd);
}

// ── Depth/velocity pre-pass hook (called by VulkanDepthPass) ─────────────

void vulkanHeightmapTerrainDrawPrepass(void) {
    HeightmapTerrain* ht = heightmapTerrainGetActive();
    if (!ht || !ht->initialized || !prepassPipe.pipe) return;
    if (static_cast<i32>(gpuTiles.size()) == 0) return;

    VulkanCommand* cmd = vulkan.currentCmd;
    if (!cmd) return;

    Entity* cameraEntity = cameraGetEntity();
    Camera* camera       = cameraEntity ? getComponent(cameraEntity->scene, Camera, cameraEntity->id) : NULL;
    if (!camera) return;

    Transform* cameraTransform = getComponent(cameraEntity->scene, Transform, cameraEntity->id);
    if (!cameraTransform) return;
    i32 camTileX = (i32)floorf(cameraTransform->pos[0] / ht->tileSizeMeters);
    i32 camTileZ = (i32)floorf(cameraTransform->pos[2] / ht->tileSizeMeters);

    const vec4* planes = (const vec4*)camera->cameraUbo.frustumPlanes;

    vulkanBindPipe(cmd, &prepassPipe);
    for (u32 i = 0; i < gpuTiles.size(); i++) {
        HeightmapGpuTile* e = &gpuTiles[i];
        if (!e->inUse) continue;

        i32 dx = e->tileX - camTileX;
        if (dx < 0) dx = -dx;
        i32 dz = e->tileZ - camTileZ;
        if (dz < 0) dz = -dz;
        i32 ring = dx > dz ? dx : dz;
        if (ring > 2) continue;

        i32 seg = heightmapRingSegments(ring);

        vec3 bmin = {e->tileX * ht->tileSizeMeters, -80.0f, e->tileZ * ht->tileSizeMeters};
        vec3 bmax = {bmin[0] + ht->tileSizeMeters, 2560.0f, bmin[2] + ht->tileSizeMeters};
        if (aabbOutsideFrustum(bmin, bmax, (vec4*)planes)) continue;

        HeightmapTerrainPushConstants pc = {
            .tileOriginX     = (float)e->tileX * ht->tileSizeMeters,
            .tileOriginZ     = (float)e->tileZ * ht->tileSizeMeters,
            .tileSizeMeters  = ht->tileSizeMeters,
            .gridSegments    = (float)seg,
            .heightScale     = 1.0f,
            .texDim          = (float)HEIGHTMAP_TEX,
            .wireFrame       = 0.0f,
            .debugHeightRamp = 0.0f,
        };

        vulkanBindDesc(cmd, &prepassPipe, &e->heightDesc, 1);
        vulkanPush(cmd, &prepassPipe, sizeof(pc), &pc);
        vulkanDraw(cmd, 6 * seg * seg, 1); // 3 corners per triangle patch, 2 * seg^2 patches

        renderer.drawCalls++;
        renderer.instanceCount++;
        renderer.triangleCount += 2u * (u32)seg * (u32)seg;
    }
}

// ── Lifecycle ─────────────────────────────────────────────────────────────

void VulkanHeightmapTerrainPass::added() {
    utils::signalSubscribe("swapchainCreated", swapchainCreated);
    recreatePipelines();
}

void VulkanHeightmapTerrainPass::removed() {
    heightmapPassReset();
    gpuTiles.clear();
    for (u32 i = 0; i < deferredDescs.size(); i++) vulkanDestroyDesc(&deferredDescs[i].desc);
    deferredDescs.clear();

    vulkanDestroyPipe(&scenePipe);
    vulkanDestroyPipe(&sceneWireFramePipe);
    vulkanDestroyPipe(&prepassPipe);
    if (layoutHeightDesc.set) vulkanDestroyDesc(&layoutHeightDesc);
    cachedHt = NULL;
}}  // namespace engine
