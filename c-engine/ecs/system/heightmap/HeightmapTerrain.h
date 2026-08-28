#pragma once
#include "ecs/system/System.h"

#include <vector>
#include "ecs/system/scene/SceneSystem.h"
#include "ecs/system/heightmap/HeightmapSource.h"

/*
 * HeightmapTerrain
 * ----------------
 * Streaming heightmap terrain service for generated worlds (e.g. the 80 km
 * Azgaar map). The surface is NOT a mesh: it is a set of square tiles whose
 * height data is generated at runtime from a deterministic HeightmapSource.
 *
 * Per tile (see plans/heightmap-terrain.md):
 *   - CPU height grid, final metres            [phase 1, done]
 *   - CPU physics subsample grid               [phase 1, done]
 *   - GPU height (R32F) texture                [phase 2, renderer pass]
 *   - Jolt heightfield collision body          [phase 3]
 *
 * Nothing is written to disk: evicted tiles are regenerated bit-identically
 * from the source (macro heights + seeded world-anchored fBm detail).
 *
 * The component is attached to a scene entity by the game (one instance per
 * world). heightmapTerrainSystem tracks the streaming window around the
 * camera anchor; O(1) access to the live instance goes through
 * heightmapTerrainGetActive().
 */

#define HEIGHTMAP_TILE_SIZE_M   2048.0f // default tile edge (metres)
#define HEIGHTMAP_WINDOW_SIZE   5       // default window (tiles per side, odd)

// 512^2 height grid per 2048 m tile ~= 4 m texels (f32 on the GPU and CPU).
// The grid spans the FULL tile edge including both border endpoints:
// sample i sits at origin + i * size / (TEX - 1), so neighbouring tiles share
// identical border columns (watertight) and the grid matches a texel-centre
// texture addressed as uv = (local/size) * (TEX-1)/TEX + 0.5/TEX.
// 256 physics samples = ~8 m, endpoints shared the same way.
#define HEIGHTMAP_TEX           512
#define HEIGHTMAP_PHYSICS_PSN   256

// Phase 4 (baked GI): per-tile sky-visibility irradiance, 128^2 RGBA u8
// (16 m per texel over the 2048 m tile; the alpha channel is always 255 —
// R8G8B8 packed is not supported as a linear-filterable sampled image by
// some drivers). Texel-centred, bilinear — see plans/terrain-baked-gi.md.
#define HEIGHTMAP_GI_DIM 128

namespace engine {
enum HeightmapTileState {
    HEIGHTMAP_TILE_EMPTY = 0,  // allocated, no data (phase-0 terminal state)
    HEIGHTMAP_TILE_GENERATING, // background thread filling grids (phase 1)
    HEIGHTMAP_TILE_READY,      // CPU + GPU + physics data valid (phases 1-3)
};

struct HeightmapTile {
    i32  tileX, tileZ;          // world tile coordinates
    float originX, originZ;     // world position of the tile's min corner
    float sizeMeters;           // tile edge (== HeightmapTerrain tile size)
    HeightmapTileState state;
    bool inWindow;              // set by updateWindow while inside the window

    // Phase 1: CPU grids (allocated when generation completes).
    std::vector<float> heights = {};        // [HEIGHTMAP_TEX]^2, row-major, metres
    std::vector<float> physicsHeights = {}; // [HEIGHTMAP_PHYSICS_PSN]^2
    double genMs;          // last generation duration (stats)

    // Bumped (global counter) every time this tile's grids are (re)published
    // as READY. The renderer pass uses it as a cache key so a regenerated
    // tile is re-uploaded instead of reusing stale GPU data.
    u64 readyStamp;

    // Phase 2: renderer backend data (Vulkan height texture).
    void* gpuData;

    // Phase 3: Jolt heightfield body.
    void* physicsData;

    // Phase 4: baked GI — sky-visibility irradiance as the terrain sees it
    // ([HEIGHTMAP_GI_DIM]^2 RGBA u8, alpha = 255). Baked on the builder
    // thread shortly after the grids go READY; freed with the tile.
    std::vector<u8> gi = {};
    bool giReady;

    u64 lruStamp;
};

struct HeightmapTerrain {
    HeightmapSource source;        // vtable + userData (copied at init)
    float tileSizeMeters;
    u32   windowSize;              // tiles per side (forced odd)
    std::vector<HeightmapTile*> tiles;   // resident tiles
    u64  lruCounter;
    u32  tilesReady;               // tiles in HEIGHTMAP_TILE_READY
    u32  inFlight;                 // builder jobs currently executing (this ht)
    bool registered;               // builder may process jobs for this instance
    u32  seamFailures;             // cumulative border-mismatch warnings
    u64  generatedTiles;           // lifetime counter (this instance)
    bool initialized;
    i32  lastLoggedCx, lastLoggedCz; // window log throttling
};

REGISTER_COMPONENT(HeightmapTerrain);

class HeightmapTerrainSystem : public System {
public:
    HeightmapTerrainSystem();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern HeightmapTerrainSystem heightmapTerrainSystem;

// Configure the component (idempotent: frees any previously resident tiles).
// source may later become invalid only after heightmapTerrainDestroyData.
void heightmapTerrainInit(HeightmapTerrain* ht,
                          const HeightmapSource* source,
                          float tileSizeMeters,
                          u32 windowSize);

// Free all resident tile data. The component struct itself is owned by the
// scene sparse set and survives (e.g. until the scene is destroyed).
void heightmapTerrainDestroyData(HeightmapTerrain* ht);

// Set/clear (NULL) the globally active instance, used by the engine system
// and by O(1) consumers (renderer backend, physics, game systems).
void heightmapTerrainSetActive(HeightmapTerrain* ht);
HeightmapTerrain* heightmapTerrainGetActive(void);

// World <-> tile coordinate helpers.
i32 heightmapWorldToTileCoord(const HeightmapTerrain* ht, float worldCoord);
void heightmapTileToWorldOrigin(const HeightmapTerrain* ht,
                                i32 tileX,
                                i32 tileZ,
                                float* outOriginX,
                                float* outOriginZ);

// Ensure the window around (anchorX, anchorZ) is resident: create missing
// tiles, refresh LRU stamps, evict out-of-window tiles, and queue generation
// for in-window EMPTY tiles on the shared background builder thread.
// Driven by heightmapTerrainSystem each frame; safe to call directly.
void heightmapTerrainUpdateWindow(HeightmapTerrain* ht, float anchorX, float anchorZ);

// Queue a single tile for background generation (deduplicated). The tile
// must exist; non-EMPTY tiles are ignored. Safe from any thread.
void heightmapTerrainRequestGeneration(HeightmapTerrain* ht, i32 tileX, i32 tileZ);

// O(n) resident-tile lookup (n <= window^2); NULL when not resident.
HeightmapTile* heightmapTerrainGetTile(HeightmapTerrain* ht, i32 tileX, i32 tileZ);

// True when a Jolt heightfield body exists for the tile containing world
// (wx, wz). Used by the player to hold the character in place until the
// ground under it exists (avoids falling through the terrain on spawn, when
// the streaming heightfields are still being generated/created).
bool heightmapTerrainHasBodyAt(const HeightmapTerrain* ht, float wx, float wz);

// Immutable copy of a READY tile's data, for consumers on other threads or
// in the renderer (which must not touch the tile table while the builder
// thread is publishing). The grid pointers stay valid until the owning tile
// is evicted on the main thread.
struct HeightmapTileView {
    i32 tileX, tileZ;
    u64 readyStamp;   // cache key (bumped on each READY publish)
    float originX, originZ;
    float sizeMeters;
    const float* heights; // [HEIGHTMAP_TEX]^2, metres
    const u8* gi;         // [giDim]^2 RGBA (alpha = 255), valid when giReady
    u32 giDim;
    bool giReady;
};

// Copy the READY tiles into outViews (up to cap entries). Safe from any
// thread. Returns the number of views written.
u32 heightmapTerrainSnapshotTiles(HeightmapTerrain* ht,
                                  HeightmapTileView* outViews,
                                  u32 cap);

// Lock-safe bulk copy of one tile's CPU height grid ([HEIGHTMAP_TEX]^2,
// metres) into outHeights. Returns true when the tile is resident and READY.
// Background consumers (e.g. grass scattering) should work on the copy: the
// view's grid pointers are invalidated as soon as the main thread evicts the
// tile.
bool heightmapTerrainCopyTile(HeightmapTerrain* ht,
                              i32 tileX,
                              i32 tileZ,
                              float* outHeights);

// Lock-safe bulk copy of one tile's physics grid ([HEIGHTMAP_PHYSICS_PSN]^2,
// metres) into outHeights. The physics grid is what the Jolt heightfield is
// built from and what the render lattice lifts, so it is the exact
// walkable/rendered ground surface; consumers that place world objects on
// the ground should sample THIS grid, not the finer CPU height grid. Same
// copy rationale as heightmapTerrainCopyTile.
bool heightmapTerrainCopyPhysicsTile(HeightmapTerrain* ht,
                                     i32 tileX,
                                     i32 tileZ,
                                     float* outHeights);

// Bilinear sample of a regular height grid spanning [0, dim-1] in both axes
// (endpoints included; coordinates are clamped to the grid). Shared by the
// physics-grid generation, heightmapTerrainSample and off-thread consumers
// that must match the rendered/physics surface exactly.
float heightmapGridBilinear(const float* grid, u32 dim, float gx, float gz);

// Height (metres) at world (wx, wz). Bilinear fast path through the CPU
// grid of a READY tile (matches the GPU/physics bilinear surface exactly);
// falls back to the source for tiles that are not READY yet.
float heightmapTerrainSample(const HeightmapTerrain* ht, float wx, float wz);
}  // namespace engine
