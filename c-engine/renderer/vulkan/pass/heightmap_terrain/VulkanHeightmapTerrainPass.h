#pragma once

#include "ecs/system/System.h"

/*
 * VulkanHeightmapTerrainPass
 * -------------------------
 * Renders the engine's HeightmapTerrain (see ecs/system/heightmap/) from a
 * precomputed per-tile indexed corner VBO/IBO: at height-upload time the
 * pass generates the tile's 256^2 render-lattice corners (world pos +
 * normal, replicating the old implicit-lattice VS float-for-float) and
 * shares one 255-segment lattice IBO across all tiles.  The vertex shader
 * is a thin transform of those corners — no lattice enumeration, no
 * height-texel fetches.  The rasterized surface is still exactly the
 * tensor-product bilinear surface the CPU/physics grids define (same
 * texel-centre heights and border-aware stencil normals as the old
 * implicit pass: geometry-identical).
 * Per-tile descriptor sets carry the tile's height texture (still uploaded
 * and bound; nothing samples it from the VS any more); per-tile push
 * constants carry origin/size and the ring-based LOD segment count
 * (uniform 255 for all rings; a per-ring ladder would need per-ring
 * corner data instead of the shared VBO/IBO pair).
 *
 * One depth/velocity pre-pass pipe renders the same lattice into the
 * depth/velocity/view-normal attachments so downstream passes (contact
 * shadows, HiZ) and FSR see the same surface as the scene pass.
 * This is the Azgaar world's terrain backend (the experimental full-mesh pass
 * was removed in the heightmap cutover).
 */

namespace engine {
struct VulkanImage;

class VulkanHeightmapTerrainPass : public System {
public:
    VulkanHeightmapTerrainPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
};

extern VulkanHeightmapTerrainPass vulkanHeightmapTerrainPass;

bool vulkanHeightmapTerrainIsWireFrameEnabled(void);
void vulkanHeightmapTerrainSetWireFrameEnabled(bool enabled);

bool vulkanHeightmapTerrainIsDebugHeightRampEnabled(void);
void vulkanHeightmapTerrainSetDebugHeightRampEnabled(bool enabled);

// Render the heightmap tiles into the CURRENT render pass' depth/velocity/
// view-normal attachments.  Called by the depth pre-pass (VulkanDepthPass)
// after it has begun its render pass.  No-op unless an active HeightmapTerrain
// exists with uploaded tiles.
void vulkanHeightmapTerrainDrawPrepass(void);
}  // namespace engine
