#pragma once

/*
 * HeightmapSource
 * ---------------
 * Pluggable content provider for HeightmapTerrain (see HeightmapTerrain.h).
 *
 * The engine knows nothing about the world that feeds the heightmap; the
 * game implements this vtable (e.g. the Azgaar adapter over a parsed .map).
 *
 * Determinism contract: for a given source instance, heightAt must
 * return identical values for identical (wx, wz) across the whole process
 * lifetime. Tile data is never persisted, so evicted tiles are regenerated
 * from the source — the surface must be a pure function of (source, xz).
 */

namespace engine {
struct HeightmapSource {
    // Final terrain height in metres (sea level 0, negative = seabed) at
    // world (wx, wz).
    float (*heightAt)(void* userData, float wx, float wz);

    void* userData;
};}  // namespace engine
