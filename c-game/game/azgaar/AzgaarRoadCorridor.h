#pragma once

#include "azgaar/AzgaarWorld.h"

// Walkable road corridor: a grade-limited height field along the Azgaar road
// network. Built once per world load and sampled per terrain vertex during
// meshing so the surface under each road never exceeds a walkable grade,
// letting the player (Jolt CharacterVirtual, max slope 45°) traverse roads
// that cross steep cell borders.
//
// The corridor is GLOBAL (covers the whole world) and independent of tile
// streaming, so streamed tiles deform shared border vertices identically and
// never crack. See plans/azgaar-road-walkable-terrain.md.

// Build the corridor from the world's routes (roads only in Phase 1). Safe to
// call again; clears any previous corridor first. Must be called before the
// first terrain tile build and before road decals are placed.
namespace game {
void azgaarRoadCorridorBuild(const AzgaarWorld* world);

// Release all corridor storage.
void azgaarRoadCorridorClear(void);

// Returns true if (worldX, worldZ) lies inside any road corridor (within
// half-width + the edge-blend band of the nearest road centerline). When true,
// *outHeight receives the corridor surface height, blended from the road
// centerline height (at distance <= half-width) toward naturalY across the
// edge-blend band, so the corridor joins the surrounding terrain without a lip.
// naturalY is the unmodified terrain height at the query point.
bool azgaarRoadCorridorSample(float worldX, float worldZ, float naturalY, float* outHeight);
}  // namespace game
