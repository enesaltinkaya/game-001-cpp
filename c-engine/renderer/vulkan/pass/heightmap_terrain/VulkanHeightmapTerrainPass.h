#pragma once

#include "ecs/system/System.h"

/*
 * VulkanHeightmapTerrainPass
 * -------------------------
 * Renders the engine's HeightmapTerrain (see ecs/system/heightmap/) with an
 * IMPLICIT grid: no VBO/IBO, the vertex shader enumerates the tile's
 * lattice triangle corners from push constants (6 * seg * seg vertices per
 * draw, plain triangle list) and lifts them from the tile's height texture
 * (R32F, 512^2, final metres) — the rasterized surface is exactly the
 * tensor-product bilinear surface the CPU/physics grids define.
 * Per-tile descriptor sets carry the tile's height texture;
 * per-tile push constants carry origin/size and the ring-based LOD segment
 * count (ring 0: 128, ring 1: 64, ring 2: 32).
 *
 * One depth/velocity pre-pass pipe renders the same lattice into the
 * depth/velocity/view-normal attachments so downstream passes (GTAO, contact
 * shadows, HiZ) and FSR see the same surface as the scene pass.
 * This is the Azgaar world's terrain backend (the experimental full-mesh pass
 * was removed in the heightmap cutover).
 */

extern struct System vulkanHeightmapTerrainPass;

bool vulkanHeightmapTerrainIsWireFrameEnabled(void);
void vulkanHeightmapTerrainSetWireFrameEnabled(bool enabled);

bool vulkanHeightmapTerrainIsDebugHeightRampEnabled(void);
void vulkanHeightmapTerrainSetDebugHeightRampEnabled(bool enabled);

// Render the heightmap tiles into the CURRENT render pass' depth/velocity/
// view-normal attachments.  Called by the depth pre-pass (VulkanDepthPass)
// after it has begun its render pass.  No-op unless an active HeightmapTerrain
// exists with uploaded tiles.
void vulkanHeightmapTerrainDrawPrepass(void);