#pragma once

#include "azgaar/AzgaarWorld.h"
#include "renderer/vulkan/pass/azgaar_props/VulkanAzgaarPropsPass.h"

/*
 * AzgaarProps
 * -----------
 * CPU side of the Azgaar vegetation / landmark system (workstream B of
 * plans/azgaar-world-population.md).  Owns:
 *   - the merged species-mesh buffer (procedural placeholders + the N hand-
 *     authored deciduous objects from deciduous.dat) + the per-(species,
 *     variant) metadata table,
 *   - the per-tile scatter (deterministic, on a small thread pool, D7) that
 *     produces per-tile instance buffers grouped by (species, variant),
 *   - a road-distance hash (from section-37 routes) for the "no trees on
 *     roads" gate.
 *
 * Everything it produces is pushed to the domain-agnostic engine pass
 * (VulkanAzgaarPropsPass).  Lifecycle is driven by LoadingAzgaar:
 *   azgaarPropsInit(world)   — build meshes + species table + road hash
 *   azgaarPropsUpdate()      — per frame: poll READY tiles, scatter, finalize
 *   azgaarPropsDestroy()     — free everything, clear the pass
 *
 * Deterministic: a tile's instances are a pure function of (mapSeed, tileX,
 * tileZ, build-time camera) so eviction + regeneration is bit-identical.
 */

// Species ids (match the plan's table; the merged mesh carries one sub-range
// per species in this order).  Vegetation = 0..12 (Phase 1), buildings = 13..19
// (Phase 3, workstream D: settlement clusters placed by AzgaarSettlements).
enum AzgaarPropSpecies {
    AZGAAR_PROP_GRASS_TUFT     = 0,
    AZGAAR_PROP_CONIFER        = 1,
    AZGAAR_PROP_CONIFER_FAR    = 2,
    AZGAAR_PROP_DECIDUOUS      = 3,
    AZGAAR_PROP_DECIDUOUS_FAR  = 4,
    AZGAAR_PROP_ACACIA         = 5,
    AZGAAR_PROP_PALM           = 6,
    AZGAAR_PROP_CACTUS         = 7,
    AZGAAR_PROP_DEAD_TREE      = 8,
    AZGAAR_PROP_REED           = 9,
    AZGAAR_PROP_SHRUB          = 10,
    AZGAAR_PROP_ROCK           = 11,
    AZGAAR_PROP_FLOWER         = 12,
    AZGAAR_PROP_HUT           = 13,
    AZGAAR_PROP_HOUSE         = 14,
    AZGAAR_PROP_TOWER         = 15,
    AZGAAR_PROP_WALL          = 16,
    AZGAAR_PROP_TEMPLE        = 17,
    AZGAAR_PROP_DOCK          = 18,
    AZGAAR_PROP_GATE          = 19,
    // Landmarks (Phase 4, workstream E): placed by AzgaarLandmarks from
    // section-35 markers, never by the biome scatter.  The lighthouse is two
    // instances (grey tower + near-white cap mesh) since per-instance tint
    // cannot two-tone a single mesh.
    AZGAAR_PROP_VOLCANO       = 20,
    AZGAAR_PROP_LIGHTHOUSE    = 21,
    AZGAAR_PROP_LIGHTHOUSE_CAP = 22,
    AZGAAR_PROP_RUIN_COLUMN   = 23,
    AZGAAR_PROP_RUIN_ARCH     = 24,
    AZGAAR_PROP_MINE_FRAME    = 25,
    AZGAAR_PROP_BRIDGE        = 26,
    AZGAAR_PROP_COUNT         = 27,
};

void azgaarPropsInit(const AzgaarWorld* world);
void azgaarPropsUpdate(void);
void azgaarPropsDestroy(void);

// Whole-map global instance sets (settlement buildings / landmarks).  The
// owning module (AzgaarSettlements / AzgaarLandmarks) registers its full set
// once at load; the props system keeps an owned copy, uploads it, and then
// culls it per-instance like the tiles (frustum + per-species distance caps)
// as the camera moves.
void azgaarPropsRegisterGlobal(const PropInstance* instances, u32 instanceCount,
                               const PropTileRange* ranges, u32 rangeCount,
                               const float aabbMin[3], const float aabbMax[3],
                               bool landmarks);
void azgaarPropsClearGlobal(bool landmarks);