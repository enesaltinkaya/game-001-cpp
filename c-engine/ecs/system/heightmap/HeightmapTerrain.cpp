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
struct HeightmapJob {
    HeightmapTerrain* ht;
    i32 tileX, tileZ;
};

static HeightmapTerrain* activeHeightmapTerrain = nullptr;
static u64 heightmapReadyCounter = 0; // global; bumped per READY publish

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

// Enqueue a job for an EMPTY tile and make sure the builder is running.
// Caller holds heightmapLock.
static void heightmapTerrainQueueJobLocked(HeightmapTerrain* ht, HeightmapTile* tile) {
    if (!ht->registered || tile->state != HEIGHTMAP_TILE_EMPTY) return;

    for (u32 i = 0; i < buildQueue.size(); ++i) {
        HeightmapJob* job = &buildQueue[i];
        if (job->ht == ht && job->tileX == tile->tileX && job->tileZ == tile->tileZ) return;
    }

    HeightmapJob job = {.ht = ht, .tileX = tile->tileX, .tileZ = tile->tileZ};
    buildQueue.push_back(job);

    if (!workerStarted) {
        workerStarted = true;
        buildWorker   = utils::threadNew(heightmapBuildThreadMain, nullptr);
    }
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
        HeightmapJob   job   = buildQueue[0];
        HeightmapTerrain* ht = job.ht;
        HeightmapTile* tile  = nullptr;
        bool           claimed = false;
        buildQueue[0] = buildQueue.back();
        buildQueue.pop_back();

        if (ht->registered) {
            tile = heightmapTerrainFindTile(ht, job.tileX, job.tileZ);
            if (tile && tile->state == HEIGHTMAP_TILE_EMPTY) {
                tile->state   = HEIGHTMAP_TILE_GENERATING;
                ht->inFlight++;
                claimed       = true;
            }
        }
        utils::threadUnlock(&heightmapLock);

        if (!claimed) continue;

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
    // Drop pending jobs for this instance.
    for (i32 i = static_cast<i32>(static_cast<i32>(buildQueue.size())) - 1; i >= 0; --i) {
        if (buildQueue[i].ht == ht) {
            buildQueue[i] = buildQueue.back();
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
