#pragma once

#include "azgaar/AzgaarWorld.h"

/*
 * AzgaarLandmarks
 * ---------------
 * CPU side of the landmark system (workstream E, Phase 4 of
 * plans/azgaar-world-population.md).  Turns section-35 markers into 3D props:
 *   - volcano: single large cone+crater instance (reads as a landmark from
 *     5 km; smoke quads are deliberately skipped in v1),
 *   - lighthouse: tower instance + near-white cap instance (v1 stand-in for
 *     the emissive lantern; per-instance tint cannot two-tone one mesh),
 *   - ruins: a small deterministic cluster of half-buried broken columns
 *     and arches,
 *   - mines: timber headframe + rock pile,
 *   - bridges: one plank-bridge instance spanning the river at the marker
 *     cell (span from the river hash; requires rivers to be enabled),
 *   - hot-springs / water-sources: small round pool decals (steam skipped),
 *   - sacred-forests: no mesh — a 300 m vegetation density x3 disc queried
 *     by the props scatter via azgaarLandmarksForestBoost.
 *
 * Everything is uploaded through the azgaar_props pass' landmark slot
 * (separate from the settlements' global slot).  Kill switch:
 * ENGINE_AZGAAR_LANDMARKS_DISABLED=1 (forest boost then reports 1.0).
 *
 * Deterministic: a marker's props are a pure function of (mapSeed, marker id)
 * so regeneration after streaming is bit-identical.
 */

// Generate the landmark instances for the whole map (filters world->markers
// by kind) and upload them to the pass' landmark slot.  Must run AFTER
// azgaarRiversInit (bridges query the river hash).  `groundAt` is the
// heightmap source' exact heightAt callback (same contract as
// azgaarSettlementsInit).  No-op when disabled or markerless.
void azgaarLandmarksInit(const AzgaarWorld* world,
                         float (*groundAt)(void* userData, float wx, float wz),
                         void* groundUserData);
void azgaarLandmarksClear(void);

// Sacred-forest density boost at (wx, wz): 3.0f inside any disc, 1.0f
// outside.  Read-only and pool-thread safe (the disc count is published
// last, same discipline as the settlement plateau grid).
float azgaarLandmarksForestBoost(float wx, float wz);
