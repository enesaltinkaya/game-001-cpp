#pragma once

#include "azgaar/AzgaarWorld.h"

/*
 * AzgaarSettlements
 * -----------------
 * CPU side of the settlement system (workstream D of
 * plans/azgaar-world-population.md).  Owns:
 *   - deterministic per-settlement building clusters (huts/houses/towers/
 *     walls/temples/docks/gates) uploaded as one whole-map instance buffer
 *     through the azgaar_props pass' global API,
 *   - the D8 terrain plateau query: blends the natural terrain height toward
 *     each settlement's flat centre so towns sit on level ground,
 *   - a nearest-settlement query for the zone banner.
 *
 * Kill switch: ENGINE_AZGAAR_SETTLE_DISABLED=1 disables both the building
 * instances and the plateau (heightAt then returns the natural height).
 *
 * Deterministic: a settlement's buildings are a pure function of
 * (mapSeed, settlement id) so regeneration after streaming is bit-identical.
 */

// Generate the building clusters for the whole map and upload them to the
// azgaar_props pass' global instance buffer.  No-op when disabled or empty.
// `groundAt` is the heightmap source' exact heightAt callback (the surface
// the terrain mesh is built from: natural + fBm detail + D8 plateau).  Each
// building's Y is sampled through it so houses sit flush with the ground
// instead of floating.
void azgaarSettlementsInit(const AzgaarWorld* world,
                            float (*groundAt)(void* userData, float wx, float wz),
                            void* groundUserData);
void azgaarSettlementsClear(void);

// D8 plateau: given the natural terrain height (meters) at (wx, wz), returns
// the height blended toward each nearby settlement's flatY.  No-op when the
// kill switch is set (returns naturalY unchanged).
float azgaarSettlementsPlateauY(const AzgaarWorld* world, float wx, float wz, float naturalY);

// The closest settlement whose footprint (radiusM + 30 m) contains (wx, wz),
// or NULL when none does.  Used by the zone banner.
const AzgaarSettlement* azgaarSettlementsNearest(const AzgaarWorld* world, float wx, float wz);
