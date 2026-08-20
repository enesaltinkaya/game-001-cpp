#pragma once

#include "azgaar/AzgaarWorld.h"

// Workstream C (rivers), plans/azgaar-world-population.md.  Builds the river
// ribbon mesh (uploaded to the azgaar_river Vulkan pass), the river-point
// spatial hash (shared with riparian workstream B and bridge workstream E),
// and the wet-strip ground decals.  All additive and failure-tolerant: with
// the ENGINE_AZGAAR_RIVERS_DISABLED=1 kill switch (or no river geometry) the
// functions become no-ops.

// A river-point hit from the spatial hash (10 m buckets).
namespace game {
struct AzgaarRiverNearHit {
    float wx, wz;   // world position of the river point
    u32   riverId;  // which river (section 32 id)
    float widthM;   // local ribbon width in metres
};

// Build the ribbon mesh + hash + wet-strip decals and upload the mesh.
// No-op when the kill switch is set or there is no river geometry.
void azgaarRiversInit(const AzgaarWorld* world);

// Tear down the pass mesh, decals and hash.
void azgaarRiversClear(void);

// Query the river-point spatial hash: fills out[] with river points within
// `radius` metres of (wx,wz); returns the number written (capped at maxPts).
u32 azgaarRiversNear(float wx, float wz, float radius,
                       AzgaarRiverNearHit* out, u32 maxPts);
}  // namespace game
