#pragma once

#include "azgaar/AzgaarWorld.h"
#include "ecs/system/heightmap/HeightmapSource.h"

/*
 * AzgaarHeightmapSource
 * ---------------------
 * HeightmapSource adapter over a parsed AzgaarWorld (the .map file).
 *
 * Final terrain height = azgaarHeightToMeters(smooth FMG pixel height)
 *                        + seeded value-noise fBm "geometry band"
 *
 * The noise is world-anchored: a pure function of world (x, z) and a seed
 * derived from the map name. Tiles therefore regenerate bit-identically
 * after eviction and nothing is ever persisted to disk.
 *
 * The geometry band is limited to wavelengths >= 64 m so it is Nyquist-safe
 * on every view of the surface (4 m CPU grid, 8.03 m physics grid and
 * ring-0 render lattice, coarser far rings). Shorter wavelengths (32/16/8/4
 * m) only perturb fragment normals (shader side) — they must never enter
 * the geometry, or the rendered ring-0 lattice aliases them away while
 * physics/CPU keep them (visible as grass/players floating above the
 * rendered ground).
 */

namespace game {
struct AzgaarHeightmapSource {
    engine::HeightmapSource vtable;  // .userData == this struct
    const AzgaarWorld* world;
    u32 noiseSeed;           // FNV-1a of the map name
    bool detailEnabled;      // false = pure FMG heights (debug)
};

void azgaarHeightmapSourceInit(AzgaarHeightmapSource* src,
                               const AzgaarWorld* world,
                               const char* mapName);
}  // namespace game
