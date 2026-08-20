#pragma once

#include "ecs/system/System.h"
#include "renderer/vulkan/resources/VulkanBuffer.h"

// Sentinel for "no texture" (procedural species). The index otherwise selects
// textures[id] in the global set 0 (see globalset.shader).
#define NO_PROPS_TEX 0xFFFFFFFFu

// Props vertex = SceneVertex (56 B) + a per-vertex texture-array index (4 B)
// + a per-vertex part color (12 B).  The index selects the base-color
// texture in the global descriptor set 0, which the props pipeline already
// binds (vulkanCreatePipe auto-includes globalSet0.layout).  Procedural
// species use NO_PROPS_TEX.
//
// `color` is a per-part albedo multiplier: white (1,1,1) marks a part as
// "tintable" (receives the per-instance biome tint, e.g. leaves); a baked
// non-white color (e.g. brown trunk) keeps its own colour and is NOT tinted.
namespace engine {
typedef struct PropsVertex {
    float position[3];   // 12 bytes
    float normal[3];     // 12 bytes
    float tangent[4];    // 16 bytes
    float uv[2];         // 8 bytes
    uint32_t joints;     // 4 bytes
    uint32_t weights;    // 4 bytes
    uint32_t texId;      // 4 bytes — texture-array index (NO_PROPS_TEX = none)
    float color[3];      // 12 bytes — per-part colour (white = tintable)
} PropsVertex;          // 72 bytes
static_assert(sizeof(PropsVertex) == 72, "PropsVertex must be 72 bytes");

/*
 * VulkanAzgaarPropsPass
 * ---------------------
 * Instanced renderer for the Azgaar world's vegetation / landmarks
 * (workstream B of plans/azgaar-world-population.md).  The pass is intentionally
 * domain-agnostic: it only knows a merged species-mesh buffer, a per-tile
 * instance buffer (instances grouped by (species, variant)), and a per-frame
 * data block (wind / density).  All scatter / species / variant logic lives in
 * the game (AzgaarProps) and is pushed through the Set* API below.
 *
 * One instanced draw per (tile, species, variant) range: the merged mesh's
 * per-variant index range is bound, the tile's instance buffer is bound at the
 * range's offset, and per-variant push constants carry the mesh bounds + sway
 * weight so the wind animation is identical for procedural placeholders and the
 * hand-authored .dat models (D11).  Per-tile CPU frustum culling (v1).
 *
 * Opaque, depth-write on, MSAA like the terrain pass.  Registered right after
 * vulkanHeightmapTerrainPass so props occlude correctly against terrain.
 */

// One scatter instance (CPU/GPU identical, 44 B).  `pos` is the world position
// on the tile's CPU height grid (y == ground height, flush placement).  `color`
// is the per-instance tint (biome colour x jitter; buildings would use
// state/culture colour).  `phase` de-syncs the wind sway per instance.  `variant`
// is the per-species mesh-variant index (CPU-side grouping key only; the engine
// pass binds the matching variant's index sub-range, so no new GPU attribute).
typedef struct PropInstance {
    float pos[3];
    float yaw;      // 0..2 pi
    float scale;    // target meters (uniform)
    float color[3]; // [0,1]
    float phase;    // 0..2 pi
    u32   species;  // id (selects the PropVariantRange rows for this species)
    u32   variant; // mesh-variant index within the species (0 for single-variant species)
} PropInstance;

static_assert(sizeof(PropInstance) == 44, "PropInstance must be 44 bytes");

// Per-(species, variant) mesh metadata, flattened to what the shader needs.
// `indexOffset`/`indexCount` select this variant's sub-range of the merged mesh
// index buffer.  `boundsMin/Max` are the local-space AABB used to normalise the
// sway weight (trunk ~ 0, canopy == 1).  `swayFactor` is how much this species
// sways (0 = rock/building).  `flags` bit 0 = alpha-test (flowers).  The table
// is flat: one row per (species, variant) pair.  A species with N authored
// objects contributes N rows; single-variant species contribute one row.
typedef struct PropVariantRange {
    u32   species;
    u32   variant;
    u32   indexOffset;
    u32   indexCount;
    float boundsMin[3];
    float boundsMax[3];
    float swayFactor;
    u32   flags;   // bit 0 = alpha-test (flowers)
    u32   lodRole; // 0 = near LOD, 1 = far LOD, 2 = no LOD (always visible)
} PropVariantRange;

// Within a tile's instance array, instances are grouped by (species, variant).
// Each range is one instanced draw: bind the variant's mesh sub-range + the
// instance buffer at `start`, draw `count` instances.
typedef struct PropTileRange {
    u32 species; // PropVariantRange species
    u32 variant; // mesh-variant index within the species
    u32 start;   // index into the tile's instance array
    u32 count;   // instance count
} PropTileRange;

class VulkanAzgaarPropsPass : public System {
public:
    VulkanAzgaarPropsPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
};

extern VulkanAzgaarPropsPass vulkanAzgaarPropsPass;

// ── Mesh + variant table (uploaded once per world load, or never for non-Azgaar
//    scenes) ─────────────────────────────────────────────────────────
// `verts` / `idx` are the merged species-mesh buffers (SceneVertex[] / u32[]).
// `variants` / `variantCount` is the flat per-(species, variant) metadata table
// (bounds, sway, mesh sub-ranges).  Re-uploading replaces the previous mesh +
// table.
void vulkanAzgaarPropsSetMeshes(const void* verts, u32 vertCount,
                                const void* idx, u32 idxCount);
void vulkanAzgaarPropsSetVariants(const PropVariantRange* variants, u32 variantCount);

// ── Per-tile instance buffers (uploaded by the game after a scatter job) ──
// Enqueues a thread-safe upload consumed on the render thread.  `instances` /
// `ranges` are owned by the caller and only read (copied under the lock).
void vulkanAzgaarPropsSetTile(i32 tileX, i32 tileZ, u64 readyStamp,
                               const PropInstance* instances, u32 instanceCount,
                               const PropTileRange* ranges, u32 rangeCount);
void vulkanAzgaarPropsClearTile(i32 tileX, i32 tileZ);
void vulkanAzgaarPropsClearAll(void);

// ── Whole-map global instance buffer (settlement buildings, workstream D) ──
// Unlike tiles (streaming, per-tile buffers), settlement buildings are one
// fixed map-wide set, so they live in a single instance buffer cullled by
// one whole-map AABB (`aabbMin`/`aabbMax`, local-space reach is folded in via
// the per-species push constants).  Same thread-safe pending-upload pattern
// as the tiles.
void vulkanAzgaarPropsSetGlobal(const PropInstance* instances, u32 instanceCount,
                                  const PropTileRange* ranges, u32 rangeCount,
                                  const float aabbMin[3], const float aabbMax[3]);
void vulkanAzgaarPropsClearGlobal(void);

// ── Second whole-map slot (landmark props, workstream E) ───────────────────
// Identical layout and thread-safety to the global slot above, kept separate
// so landmarks (AzgaarLandmarks) and settlement buildings can both be live
// without one upload clobbering the other's buffer.
void vulkanAzgaarPropsSetLandmarks(const PropInstance* instances, u32 instanceCount,
                                    const PropTileRange* ranges, u32 rangeCount,
                                    const float aabbMin[3], const float aabbMax[3]);
void vulkanAzgaarPropsClearLandmarks(void);

// Kill switch (draws nothing; also the default for non-Azgaar scenes).
void vulkanAzgaarPropsSetEnabled(bool enabled);

// Depth/velocity pre-pass hook (called by VulkanDepthPass).  Renders the
// animated props into the velocity + view-normal attachments so FSR gets
// valid per-pixel motion vectors.  No-op when props are not active.
void vulkanAzgaarPropsDrawPrepass(void);

// Shadow pass hook (called by VulkanShadowPass once per cascade, INSIDE the
// shadow map's render pass).  Draws the culled props with the cascade's light
// view-projection so vegetation / settlement buildings / landmarks cast sun
// shadows.  No-op when props are not active.
void vulkanAzgaarPropsDrawShadow(struct VulkanCommand* cmd, u32 cascadeIndex);
}  // namespace engine
