#pragma once

#include "azgaar/AzgaarWorld.h"

// ── Azgaar water: CPU side ────────────────────────────────────────────
// Builds a camera-following grid mesh, pushes sea level + water params
// each frame via the Vulkan water pass, and handles lifecycle.
//
// Lifecycle:
//   azgaarWaterInit(world)        — called once after world is loaded
//   azgaarWaterUpdate(camX, camZ) — called each frame with camera position
//   azgaarWaterDestroy()          — called on teardown

namespace game {
void azgaarWaterInit(const AzgaarWorld* world);
void azgaarWaterUpdate(float camX, float camZ);
void azgaarWaterDestroy(void);

// Grid parameters (must match shader defines).
// GRID_SIZE is the full edge length of the camera-following grid; the grid
// radius (GRID_SIZE/2) should reach the camera far plane so the water
// extends to the horizon without a visible edge. CELL_SIZE = GRID_SIZE/GRID_DIVS
// must stay coarse enough to keep the mesh affordable; world vertices land on
// multiples of CELL_SIZE so the camera-snap keeps the wave field stable.
#define AZGAAR_WATER_GRID_SIZE  8192.0f
#define AZGAAR_WATER_GRID_DIVS  512
}  // namespace game
