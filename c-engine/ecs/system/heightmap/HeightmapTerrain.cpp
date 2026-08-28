#include "ecs/system/heightmap/HeightmapTerrain.h"
#include "ecs/Ecs.h"
#include "ecs/system/System.h"
#include "ecs/system/scene/SceneSystem.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/transform/TransformComponent.h"
#include "ecs/system/physics/PhysicsSystem.h"
#include "thread/Thread.h"

/*
 * Threading model
 * ---------------
 * One process-wide builder worker consumes a global job queue
 * (HeightmapTerrain* + tile coords).  `heightmapLock` protects:
 *   - the job queue,
 *   - every HeightmapTerrain's tile table and tile state transitions,
 *   - the registered/inFlight bookkeeping.
 * The heavy grid generation runs with the lock released; the builder only
 * re-locks to publish (or discard) the grids.  Grid memory of a published
 * tile is freed exclusively by the main thread (eviction / destroyData),
 * and destroyData drains inFlight before freeing anything, so no other
 * locking is needed to read a READY grid.
 */

namespace engine {
// Off-tile fast-reject lattice for the GI march (see the Phase 4 section
// for the design): exact heights on a regular world-space lattice covering
// the tile plus a march-range halo. Params are derived from (tile, tile
// size, storage size) — the job only stores the heights themselves.
struct HeightmapGiRegion {
    float minWX, minWZ; // world coords of lattice point (0, 0)
    float invCell;      // 1 / lattice spacing
    u32   dim;          // lattice points per side
    const float* h;     // [dim]^2, exact heights at the lattice points
};

struct HeightmapJob {
    HeightmapTerrain* ht;
    i32 tileX, tileZ;
    bool gi; // false = grids generation, true = baked-GI bake
    // Baked-GI chunking (gi jobs only): a bake runs in row chunks and yields
    // to pending grids jobs (tile streaming is critical, GI is progressive).
    // The claimed grid snapshot, the fast-reject region lattice and the
    // partial output ride in the job so a resumed chunk stays bit-identical
    // (fixed per-texel math, rows are independent).
    std::vector<float> giHeights;
    std::vector<float> giRegion; // empty = not built yet; [dim]^2 when filled
    std::vector<u8>    giPartial;
    u32                giRow = 0; // next row to bake
    double             giMs  = 0; // accumulated bake time
    bool               giOwned = false; // owns an ht->inFlight slot
};

static HeightmapTerrain* activeHeightmapTerrain = nullptr;
static u64 heightmapReadyCounter = 0; // global; bumped per READY publish

// ENGINE_GI_DISABLED=1 skips the GI bake entirely (the shader keeps the
// IBL/SH ambient fallback). Read once, like the other engine env toggles.
static bool heightmapGiDisabled(void) {
    static int v = -1;
    if (v < 0) v = getenv("ENGINE_GI_DISABLED") != nullptr;
    return v == 1;
}

static utils::Thread heightmapLock = {.mutex = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER};
static utils::Thread* buildWorker  = nullptr;
static bool workerStarted   = false;
static std::vector<HeightmapJob> buildQueue;

static void* heightmapBuildThreadMain(void* userData);

void heightmapTerrainSetActive(HeightmapTerrain* ht) {
    activeHeightmapTerrain = ht;
}

HeightmapTerrain* heightmapTerrainGetActive(void) {
    return activeHeightmapTerrain;
}

// ── Grid helpers (lock-free: pure math on already-published data) ─────────

// Bilinear sample of a regular grid spanning [0, dim-1] (endpoints included).
float heightmapGridBilinear(const float* grid, u32 dim, float gx, float gz) {
    if (gx < 0.0f) gx = 0.0f;
    else if (gx > static_cast<float>(dim) - 1.0f) gx = static_cast<float>(dim) - 1.0f;
    if (gz < 0.0f) gz = 0.0f;
    else if (gz > static_cast<float>(dim) - 1.0f) gz = static_cast<float>(dim) - 1.0f;

    i32 x1 = static_cast<i32>(gx);
    i32 z1 = static_cast<i32>(gz);
    if (x1 >= static_cast<i32>(dim) - 1) x1 = static_cast<i32>(dim) - 2;
    if (z1 >= static_cast<i32>(dim) - 1) z1 = static_cast<i32>(dim) - 2;
    float tx = gx - static_cast<float>(x1);
    float tz = gz - static_cast<float>(z1);

    size_t stride = dim;
    float a = grid[z1 * stride + x1];
    float b = grid[z1 * stride + x1 + 1];
    float c = grid[(z1 + 1) * stride + x1];
    float d = grid[(z1 + 1) * stride + x1 + 1];
    return a + (b - a) * tx + (c + (d - c) * tx - (a + (b - a) * tx)) * tz;
}

// ── Tiles ──────────────────────────────────────────────────────────────────

static HeightmapTile* heightmapTerrainCreateTile(HeightmapTerrain* ht, i32 tileX, i32 tileZ) {
    HeightmapTile* tile = new HeightmapTile{
        .tileX          = tileX,
        .tileZ          = tileZ,
        .originX        = static_cast<float>(tileX) * ht->tileSizeMeters,
        .originZ        = static_cast<float>(tileZ) * ht->tileSizeMeters,
        .sizeMeters     = ht->tileSizeMeters,
        .state          = HEIGHTMAP_TILE_EMPTY,
        .inWindow       = false,
        .genMs          = 0.0,
        .readyStamp     = 0,
        .gpuData        = nullptr,
        .physicsData    = nullptr,
        .giReady        = false,
        .lruStamp       = 0,
    };
    ht->tiles.push_back(tile);
    return tile;
}

static void heightmapTerrainFreeTile(HeightmapTile* tile) {
    // Phase 2 hook: destroy GPU textures; phase 3 hook: destroy the Jolt
    // heightfield body before freeing the tile. Main thread only.
    //
    // The body is freed UNCONDITIONALLY (not gated on physicsSystemJoltActive):
    // joltHeightMapDestroy is safe when the Jolt world is already gone (it
    // null-checks joltSystem and just deletes the wrapper). Gating on the
    // active flag leaked every body, because the physics system is removed
    // before the heightmap terrain is destroyed at shutdown.
    if (tile->physicsData) {
        joltHeightMapDestroy(reinterpret_cast<JoltHeightMap*>(tile->physicsData));
        tile->physicsData = nullptr;
    }
    tile->gi.clear(); // phase 4: baked GI lives with the tile
    delete tile;
}

HeightmapTile* heightmapTerrainGetTile(HeightmapTerrain* ht, i32 tileX, i32 tileZ) {
    if (!ht || !ht->initialized) return nullptr;
    for (HeightmapTile* tile : ht->tiles) {
        if (tile->tileX == tileX && tile->tileZ == tileZ) return tile;
    }
    return nullptr;
}

// Forward declaration (defined below; caller must hold heightmapLock).
static HeightmapTile* heightmapTerrainFindTile(HeightmapTerrain* ht, i32 tileX, i32 tileZ);

bool heightmapTerrainHasBodyAt(const HeightmapTerrain* ht, float wx, float wz) {
    if (!ht || !ht->initialized) return false;

    utils::threadLock(&heightmapLock);
    bool has = false;
    if (ht->registered) {
        HeightmapTile* tile = heightmapTerrainFindTile(const_cast<HeightmapTerrain*>(ht),
                                                       heightmapWorldToTileCoord(ht, wx),
                                                       heightmapWorldToTileCoord(ht, wz));
        has = (tile && tile->physicsData != nullptr);
    }
    utils::threadUnlock(&heightmapLock);
    return has;
}

u32 heightmapTerrainSnapshotTiles(HeightmapTerrain* ht,
                                  HeightmapTileView* outViews,
                                  u32 cap) {
    if (!ht || !ht->initialized || !outViews) return 0;

    utils::threadLock(&heightmapLock);
    u32 written = 0;
    if (ht->registered) {
        for (HeightmapTile* tile : ht->tiles) {
            if (written >= cap) break;
            if (tile->state != HEIGHTMAP_TILE_READY || tile->heights.empty()) {
                continue;
            }
            outViews[written] = HeightmapTileView{
                .tileX      = tile->tileX,
                .tileZ      = tile->tileZ,
                .readyStamp = tile->readyStamp,
                .originX    = tile->originX,
                .originZ    = tile->originZ,
                .sizeMeters = tile->sizeMeters,
                .heights    = tile->heights.data(),
                .gi         = tile->giReady ? tile->gi.data() : nullptr,
                .giDim      = HEIGHTMAP_GI_DIM,
                .giReady    = tile->giReady,
            };
            written++;
        }
    }
    utils::threadUnlock(&heightmapLock);
    return written;
}

bool heightmapTerrainCopyTile(HeightmapTerrain* ht,
                              i32 tileX,
                              i32 tileZ,
                              float* outHeights) {
    if (!ht || !ht->initialized || !outHeights) return false;

    utils::threadLock(&heightmapLock);
    bool copied = false;
    if (ht->registered) {
        HeightmapTile* tile = heightmapTerrainFindTile(ht, tileX, tileZ);
        if (tile && tile->state == HEIGHTMAP_TILE_READY && !tile->heights.empty()) {
            memcpy(outHeights, tile->heights.data(), sizeof(float) * static_cast<size_t>(HEIGHTMAP_TEX) * HEIGHTMAP_TEX);
            copied = true;
        }
    }
    utils::threadUnlock(&heightmapLock);
    return copied;
}

bool heightmapTerrainCopyPhysicsTile(HeightmapTerrain* ht,
                                     i32 tileX,
                                     i32 tileZ,
                                     float* outHeights) {
    if (!ht || !ht->initialized || !outHeights) return false;

    utils::threadLock(&heightmapLock);
    bool copied = false;
    if (ht->registered) {
        HeightmapTile* tile = heightmapTerrainFindTile(ht, tileX, tileZ);
        if (tile && tile->state == HEIGHTMAP_TILE_READY && !tile->physicsHeights.empty()) {
            memcpy(outHeights,
                   tile->physicsHeights.data(),
                   sizeof(float) * static_cast<size_t>(HEIGHTMAP_PHYSICS_PSN) *
                       static_cast<size_t>(HEIGHTMAP_PHYSICS_PSN));
            copied = true;
        }
    }
    utils::threadUnlock(&heightmapLock);
    return copied;
}

static HeightmapTile* heightmapTerrainFindTile(HeightmapTerrain* ht, i32 tileX, i32 tileZ) {
    // Caller holds heightmapLock.
    for (HeightmapTile* tile : ht->tiles) {
        if (tile->tileX == tileX && tile->tileZ == tileZ) return tile;
    }
    return nullptr;
}

// ── Generation ─────────────────────────────────────────────────────────────

// Runs on the builder thread, lock released. Fills the tile's CPU grids from
// the source. Border columns sample the exact same world coordinates as the
// neighbouring tiles' borders, so the surface is watertight.
static void heightmapTerrainGenerateGrids(HeightmapTerrain* ht, HeightmapTile* tile) {
    const u32 tex = HEIGHTMAP_TEX;
    const u32 psn = HEIGHTMAP_PHYSICS_PSN;

    double t0 = utils::nanos();

    std::vector<float> heights(static_cast<size_t>(tex) * tex);
    float step = tile->sizeMeters / static_cast<float>(tex - 1);
    for (u32 z = 0; z < tex; ++z) {
        float wz = tile->originZ + static_cast<float>(z) * step;
        for (u32 x = 0; x < tex; ++x) {
            float wx = tile->originX + static_cast<float>(x) * step;
            heights[static_cast<size_t>(z) * tex + x] = ht->source.heightAt(ht->source.userData, wx, wz);
        }
    }

    // Physics grid: bilinear resample of the fine grid. Endpoints coincide
    // with the fine grid's endpoints, so the physics surface shares tile
    // borders exactly (and borders with neighbouring physics grids).
    std::vector<float> physics(static_cast<size_t>(psn) * psn);
    float scale    = static_cast<float>(tex - 1) / static_cast<float>(psn - 1);
    for (u32 z = 0; z < psn; ++z) {
        float gz = static_cast<float>(z) * scale;
        for (u32 x = 0; x < psn; ++x) {
            physics[static_cast<size_t>(z) * psn + x] = heightmapGridBilinear(heights.data(), tex, static_cast<float>(x) * scale, gz);
        }
    }

    tile->heights        = std::move(heights);
    tile->physicsHeights = std::move(physics);
    tile->genMs          = (utils::nanos() - t0) / 1e6;
}

// Shared-border consistency check between a freshly READY tile and its
// already-READY neighbours. Border columns sample identical world positions,
// so any mismatch means a non-deterministic source (would surface as cracks
// or physics gaps at tile edges).  Caller holds heightmapLock.
static void heightmapTerrainSeamCheck(HeightmapTerrain* ht, HeightmapTile* tile) {
    const u32   tex     = HEIGHTMAP_TEX;
    const float tol     = 1e-4f;
    const char* pairs[2] = {"west", "north"};
    const i32   dx[2]    = {-1, 0};
    const i32   dz[2]    = {0, -1};

    for (int i = 0; i < 2; ++i) {
        HeightmapTile* nb = heightmapTerrainFindTile(ht, tile->tileX + dx[i], tile->tileZ + dz[i]);
        if (!nb || nb->state != HEIGHTMAP_TILE_READY || nb->heights.empty()) continue;

        bool bad = false;
        float diff = 0.0f;
        if (i == 0) {
            // West neighbour: its last column (x = tex-1) vs our first column.
            for (u32 z = 0; z < tex && !bad; ++z) {
                diff = fabsf(nb->heights[static_cast<size_t>(z) * tex + (tex - 1)] - tile->heights[static_cast<size_t>(z) * tex]);
                if (diff > tol) bad = true;
            }
        } else {
            // North neighbour (tileZ-1): its last row (z = tex-1) vs our first row.
            for (u32 x = 0; x < tex && !bad; ++x) {
                diff = fabsf(nb->heights[(size_t)(tex - 1) * tex + x] - tile->heights[x]);
                if (diff > tol) bad = true;
            }
        }
        if (bad) {
            ht->seamFailures++;
            utils::warn("heightmapTerrain: SEAM MISMATCH vs %s neighbour tile(%d,%d) at tile(%d,%d): diff=%.5f m",
                 pairs[i],
                 tile->tileX + dx[i],
                 tile->tileZ + dz[i],
                 tile->tileX,
                 tile->tileZ,
                 diff);
        }
    }
}

// ── Phase 4: baked GI (per-tile sky-visibility irradiance) ──────────────────
// Each READY tile additionally carries a [HEIGHTMAP_GI_DIM]^2 RGB u8 texture:
// the sky irradiance the terrain sees at each point, integrated over
// HEIGHTMAP_GI_DIRS fixed stratified upper-hemisphere directions and marched
// against the GLOBAL heightfield (ht->source.heightAt) — rays may leave the
// tile freely, so there are no neighbour dependencies and the bake is a pure
// function of (source, tileX, tileZ) (the determinism contract of the height
// grids applies). Valleys read darker than the uniform IBL/SH ambient, ridges
// brighter. See plans/terrain-baked-gi.md.
//
// Runs on the builder thread (same worker as grid generation) with
// heightmapLock released — heavy work lock-free, publish under lock. The
// bake touches only a lock-protected snapshot of the tile's published grid
// (READY tiles may be evicted and freed mid-bake, so the tile pointer must
// not be dereferenced here) plus ht->source, which stays valid while the
// worker is in flight (destroyData drains inFlight).

#define HEIGHTMAP_GI_DIRS      256  // stratified upper-hemisphere directions
#define HEIGHTMAP_GI_LATTICE   16   // sqrt of dirs (16 x 16 zenith/azimuth lattice)
#define HEIGHTMAP_GI_MARCH_START_M 4.0f
#define HEIGHTMAP_GI_MARCH_MAX_M   3000.0f
#define HEIGHTMAP_GI_BLOCK_EPS_M   0.25f // slab epsilon above the ray
#define HEIGHTMAP_GI_LIFT_M        0.5f  // origin lift above the rendered surface
#define HEIGHTMAP_GI_WATER_MAX_M   0.5f  // cells below this get the flat water ambient
#define HEIGHTMAP_GI_CHUNK_ROWS    16   // bake rows per chunk; yields to grids between chunks

// Fast-reject margins. The march only asks "is this direction blocked at all"
// (boolean: no soft shadow, no distance term), so each step first tests the
// coarse MAX of the lattice samples around the step point and only calls the
// exact heightAt when the coarse max is within the margin below the ray.
// The coarse max under-estimates the true cell max by the in-cell curvature
// excess — a SLOPE never causes under-estimation (a monotone ramp peaks at a
// lattice corner), only a feature narrower than the lattice cell does. The
// Azgaar height field is C1-smooth (bicubic FMG over ~40 m pixels, fBm with
// wavelengths >= 64 m, settlement plateau smoothstep bands), so:
//   fine grid, 4 m lattice (in-tile): excess <= ~0.2 m -> margin 0.5 m
//   region grid, 32 m lattice (halo): excess <= ~2 m   -> margin 3 m
// A missed reject can only ADD exact calls (never over-report occlusion); a
// margin that is too small can only MISS a shadow within a band ~1 lattice
// cell wide at the foot of a steep feature (subtle on an ambient term).
#define HEIGHTMAP_GI_FINE_REJECT_M   0.5f
#define HEIGHTMAP_GI_REGION_REJECT_M 3.0f
#define HEIGHTMAP_GI_REGION_CELL_M   32.0f // fast-reject lattice spacing (halo)

// Upper-hemisphere sample table + its per-direction sky colour, filled once
// (deterministic, no RNG). Equal-area stratification over a 16 x 16 lattice:
// cos(theta) uniform in (0,1) — 16 zenith bands of equal solid angle — with
// 16 azimuth slots, each jittered by the golden-ratio fractional part. The
// sky colour depends only on elevation (the sun terms are excluded, see
// heightmapGiSkyColor), so it is precomputed per direction here.
static float giDirTable[HEIGHTMAP_GI_DIRS * 3];
static float giSkyTable[HEIGHTMAP_GI_DIRS * 3];

// CPU port of skyEvaluate (c-engine/data/pak_0_engine/shaders/includes/
// sky.shader — the shared procedural sky used by the skybox and the IBL
// env-cube bake), WITHOUT the sun terms: the disc integrates to ~0 and the
// glow/lobe are sun-azimuth-dependent, so excluding them keeps the bake
// static (the directional light stays the only sun-dependent signal). The
// horizon wash is pinned to noon (sunElev = 1 → wash = 0 → clear-noon haze);
// safe while the sun is static (see plans/terrain-baked-gi.md, Risks).
// sky.shader is the source of truth — keep this in sync with it.
static void heightmapGiSkyColor(float dy, float out[3]) {
    static const float horizon[3] = {0.42f, 0.60f, 0.88f}; // clear-noon haze
    static const float zenith[3]  = {0.15f, 0.35f, 0.75f};
    static const float ground[3]  = {0.25f, 0.28f, 0.32f};

    float t = dy < 0.0f ? 0.0f : (dy > 1.0f ? 1.0f : dy);
    float m = powf(t, 0.5f); // pow curve: more range near the horizon
    out[0]  = horizon[0] + (zenith[0] - horizon[0]) * m;
    out[1]  = horizon[1] + (zenith[1] - horizon[1]) * m;
    out[2]  = horizon[2] + (zenith[2] - horizon[2]) * m;

    // Below-horizon fade (sky.shader's smoothstep(0.0, -0.15, elevation)).
    float gf = (dy + 0.15f) / 0.15f;
    if (gf < 0.0f) gf = 0.0f;
    else if (gf > 1.0f) gf = 1.0f;
    gf = gf * gf * (3.0f - 2.0f * gf);
    out[0] += (ground[0] - out[0]) * gf;
    out[1] += (ground[1] - out[1]) * gf;
    out[2] += (ground[2] - out[2]) * gf;
}

static void heightmapGiInitTables(void) {
    static const double invN   = 1.0 / (double)HEIGHTMAP_GI_DIRS;
    static const double invLat = 1.0 / (double)HEIGHTMAP_GI_LATTICE;
    static const double gr     = 0.6180339887498949; // golden-ratio conjugate
    static const double PI     = 3.14159265358979323846;

    for (u32 i = 0; i < HEIGHTMAP_GI_LATTICE; ++i) {
        for (u32 j = 0; j < HEIGHTMAP_GI_LATTICE; ++j) {
            u32   k   = i * HEIGHTMAP_GI_LATTICE + j;
            double jit = fmod((k + 0.7) * gr, 1.0);
            double c   = (k + 0.5 + (jit - 0.5)) * invN; // cos(theta), in (0,1)
            double azj = fmod((j + 0.3) * gr, 1.0);
            double az  = 2.0 * PI * ((j + 0.5 + (azj - 0.5)) * invLat);
            double s   = sqrt(1.0 - c * c);
            giDirTable[k * 3 + 0] = (float)(s * cos(az));
            giDirTable[k * 3 + 1] = (float)c;
            giDirTable[k * 3 + 2] = (float)(s * sin(az));
            heightmapGiSkyColor(giDirTable[k * 3 + 1], &giSkyTable[k * 3]);
        }
    }
}

// Conservative max of the 4 lattice samples surrounding (gx, gz) given in
// grid units [0, dim-1] (clamped at the border). For the smooth Azgaar
// height field the true in-cell max stays within a few decimetres of this
// value; the caller applies a reject margin (HEIGHTMAP_GI_*_REJECT_M).
static float giLatticeMax4(const float* grid, u32 dim, float gx, float gz) {
    if (gx < 0.0f) gx = 0.0f;
    else if (gx > (float)dim - 1.0f) gx = (float)dim - 1.0f;
    if (gz < 0.0f) gz = 0.0f;
    else if (gz > (float)dim - 1.0f) gz = (float)dim - 1.0f;
    i32 x1 = (i32)gx;
    i32 z1 = (i32)gz;
    if (x1 > (i32)dim - 2) x1 = (i32)dim - 2;
    if (z1 > (i32)dim - 2) z1 = (i32)dim - 2;
    const float* r0 = &grid[(size_t)z1 * dim + x1];
    const float* r1 = r0 + dim;
    return fmaxf(fmaxf(r0[0], r0[1]), fmaxf(r1[0], r1[1]));
}

// Build the off-tile fast-reject lattice: exact heightAt on a
// HEIGHTMAP_GI_REGION_CELL_M grid covering the tile plus a
// HEIGHTMAP_GI_MARCH_MAX_M halo (a ray from any texel reaches at most the
// halo edge). Pure function of (source, tileX, tileZ), built once per bake
// job (~64k exact calls, ~15 ms) and reused by every resumed chunk. The
// lattice extends 1 extra cell past the halo so every possible step point
// sits between two lattice points (no wide corner cells).
static void heightmapTerrainBuildGiRegion(const HeightmapSource& source,
                                          float originX,
                                          float originZ,
                                          float sizeMeters,
                                          std::vector<float>& storage,
                                          HeightmapGiRegion* out) {
    const float span = sizeMeters + 2.0f * HEIGHTMAP_GI_MARCH_MAX_M;
    const u32   dim  = (u32)(span / HEIGHTMAP_GI_REGION_CELL_M) + 2;
    storage.assign((size_t)dim * dim, 0.0f);
    out->minWX   = originX - HEIGHTMAP_GI_MARCH_MAX_M;
    out->minWZ   = originZ - HEIGHTMAP_GI_MARCH_MAX_M;
    out->invCell = 1.0f / HEIGHTMAP_GI_REGION_CELL_M;
    out->dim     = dim;
    out->h       = storage.data();
    for (u32 j = 0; j < dim; ++j) {
        const float wz = out->minWZ + (float)j * HEIGHTMAP_GI_REGION_CELL_M;
        for (u32 i = 0; i < dim; ++i) {
            const float wx = out->minWX + (float)i * HEIGHTMAP_GI_REGION_CELL_M;
            storage[(size_t)j * dim + i] = source.heightAt(source.userData, wx, wz);
        }
    }
}

// Baked GI for one tile. Builder thread, lock released. `heights` is the
// caller's lock-protected snapshot of the tile's published fine grid;
// `out` (preallocated [HEIGHTMAP_GI_DIM]^2 RGBA, alpha 255) receives rows
// [rowStart, rowEnd). Single-threaded and fixed loop order, so a re-bake of
// the same (source, tile) is bit-identical regardless of chunking.
static void heightmapTerrainBakeGi(const HeightmapSource& source,
                                   float originX,
                                   float originZ,
                                   float sizeMeters,
                                   const std::vector<float>& heights,
                                   const HeightmapGiRegion* region,
                                   std::vector<u8>& out,
                                   u32 rowStart,
                                   u32 rowEnd) {
    heightmapGiInitTables();

    // Flat deep-water ambient: half the unoccluded hemispheric sky average.
    // Rays from underwater self-hit the surface instantly, so the march is
    // skipped (a dim open-sky ambient reads correctly under the water pass).
    float openSky[3] = {0.0f, 0.0f, 0.0f};
    for (u32 d = 0; d < HEIGHTMAP_GI_DIRS; ++d) {
        openSky[0] += giSkyTable[d * 3 + 0];
        openSky[1] += giSkyTable[d * 3 + 1];
        openSky[2] += giSkyTable[d * 3 + 2];
    }
    float deepWater[3];
    for (int c = 0; c < 3; ++c) deepWater[c] = 0.5f * openSky[c] / (float)HEIGHTMAP_GI_DIRS;

    const u32   dim       = HEIGHTMAP_GI_DIM;
    const float giStep    = sizeMeters / (float)dim;                    // metres per GI texel
    const float gridScale = (float)(HEIGHTMAP_TEX - 1) / (float)dim;    // GI texel centre -> fine-grid coord
    const float fineScale = (float)(HEIGHTMAP_TEX - 1) / sizeMeters;    // world metres -> fine-grid coord
    const float maxX      = originX + sizeMeters;
    const float maxZ      = originZ + sizeMeters;

    for (u32 gz = rowStart; gz < rowEnd && gz < dim; ++gz) {
        const float wz    = originZ + (float)(gz + 0.5f) * giStep;
        const float gridZ = (float)(gz + 0.5f) * gridScale;
        u8*         row   = &out[(size_t)gz * dim * 4];

        for (u32 gx = 0; gx < dim; ++gx) {
            const float wx = originX + (float)(gx + 0.5f) * giStep;
            // The exact rendered surface (bilinear of the fine grid) + lift.
            const float oy =
                heightmapGridBilinear(heights.data(), HEIGHTMAP_TEX, (float)(gx + 0.5f) * gridScale, gridZ) + HEIGHTMAP_GI_LIFT_M;
            u8* px = &row[(size_t)gx * 4];

            if (oy < HEIGHTMAP_GI_WATER_MAX_M) {
                px[0] = (u8)(deepWater[0] * 255.0f + 0.5f);
                px[1] = (u8)(deepWater[1] * 255.0f + 0.5f);
                px[2] = (u8)(deepWater[2] * 255.0f + 0.5f);
                px[3] = 255;
                continue;
            }

            float acc[3] = {0.0f, 0.0f, 0.0f};
            for (u32 d = 0; d < HEIGHTMAP_GI_DIRS; ++d) {
                const float dx = giDirTable[d * 3 + 0];
                const float dy = giDirTable[d * 3 + 1];
                const float dz = giDirTable[d * 3 + 2];

                // Geometric march: t from 4 m, step *= 1.5, up to 3000 m
                // (beyond the tile by ~1 km in the worst direction — the
                // source is global). Each step is a fast-reject test: the
                // coarse max of the lattice samples around the step point
                // rejects the (expensive) exact call while it sits well
                // below the ray; the exact heightAt then decides (and
                // resolves the coarse check's false positives). In-tile
                // steps use the tile's own 4 m fine grid (exact, already in
                // memory); out-of-tile steps use the 32 m region lattice.
                float t    = HEIGHTMAP_GI_MARCH_START_M;
                float step = HEIGHTMAP_GI_MARCH_START_M;
                bool  blocked = false;
                while (t <= HEIGHTMAP_GI_MARCH_MAX_M) {
                    const float sx = wx + dx * t;
                    const float sz = wz + dz * t;
                    const float sy = oy + dy * t;
                    float        coarse;
                    if (sx >= originX && sx <= maxX && sz >= originZ && sz <= maxZ) {
                        coarse = giLatticeMax4(heights.data(), HEIGHTMAP_TEX,
                                               (sx - originX) * fineScale,
                                               (sz - originZ) * fineScale);
                        if (coarse <= sy + HEIGHTMAP_GI_BLOCK_EPS_M - HEIGHTMAP_GI_FINE_REJECT_M) {
                            t += step;
                            step *= 1.5f;
                            continue;
                        }
                    } else {
                        coarse = giLatticeMax4(region->h, region->dim,
                                               (sx - region->minWX) * region->invCell,
                                               (sz - region->minWZ) * region->invCell);
                        if (coarse <= sy + HEIGHTMAP_GI_BLOCK_EPS_M - HEIGHTMAP_GI_REGION_REJECT_M) {
                            t += step;
                            step *= 1.5f;
                            continue;
                        }
                    }
                    if (source.heightAt(source.userData, sx, sz) > sy + HEIGHTMAP_GI_BLOCK_EPS_M) {
                        blocked = true;
                        break;
                    }
                    t += step;
                    step *= 1.5f;
                }
                if (!blocked) {
                    acc[0] += giSkyTable[d * 3 + 0];
                    acc[1] += giSkyTable[d * 3 + 1];
                    acc[2] += giSkyTable[d * 3 + 2];
                }
            }

            const float inv = 1.0f / (float)HEIGHTMAP_GI_DIRS;
            for (int c = 0; c < 3; ++c) {
                float v = acc[c] * inv;
                if (v < 0.0f) v = 0.0f;
                else if (v > 1.0f) v = 1.0f;
                px[c] = (u8)(v * 255.0f + 0.5f);
            }
            px[3] = 255;
        }
    }
}

// Enqueue a job for an EMPTY tile and make sure the builder is running.
// Caller holds heightmapLock.
static void heightmapTerrainQueueJobLocked(HeightmapTerrain* ht, HeightmapTile* tile) {
    if (!ht->registered || tile->state != HEIGHTMAP_TILE_EMPTY) return;

    // Kind-scoped dedup: a stale GI job must never block the grids job of a
    // re-created tile at the same coordinates (the stale GI job is simply
    // discarded at claim time when its tile is not READY).
    for (u32 i = 0; i < buildQueue.size(); ++i) {
        HeightmapJob* job = &buildQueue[i];
        if (!job->gi && job->ht == ht && job->tileX == tile->tileX && job->tileZ == tile->tileZ) return;
    }

    HeightmapJob job = {.ht = ht, .tileX = tile->tileX, .tileZ = tile->tileZ, .gi = false, .giHeights = {}, .giRegion = {}, .giPartial = {}, .giRow = 0, .giMs = 0, .giOwned = false};
    buildQueue.push_back(job);

    if (!workerStarted) {
        workerStarted = true;
        buildWorker   = utils::threadNew(heightmapBuildThreadMain, nullptr);
    }
    utils::threadSignal(&heightmapLock);
}

// Enqueue a GI bake job for a just-READY-published tile (progressive: the
// bake runs seconds after the tile is usable and never gates READY).
// Caller holds heightmapLock.
static void heightmapTerrainQueueGiJobLocked(HeightmapTerrain* ht, HeightmapTile* tile) {
    if (heightmapGiDisabled()) return;

    for (u32 i = 0; i < buildQueue.size(); ++i) {
        HeightmapJob* job = &buildQueue[i];
        if (job->gi && job->ht == ht && job->tileX == tile->tileX && job->tileZ == tile->tileZ) return;
    }

    buildQueue.push_back(HeightmapJob{.ht = ht, .tileX = tile->tileX, .tileZ = tile->tileZ, .gi = true, .giHeights = {}, .giRegion = {}, .giPartial = {}, .giRow = 0, .giMs = 0, .giOwned = false});
    utils::threadSignal(&heightmapLock);
}

static void* heightmapBuildThreadMain(void* _) {
    (void)_;
    utils::threadSetName("heightmapBuild");

    for (;;) {
        utils::threadLock(&heightmapLock);
        while (static_cast<i32>(buildQueue.size()) == 0) utils::threadWait(&heightmapLock);

        // Pop and claim in the same critical section so destroyData cannot
        // unregister/free the tile between the pop and the claim.
        // Grids (streaming-critical) always run ahead of GI bakes:
        // without this, the GI jobs queued at spawn would sit in the FIFO
        // and stall new-tile grids — and thus tile streaming — for minutes
        // while the player moves.
        i64 sel = 0;
        for (u32 i = 0; i < buildQueue.size(); ++i) {
            if (!buildQueue[i].gi) {
                sel = i;
                break;
            }
        }
        HeightmapJob job = std::move(buildQueue[sel]);
        if (sel != static_cast<i64>(static_cast<i32>(buildQueue.size()) - 1)) buildQueue[sel] = std::move(buildQueue.back());
        buildQueue.pop_back();
        HeightmapTerrain* ht = job.ht;
        HeightmapTile*    tile = nullptr;
        bool              claimed = false;
        // GI jobs snapshot the published grid HERE (under the lock): the
        // chunk runs lock-free and a READY tile may be evicted (freed)
        // mid-bake, so the bake must not dereference the tile pointer.
        if (ht->registered) {
            tile = heightmapTerrainFindTile(ht, job.tileX, job.tileZ);
            if (job.gi) {
                bool resumable = tile && tile->state == HEIGHTMAP_TILE_READY && !tile->heights.empty() && !tile->giReady;
                if (job.giOwned) {
                    claimed = resumable; // resumed chunk: snapshot + partial ride in the job
                } else if (resumable) {
                    job.giHeights.assign(tile->heights.begin(), tile->heights.end());
                    job.giOwned = true;
                    ht->inFlight++;
                    claimed = true;
                }
            } else if (tile && tile->state == HEIGHTMAP_TILE_EMPTY) {
                tile->state   = HEIGHTMAP_TILE_GENERATING;
                ht->inFlight++;
                claimed       = true;
            }
            if (!claimed && job.giOwned) ht->inFlight--; // dropped yielded chunk (stale)
        } else if (job.giOwned) {
            ht->inFlight--;
        }
        utils::threadUnlock(&heightmapLock);

        if (!claimed) continue;

        if (job.gi) {
            // Phase 4 bake: heavy and lock-free (touches only the snapshot
            // in the job and ht->source, both valid while owned). Chunked:
            // after each chunk the worker re-checks the queue and yields
            // (requeues) while grids work is pending, so streaming is never
            // blocked for a full bake. Rows are independent and the
            // per-texel math is fixed-order, so resuming from a row offset
            // stays bit-identical. The tile may have been evicted and
            // re-created at these coordinates — re-validate under the lock
            // before publishing (a re-created READY tile is safe to publish
            // to: the bake is a pure function of (source, tileX, tileZ)).
            const u32 dim = HEIGHTMAP_GI_DIM;
            if (job.giPartial.empty()) job.giPartial.assign((size_t)dim * dim * 4, 0);
            const u32 rowEnd = (u32)std::min((i32)dim, (i32)job.giRow + (i32)HEIGHTMAP_GI_CHUNK_ROWS);

            const double t0   = utils::nanos();
            const float  oX   = (float)job.tileX * ht->tileSizeMeters;
            const float  oZ   = (float)job.tileZ * ht->tileSizeMeters;
            HeightmapGiRegion region;
            if (job.giRegion.empty()) {
                // First chunk of this bake: build the off-tile fast-reject
                // lattice (~15 ms). Pure function of (source, tileX, tileZ),
                // so a resumed chunk of an evicted/re-created tile reuses it
                // safely (same argument as the height snapshot).
                heightmapTerrainBuildGiRegion(ht->source, oX, oZ, ht->tileSizeMeters, job.giRegion, &region);
            } else {
                // Requeue std::moves the job (and its vector), so rebind the
                // storage pointer; the params are derived, not stored.
                const float span = ht->tileSizeMeters + 2.0f * HEIGHTMAP_GI_MARCH_MAX_M;
                region.minWX   = oX - HEIGHTMAP_GI_MARCH_MAX_M;
                region.minWZ   = oZ - HEIGHTMAP_GI_MARCH_MAX_M;
                region.invCell = 1.0f / HEIGHTMAP_GI_REGION_CELL_M;
                region.dim     = (u32)(span / HEIGHTMAP_GI_REGION_CELL_M) + 2;
                region.h       = job.giRegion.data();
            }
            heightmapTerrainBakeGi(ht->source, oX, oZ, ht->tileSizeMeters, job.giHeights, &region, job.giPartial, job.giRow, rowEnd);
            job.giRow = rowEnd;
            job.giMs += (utils::nanos() - t0) / 1e6;

            utils::threadLock(&heightmapLock);
            HeightmapTile* t2   = heightmapTerrainFindTile(ht, job.tileX, job.tileZ);
            bool           valid = ht->registered && t2 && t2->state == HEIGHTMAP_TILE_READY && !t2->heights.empty() && !t2->giReady;
            bool           gridsPending = false;
            for (const HeightmapJob& j : buildQueue) {
                if (!j.gi) {
                    gridsPending = true;
                    break;
                }
            }
            if (valid && rowEnd >= dim) {
                t2->gi      = std::move(job.giPartial);
                t2->giReady = true;
                ht->inFlight--;
                utils::threadUnlock(&heightmapLock);
                utils::info("heightmapTerrain: tile(%d,%d) gi baked in %.1f ms", job.tileX, job.tileZ, (float)job.giMs);
            } else if (valid) {
                // Incomplete: run the next chunk later — right now if no
                // grids work is pending, soon after it if there is.
                if (gridsPending) {
                    utils::info("heightmapTerrain: tile(%d,%d) gi yielding to grids at row %u/%u",
                                job.tileX,
                                job.tileZ,
                                rowEnd,
                                dim);
                }
                buildQueue.push_back(std::move(job));
                utils::threadUnlock(&heightmapLock);
            } else {
                ht->inFlight--;
                utils::threadUnlock(&heightmapLock);
                utils::info("heightmapTerrain: tile(%d,%d) gi discarded after %.1f ms (stale)", job.tileX, job.tileZ, (float)job.giMs);
            }
            continue;
        }

        heightmapTerrainGenerateGrids(ht, tile);

        utils::threadLock(&heightmapLock);
        ht->inFlight--;
        bool published = (tile->state == HEIGHTMAP_TILE_GENERATING);
        if (published) {
            tile->state        = HEIGHTMAP_TILE_READY;
            tile->readyStamp   = ++heightmapReadyCounter;
            ht->tilesReady++;
            ht->generatedTiles++;
            heightmapTerrainSeamCheck(ht, tile);
            // Phase 4: the GI bake must not gate readiness — queue it and
            // let the worker run it after the remaining grid work.
            heightmapTerrainQueueGiJobLocked(ht, tile);
        } else {
            // Evicted or destroyed while generating: discard the grids.
            tile->heights.clear();
            tile->physicsHeights.clear();
            tile->state          = HEIGHTMAP_TILE_EMPTY;
        }
        u32 ready           = ht->tilesReady;
        u32 resident        = static_cast<u32>(static_cast<i32>(ht->tiles.size()));
        u32 window          = ht->windowSize;
        u32 seams           = ht->seamFailures;
        u64 total           = ht->generatedTiles;
        i32 logTx           = tile->tileX;
        i32 logTz           = tile->tileZ;
        double logMs        = tile->genMs;
        bool  logSeamsDirty = (seams > 0);
        utils::threadUnlock(&heightmapLock);

        if (published) {
            utils::info("heightmapTerrain: tile(%d,%d) ready in %.1f ms (ready=%u/%u resident=%u total=%llu)",
                 logTx,
                 logTz,
                 static_cast<float>(logMs),
                 ready,
                 window * window,
                 resident,
                 (unsigned long long)total);
        } else {
            utils::info("heightmapTerrain: tile(%d,%d) discarded after %.1f ms (evicted while generating)",
                 logTx,
                 logTz,
                 static_cast<float>(logMs));
        }
        if (logSeamsDirty) {
            utils::warn("heightmapTerrain: %u seam mismatch(es) so far", seams);
        }
    }
    return nullptr;
}

// ── Window / LRU (main thread or builder via updateWindow) ─────────────────

void heightmapTerrainUpdateWindow(HeightmapTerrain* ht, float anchorX, float anchorZ) {
    if (!ht || !ht->initialized) return;

    i32 cx   = heightmapWorldToTileCoord(ht, anchorX);
    i32 cz   = heightmapWorldToTileCoord(ht, anchorZ);
    i32 half = static_cast<i32>(ht->windowSize / 2);

    utils::threadLock(&heightmapLock);

    // Mark the window and create missing tiles.
    for (i32 z = cz - half; z <= cz + half; ++z) {
        for (i32 x = cx - half; x <= cx + half; ++x) {
            HeightmapTile* tile = heightmapTerrainFindTile(ht, x, z);
            if (!tile) tile = heightmapTerrainCreateTile(ht, x, z);
            tile->inWindow = true;
            tile->lruStamp = ++ht->lruCounter;
        }
    }

    // Queue generation for in-window tiles that have no data yet. Must run
    // before the eviction pass below, which clears the inWindow flags.
    // Queue nearest-to-anchor first so the tile under the player (and thus its
    // Jolt heightfield body) is generated and created before the outer ring,
    // which is what the character controller needs to stand on at spawn.
    {
        HeightmapTile* pending[64];
        u32           pendingCount = 0;
        for (HeightmapTile* tile : ht->tiles) {
            if (tile->inWindow && tile->state == HEIGHTMAP_TILE_EMPTY && pendingCount < 64) {
                pending[pendingCount++] = tile;
            }
        }
        // Stable-ish insertion sort by squared distance to the anchor tile.
        for (u32 i = 1; i < pendingCount; ++i) {
            HeightmapTile* key = pending[i];
            float         keyD = static_cast<float>(key->tileX - cx) * static_cast<float>(key->tileX - cx)
                                + static_cast<float>(key->tileZ - cz) * static_cast<float>(key->tileZ - cz);
            u32         j      = i;
            while (j > 0) {
                float prevD = static_cast<float>(pending[j - 1]->tileX - cx) * static_cast<float>(pending[j - 1]->tileX - cx)
                             + static_cast<float>(pending[j - 1]->tileZ - cz) * static_cast<float>(pending[j - 1]->tileZ - cz);
                if (prevD <= keyD) break;
                pending[j] = pending[j - 1];
                --j;
            }
            pending[j] = key;
        }
        for (u32 i = 0; i < pendingCount; ++i) {
            heightmapTerrainQueueJobLocked(ht, pending[i]);
        }
    }

    // Evict everything outside the window. GENERATING tiles stay resident
    // until the builder finishes (it may hold their buffers); they are
    // evicted by the next update.
    for (i32 i = static_cast<i32>(static_cast<i32>(ht->tiles.size())) - 1; i >= 0; --i) {
        HeightmapTile* tile = ht->tiles[i];
        if (tile->inWindow) {
            tile->inWindow = false;
            continue;
        }
        if (tile->state == HEIGHTMAP_TILE_GENERATING) continue;
        if (tile->state == HEIGHTMAP_TILE_READY) --ht->tilesReady;
        heightmapTerrainFreeTile(tile);
        ht->tiles.erase(ht->tiles.begin() + i);
    }

    i32  logCx        = (cx != ht->lastLoggedCx || cz != ht->lastLoggedCz) ? cx : -999999;
    i32  logCz        = (cx != ht->lastLoggedCx || cz != ht->lastLoggedCz) ? cz : -999999;
    u32  resident     = static_cast<u32>(static_cast<i32>(ht->tiles.size()));
    u32  ready        = ht->tilesReady;
    u32  inFlightN    = ht->inFlight;
    u64  queued       = static_cast<i32>(buildQueue.size());
    ht->lastLoggedCx  = cx;
    ht->lastLoggedCz  = cz;

    utils::threadUnlock(&heightmapLock);

    if (logCx != -999999) {
        utils::info("heightmapTerrain: window @ tile(%d,%d) anchor(%.0f,%.0f) resident=%u ready=%u inFlight=%u queued=%llu",
             logCx,
             logCz,
             anchorX,
             anchorZ,
             resident,
             ready,
             inFlightN,
             (unsigned long long)queued);
    }
}

void heightmapTerrainRequestGeneration(HeightmapTerrain* ht, i32 tileX, i32 tileZ) {
    if (!ht || !ht->initialized) return;
    utils::threadLock(&heightmapLock);
    HeightmapTile* tile = heightmapTerrainFindTile(ht, tileX, tileZ);
    if (tile) heightmapTerrainQueueJobLocked(ht, tile);
    utils::threadUnlock(&heightmapLock);
}

// ── Component lifecycle ────────────────────────────────────────────────────

void heightmapTerrainInit(HeightmapTerrain* ht,
                          const HeightmapSource* source,
                          float tileSizeMeters,
                          u32 windowSize) {
    if (!ht || !source || !source->heightAt) {
        utils::error("heightmapTerrainInit: invalid args (heightAt is required)");
        return;
    }

    heightmapTerrainDestroyData(ht);

    ht->source         = *source;
    ht->tileSizeMeters = tileSizeMeters > 0.0f ? tileSizeMeters : HEIGHTMAP_TILE_SIZE_M;
    ht->windowSize     = windowSize < 3 ? 3 : windowSize;
    if ((ht->windowSize & 1u) == 0u) ht->windowSize++;
    ht->lruCounter     = 0;
    ht->tilesReady     = 0;
    ht->inFlight       = 0;
    ht->seamFailures   = 0;
    ht->generatedTiles = 0;
    ht->initialized    = true;
    ht->registered     = true;
    ht->lastLoggedCx   = INT32_MIN;
    ht->lastLoggedCz   = INT32_MIN;
}

void heightmapTerrainDestroyData(HeightmapTerrain* ht) {
    if (!ht) return;

    utils::threadLock(&heightmapLock);
    ht->registered = false;
    // Drop pending jobs for this instance. A yielded (requeued) GI chunk
    // owns an inFlight slot that will never be released otherwise.
    for (i32 i = static_cast<i32>(static_cast<i32>(buildQueue.size())) - 1; i >= 0; --i) {
        if (buildQueue[i].ht == ht) {
            if (buildQueue[i].giOwned) ht->inFlight--;
            buildQueue[i] = std::move(buildQueue.back());
            buildQueue.pop_back();
        }
    }
    utils::threadUnlock(&heightmapLock);

    // Wait for in-flight generation to finish (the builder holds no lock
    // while generating, so we poll).
    for (;;) {
        utils::threadLock(&heightmapLock);
        bool busy = ht->inFlight > 0;
        utils::threadUnlock(&heightmapLock);
        if (!busy) break;
        utils::gotoSleepNS(1000000); // 1 ms
    }

    for (i32 i = static_cast<i32>(static_cast<i32>(ht->tiles.size())) - 1; i >= 0; --i) {
        heightmapTerrainFreeTile(ht->tiles[i]);
    }
    ht->tiles.clear();
    ht->tilesReady   = 0;
    ht->lruCounter   = 0;
    ht->inFlight     = 0;
    ht->initialized  = false;
}

// ── Coordinates ────────────────────────────────────────────────────────────

i32 heightmapWorldToTileCoord(const HeightmapTerrain* ht, float worldCoord) {
    return static_cast<i32>(floorf(worldCoord / ht->tileSizeMeters));
}

void heightmapTileToWorldOrigin(const HeightmapTerrain* ht,
                                i32 tileX,
                                i32 tileZ,
                                float* outOriginX,
                                float* outOriginZ) {
    if (outOriginX) *outOriginX = static_cast<float>(tileX) * ht->tileSizeMeters;
    if (outOriginZ) *outOriginZ = static_cast<float>(tileZ) * ht->tileSizeMeters;
}

// ── Sampling ───────────────────────────────────────────────────────────────

float heightmapTerrainSample(const HeightmapTerrain* ht, float wx, float wz) {
    if (!ht || !ht->initialized) return 0.0f;

    utils::threadLock(&heightmapLock);
    float y    = 0.0f;
    bool  have = false;
    if (ht->registered) {
        HeightmapTile* tile =
            heightmapTerrainFindTile(const_cast<HeightmapTerrain*>(ht),
                                     heightmapWorldToTileCoord(ht, wx),
                                     heightmapWorldToTileCoord(ht, wz));
        if (tile && tile->state == HEIGHTMAP_TILE_READY && !tile->heights.empty()) {
            // Grid coords in [0, TEX-1]; the grid spans the full tile edge.
            float gx = (wx - tile->originX) / tile->sizeMeters * static_cast<float>(HEIGHTMAP_TEX - 1);
            float gz = (wz - tile->originZ) / tile->sizeMeters * static_cast<float>(HEIGHTMAP_TEX - 1);
            y        = heightmapGridBilinear(tile->heights.data(), HEIGHTMAP_TEX, gx, gz);
            have     = true;
        }
    }
    utils::threadUnlock(&heightmapLock);

    return have ? y : ht->source.heightAt(ht->source.userData, wx, wz);
}

// ── Phase 3: Jolt heightfield collision ────────────────────────────────────
// One static heightfield body per READY tile, built from the tile's physics
// grid (HEIGHTMAP_PHYSICS_PSN^2 samples, spacing size/(PSN-1) — the SAME
// lattice the ring-0 render lattice uses, so the walked surface is the
// rendered surface). Bodies are created on the main thread (the Jolt world
// is not thread-safe) with a per-frame budget, nearest to the anchor first,
// and destroyed with the tile on eviction (heightmapTerrainFreeTile).

#define HEIGHTMAP_PHYSICS_BODIES_PER_FRAME 8

struct HeightmapPhysicsPending {
    HeightmapTile* tile;
    float          dist2;
};

static int heightmapPhysicsPendingCompare(const void* a, const void* b) {
    float da = static_cast<const HeightmapPhysicsPending*>(a)->dist2;
    float db = static_cast<const HeightmapPhysicsPending*>(b)->dist2;
    return (da > db) - (da < db);
}

static void heightmapTerrainSyncPhysics(HeightmapTerrain* ht, float anchorX, float anchorZ) {
    if (!physicsSystemJoltActive()) return;

    // Collect READY tiles without a body (under the lock; the builder thread
    // only publishes grids, it never touches Jolt).
    HeightmapPhysicsPending pending[64];
    u32                     count = 0;

    utils::threadLock(&heightmapLock);
    if (ht->registered) {
        for (HeightmapTile* tile : ht->tiles) {
            if (count >= 64) break;
            if (tile->state != HEIGHTMAP_TILE_READY || tile->physicsHeights.empty()) continue;
            if (tile->physicsData) continue;
            float dx = tile->originX + tile->sizeMeters * 0.5f - anchorX;
            float dz = tile->originZ + tile->sizeMeters * 0.5f - anchorZ;
            pending[count].tile  = tile;
            pending[count].dist2 = dx * dx + dz * dz;
            count++;
        }
    }
    utils::threadUnlock(&heightmapLock);

    if (count == 0) return;
    qsort(pending, count, sizeof(pending[0]), heightmapPhysicsPendingCompare);

    static int hitchOn = -1;
    if (hitchOn < 0) hitchOn = getenv("ENGINE_HITCH_DEBUG") != nullptr;
    double joltT0 = utils::nanos();
    u32 created = 0;
    for (u32 i = 0; i < count && created < HEIGHTMAP_PHYSICS_BODIES_PER_FRAME; i++) {
        HeightmapTile* tile = pending[i].tile;

        // Sample (x, y) sits at world offset + scale * (x, h, y): anchor the
        // grid's first sample at the tile's min corner with the physics grid's
        // spacing, so the heightfield surface coincides with the CPU grid and
        // the ring-0 render lattice (255 segments over the same 255 intervals).
        float pos[3]    = {0.0f, 0.0f, 0.0f};
        float rot[4]    = {0.0f, 0.0f, 0.0f, 1.0f};
        float offset[3] = {tile->originX, 0.0f, tile->originZ};
        float spacing   = tile->sizeMeters / static_cast<float>(HEIGHTMAP_PHYSICS_PSN - 1);
        float scale[3]  = {spacing, 1.0f, spacing};

        JoltHeightMap* hm =
            joltCreateHeightShapeNoFile(tile->physicsHeights.data(), pos, rot, offset, scale, HEIGHTMAP_PHYSICS_PSN);
        if (!hm) {
            utils::warn("heightmapTerrain: Jolt heightfield creation failed for tile(%d,%d)", tile->tileX, tile->tileZ);
            continue;
        }
        tile->physicsData = hm;
        created++;
    }
    if (hitchOn && created > 0) utils::info("HITCH: jolt heightfields: %u bodies in %.1f ms", created, (utils::nanos() - joltT0) / 1e6);
    if (created > 0) {
        utils::info("heightmapTerrain: created %u heightfield bod%s this frame (%u pending)",
             created,
             created == 1 ? "y" : "ies",
             count);
    }
}

// ── System ─────────────────────────────────────────────────────────────────
// Tracks the streaming window around the camera. Runs after the camera system
// so the active camera transform is final for the frame. Does nothing while
// no world has an active heightmap terrain (e.g. regular-mesh worlds).

void HeightmapTerrainSystem::added() {}
void HeightmapTerrainSystem::removed() {}
void HeightmapTerrainSystem::preUpdate() {}
void HeightmapTerrainSystem::postUpdate() {}

void HeightmapTerrainSystem::update() {
    HeightmapTerrain* ht = heightmapTerrainGetActive();
    if (!ht || !ht->initialized) return;

    Entity* camera = cameraGetEntity();
    if (!camera) return;
    Transform* transform = getComponent(camera->scene, Transform, camera->id);
    if (!transform) return;

    heightmapTerrainUpdateWindow(ht, transform->pos[0], transform->pos[2]);

    // Phase 3: keep the Jolt heightfield bodies in sync with the window.
    heightmapTerrainSyncPhysics(ht, transform->pos[0], transform->pos[2]);
}

HeightmapTerrainSystem heightmapTerrainSystem;

HeightmapTerrainSystem::HeightmapTerrainSystem() : System("heightmapTerrain") {}
}  // namespace engine
