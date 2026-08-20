# Heightmap Terrain (Production) Implementation Plan

## Goal

Replace the experimental mesh-based Azgaar terrain with a production
**heightmap terrain** system for the generated world:

1. At runtime, generate heightmap tiles on the fly from the parsed `.map`
   file (`AzgaarWorld`) in the streaming window around the player.
2. Upload each tile's heights as a GPU texture and render it with a
   **vertex shader** that lifts a coarse implicit grid — no mesh assets,
   no index buffers, no per-LOD geometry.
3. Create a **Jolt heightfield collision body per tile** from the same
   generated heights, so the character controller walks exactly on the
   rendered surface.
4. **No heightmap data is stored on disk.** The `.map` file + a fixed
   noise seed is the single source of truth; tiles are deterministic and
   can be regenerated identically after eviction.

Target world: the 80 km × 80 km Azgaar map. A single 4k texture would be
19.5 m/texel — unusable. Tiling + streaming dissolves the scale problem
(see "Numbers" below).

---

## Current state (so the plan is self-contained)

### Data source

- `c-game/game/azgaar/AzgaarWorld.h/.c` — full `.map` parser:
  - pixel height grid (`heightGrid`, capped 2048×1024; FMG heights are
    uint8 [0,100], sea level = 20),
  - terrain cells + vertices (the "Voronoi" feature polygons, each with a
    height and biome),
  - pack cells (states/provinces), biomes table, settlements, routes,
    rivers, markers.
  - coordinate/scaling: `metersPerPixel`, `azgaarMapToWorld()`,
    `azgaarHeightToMeters()` (FMG formula `(h - 18)^heightExponent`,
    coastline blend, seabed down to −60 m),
  - height queries: `azgaarWorldSampleHeightSmooth()` (Catmull-Rom over
    the pixel grid; nearest-cell fallback).
- Loaded via `loadingAzgaarSystem`
  (`c-game/game/loadingAzgaar/LoadingAzgaar.c`), current map hardcoded to
  `azgaar/Chilerel 2026-08-11-15-35.map`.

### Experimental (to be replaced)

- `c-game/game/azgaar/AzgaarTerrain.c` — builds 2000 m tiles as **full
  meshes**: 192 subdivisions (193×193 verts, ~73 k tris/tile) with
  positions/normals/UVs/tangents, threaded build, smooth cross-tile
  normals, per-chunk `JoltMesh` via `joltCreateMeshShapeNoCache`.
- `c-engine/renderer/vulkan/pass/azgaar_terrain/VulkanAzgaarTerrainPass` +
  shaders `pak_0_engine/shaders/pass/azgaar_terrain/` — renders the mesh
  tiles (biome tint passed through the tangent channel), wireframe +
  debug-cell-color toggles, threaded upload.
- `c-game/game/azgaar/AzgaarStreaming.c` — tile window around the player
  (`AZGAAR_TILE_RADIUS 2` → 5×5 tiles).
- Consumers that sample tile heights / the tile API: `AzgaarGrass`,
  `AzgaarWater`, `AzgaarRoadCorridor`, `AzgaarRoadDecals`,
  `AzgaarCellTracker`, debug GUI, `VulkanDebugPhysicsPass`.
- Pass registration: `c-engine/renderer/vulkan/Vulkan.c`
  (`addPass(&vulkanAzgaarTerrainPass)` etc.).

### Production (stays as-is; the new system must coexist with it)

- `Terrain` component + `TerrainChunk` (glTF chunked meshes),
  `VulkanTerrainPass` + `VulkanTerrain` (mesh rendering, heightfield
  texture baked by `VulkanTerrainHeightBaker`), `terrainLoadJoltShapes`
  (`.jolt.dat` sidecars), `PhysicsSystem` (`physicsCreateMesh` from raw
  positions/indices, `physicsCreateFromBlob`).

### Jolt C wrapper (modifications allowed)

`/home/enes/Projects/c/cpp-thirdparty/jolt/wrapper/src/jolt_c_api.h`
(path in `CMakeLists.txt` via `set(thirdparty ...)`):

- **`joltCreateHeightShape(heights, pos, rot, offsetAABB, scale,
joltPath, inSampleCount, initJolt)` → `JoltHeightMap*`** already exists
  and wraps Jolt's `HeightFieldShapeSettings` (surface defined by
  `offset + scale * (x, sample, y)` for integer x,y in `[0, n)`).
  `inSampleCount / blockSize(=16) >= 2`; multiples of 16 are most
  efficient. `joltHeightMapDestroy()` removes + destroys the body.
- Quirks to fix in the wrapper (Phase 3):
  - `createHeight()` unconditionally tries `std::ofstream(joltPath)` to
    write a binary cache — harmless with an empty path, but streaming
    tiles should have an explicit no-file variant.
  - `joltCreateHeightShape` has no friction/restitution/userData params
    (always static, no material list).
  - The "fucky you bebe" error message on shape creation failure —
    replace with a real error string (heightfield creation fails loudly
    on bad sample counts; we want diagnosable logs).

---

## Design decisions & rationale

### 1. Tiles are the unit of everything (render + physics + CPU queries)

- **Tile size: 2048 m** (2 km). 80 km world → ~39×39 tiles. 2048 is a
  clean power-of-two-friendly size: 512 texture texels → exactly 4 m/texel;
  256 physics samples → exactly 8 m/sample (256 = 16×16 Jolt blocks).
- **Window: 5×5 tiles around the player** (≈10 km radius, matches the
  existing `AZGAAR_TILE_RADIUS 2`), LRU eviction; background-thread
  prefetch of the next ring so crossings never hitch.
- One tile's data feeds all consumers: vertex-shader rendering, Jolt
  collision, grass/water/road scatter, probe placement, CPU height
  queries. No more separate baked heightfield readback.

### 2. Per-tile data (the only "heightmap" that exists)

```c
typedef struct HeightmapTile {
    i32   tileX, tileZ;            // world tile coords (origin = tileX*TILE)
    u32   state;                   // EMPTY | GENERATING | READY
    // CPU: final heights in metres (macro + geometry-band noise), TEX^2
    float *heights;                // [TEX][TEX], row-major, 4 m texel
    // CPU: physics subsample (every 2nd texel), PS^2
    float *physicsHeights;         // [PS][PS] = 256^2, 8 m sample
    // GPU: R32F height texture + RGBA8 biome tint texture
    VulkanImage heightTex;         // TEX x TEX (512^2, 1 MB)
    VulkanImage tintTex;           // TEX x TEX (RGBA8)
    JoltHeightMap *jolt;           // per-tile static heightfield body
    u64   generationStamp;         // for LRU / double-buffered swap
} HeightmapTile;

#define HEIGHTMAP_TILE_SIZE_M 2048.0f
#define HEIGHTMAP_TEX         512   // 4 m texel
#define HEIGHTMAP_PHYSICS_PSN 256   // 8 m sample (every 2nd texel)
#define HEIGHTMAP_WINDOW      5     // tiles per side
```

- Heights are baked **after** `azgaarHeightToMeters()` — the texture holds
  final metres, so the vertex shader is just `y = texSample(xz)`.
- Height texture format: **`VK_FORMAT_R32F`** (float32).
  Heights stay f32 end to end: the CPU grid, Jolt (`joltCreateHeightShape`
  takes `float*`) and the GPU texture are all float32, so the tile's height
  grid is copied straight into the image with no conversion and no driver
  format probing (R32F linear filtering is guaranteed by core Vulkan).
  Costs 1 MB/tile vs 512 KB for R16F — accepted; the resident 5×5 window
  still costs only ~25 MB for heights.
- Seabed (FMG h < 20) bakes as negative metres; ocean tiles render seabed
  and the existing water system draws the surface. No special-casing.

### 3. Height function = macro (from `.map`) + fBm detail, split by frequency

The `.map`'s true resolution at 80 km is ~78 m/pixel (1024² canvas) plus
cell boundaries. Anything finer is synthesized:

- **Geometry band** (wavelength ≥ 16 m, i.e. ≥ 4 texels): baked into
  `heights[]` → visible in the mesh (ring-0 grid spacing is 16 m), in the
  8 m physics grid, and in CPU queries. One CPU fBm (value noise, ~4
  octaves, world-anchored, seeded from the map) contributes this band.
- **Micro band** (4–16 m): affects **fragment normals only** (procedural
  normal perturbation in the shader, same noise family) — perceived close
  range "roughness" without geometry or collision cost.
- This split guarantees render ≈ physics ≈ CPU query: the piecewise-linear
  physics surface and the piecewise-planar render surface both sample the
  same baked heights, and neither aliases the micro band.

Determinism: `seed = hash(mapName)`, noise is a pure function of world
(x, z). Evicted tiles regenerate bit-identical. Nothing on disk.

### 4. Rendering: implicit grid, per-ring LOD, vertex-shader lift

- No VBO/IBO: the vertex shader generates the `grid × grid` lattice from
  push constants `{tileOriginXZ, tileSize, gridSize}`.
- **Per-ring grid sizes** (all sampling the same 512² texture, bilinear):
  - ring 0 (9 tiles, closest): 128×128 → 16 m cells, 32 768 tris each
  - ring 1 (8 tiles): 64×64 → 32 m cells, 8 192 tris each
  - ring 2 (8 tiles): 32×32 → 64 m cells, 2 048 tris each
  - total ≈ 380 k tris vs the experimental ~1.8 M — and the far rings
    stay watertight because heights are world-anchored (no LOD pinning,
    no transition meshes).
- Normals: vertex shader from texture neighbor samples (±1 texel);
  fragment shader refines with the micro-band procedural normal.
- UVs: world `(xz / 128)` for tiling the ground texture (matches the
  experimental pass's tiling feel; tune in review).
- Vertex stream is **tile-local** (0…tileSize) + push-constant origin —
  keeps float32 precision clean at 80 km extents.
- Shading: port `azgaar_terrain.frag` (biome tint over grass texture,
  IBL, shadows, GTAO inputs) into the new pass; tint comes from `tintTex`
  (replaces the tangent-channel hack).
- Wireframe + debug-height-ramp toggles preserved, wired to the debug GUI
  exactly like the experimental pass.

### 5. Physics: Jolt heightfield per tile from the same heights

- `joltCreateHeightShape(physicsHeights, tileOrigin, identityRot,
offsetAABB = {tileX*TILE, 0, tileZ*TILE}, scale = {8, 1, 8},
joltPath = "" (no file), inSampleCount = 256, initJolt = 1)`.
- Heightfields are _surfaces_: no cliff walls. Accepted for v1 on rolling
  terrain (`CharacterVirtual` + slope limit handles the rest); steep-cliff
  wall colliders are a noted future extension (see Edge Cases).
- Physics window = render window (5×5) for v1 simplicity; shrinking the
  physics radius is a later optimization, not a correctness issue.
- Bodies are created/destroyed with the tile LRU (add on READY, remove on
  eviction). `CharacterVirtual` collides with them natively.
- Wrapper changes (Phase 3): add `joltCreateHeightShapeNoFile(...)`
  (explicit no-cache-file, optional friction/restitution + userData),
  real error reporting instead of the debug string.

### 6. Generic engine core + Azgaar source adapter

- Engine owns the _concept_: `c-engine/ecs/system/heightmap/`
  (`HeightmapTerrain.h/.c` — tile manager: window, LRU, generation
  thread, CPU grids, sample API, physics create/destroy hooks) and
  `c-engine/renderer/vulkan/pass/heightmap_terrain/` (pass + shaders).
- The engine calls a small vtable for world content:

```c
typedef struct HeightmapSource {
    // Final metres at world (wx, wz). Must be deterministic.
    float (*heightAt)(void* ud, float wx, float wz);
    // Optional biome/ground tint [0,1] RGB; NULL → default.
    void  (*tintAt)(void* ud, float wx, float wz, float outRGB[3]);
    void* userData;
} HeightmapSource;
```

- Game provides the Azgaar adapter (`c-game/game/azgaar/AzgaarHeightmapSource`):
  `heightAt = azgaarWorldSampleHeightSmooth → azgaarHeightToMeters + fBm
geometry band (seeded)`, `tintAt = biome color of the nearest cell`.
- This keeps the engine free of Azgaar dependencies and makes the system
  reusable for any generated world (and testable with a trivial analytic
  source in unit tests / tools).

### 7. Streaming & consumers

- Replace `azgaarStreamingSystem`'s role: a `HeightmapTerrain` ECS system
  (engine) tracks the player tile, requests the window, drives generation
  on a background thread (reuse the `AzgaarTerrain` build-thread pattern:
  lock, pending swap, `buildReady` handoff).
- Public CPU API for consumers:
  - `heightmapTerrainSample(wx, wz) → y` (uses CPU grids; falls back to
    the source function for tiles not yet READY),
  - `heightmapTerrainTintAt(wx, wz, outRGB)`,
  - `heightmapTerrainForEachReadyTile(fn, ud)` (grass/water/road scatter).
- `AzgaarGrass`, `AzgaarWater`, `AzgaarRoadCorridor/Decals` re-point
  from the old `AzgaarTileGrid`/`heightfield.heights` to this API.
  Behavior must stay visually identical.

### 8. Coexistence & cutover

- The oghuzlands (regular-mesh) world is untouched: `vulkanTerrainPass`
  - `Terrain` component keep working. In the Azgaar world, the new
    `vulkanHeightmapTerrainPass` replaces `vulkanAzgaarTerrainPass`
    (old pass deregistered/removed after cutover; `AzgaarTerrain.c` mesh
    builder deleted, `AzgaarWorld` kept).
- Cutover is gated so both can be A/B'd during development (debug GUI
  switch "terrain backend: mesh (experimental) / heightmap").

---

## Numbers (why this works at 80 km)

| Quantity                    | Value                                                                           |
| --------------------------- | ------------------------------------------------------------------------------- |
| Tile                        | 2048 m                                                                          |
| World tiles (80 km)         | ~39 × 39 = 1 521                                                                |
| Height texture per tile     | 512² R32F = 1 MB                                                                |
| Resident window (5×5)       | 25 tiles ≈ 25 MB VRAM (heights) + ~25 MB (tints)                                |
| Physics per tile            | 256² samples (Jolt-compressed blocks)                                           |
| Total triangles (LOD rings) | ≈ 380 k (vs ~1.8 M experimental)                                                |
| Tile generation cost (est.) | 512² × (Catmull-Rom grid + 4-oct fBm) ≈ 10–60 ms on one core, background thread |
| On-disk terrain data        | 0 bytes (only the `.map` file)                                                  |

---

## Implementation phases

Each phase is independently buildable (`./scripts/build.sh`) and must be
validated before the next starts.

### Phase 0 — Skeleton, source adapter, debug entry point

- New engine module `c-engine/ecs/system/heightmap/`:
  `HeightmapSource` vtable, `HeightmapTerrain` component + system
  (window tracking, tile LRU state machine, sample API with source
  fallback — no generation yet), registration in `Vulkan.c` / ECS init.
- New game adapter `c-game/game/azgaar/AzgaarHeightmapSource` (height +
  tint from `AzgaarWorld`; fBm helper with map-name seed).
- `HeightmapTerrain` exposed to ECS/scene so consumers can find it.
- **Automated-validation entry**: add a way to launch straight into the
  Azgaar world for `run.sh play` (e.g. `ENGINE_AZGAAR_WORLD=1` handled in
  `main.c`/game state, or a debug-GUI button). Without this, the
  screenshot/log flows can't reach the Azgaar world (main menu skip
  currently goes to the regular world).
- Exit: builds clean (warning-strict), `run.sh log 5000` boots into the
  Azgaar world, sample API returns sane heights (log a few probes).

### Phase 1 — CPU tile generation

- Background-thread tile builder (port `AzgaarTerrain`'s threaded-build
  pattern): fills `heights[512²]` (final metres, geometry-band fBm
  included, shared-border samples identical to neighbours → watertight)
  and `physicsHeights[256²]` (every 2nd texel).
- Seam check utility: assert border rows of a tile equal the adjacent
  tile's border rows within 1e-4 (deterministic sampling).
- Log per-tile generation time; target ≤ 60 ms (tune fBm octaves/TEX if
  slower).
- Exit: headless `run.sh play log 8000` logs a full window of 25 tiles
  generated without stutter/crash; seam check passes; heights at known
  points (a city from the `.map`) match `azgaarWorldSampleHeightSmooth`
  - fBm (spot-checked in log).

### Phase 2 — GPU: height textures + vertex-shader pass

- Per-tile `VulkanImage` height (R32F, 512², bilinear) + tint
  (R8G8B8A8_UNORM, 512², bilinear), created/destroyed with the tile LRU
  (outside the image pool — `noPool` — one transient-upload copy per tile
  at generation time).
- Shaders `pak_0_engine/shaders/pass/heightmap_terrain/`:
  - `.vert`: implicit grid from push constants
    `{tileOriginXZ, tileSize, gridSize, heightScale(=1), wireframe,
debugMode}`, local lattice coords → world xz, `y = texture(heightTex)`,
    normal from ±1-texel neighbors, UV from world xz;
  - `.frag`: port of `azgaar_terrain.frag` shading (biome tint from
    `tintTex`, ground texture, IBL, shadow/GTAO), micro-band procedural
    normal perturbation, debug height ramp mode.
- `VulkanHeightmapTerrainPass` (registered in `Vulkan.c` next to the
  other terrain passes): one descriptor set per tile (heightTex, tintTex,
  material/ground texture), one push-constant draw per visible tile with
  the ring-based `gridSize`.
- Debug GUI: wireframe toggle, debug height-ramp toggle, backend switch
  (experimental mesh pass vs heightmap pass) for A/B.
- Exit: `run.sh play screenshot /tmp/hm.jpg` (Azgaar world) shows correct
  terrain with visible biome variation; A/B screenshot next to the
  experimental pass is visually equivalent (macro shape) with the
  heightmap version smoother (no 10 m-mesh faceting); wireframe shows the
  128/64/32 grid rings; fly across tile borders — no cracks, no pops.

### Phase 3 — Jolt physics (wrapper changes + per-tile bodies)

- Wrapper (`cpp-thirdparty/jolt/wrapper`):
  - `joltCreateHeightShapeNoFile(heights, pos, rot, offsetAABB, scale,
sampleCount, friction, restitution, userData)` — no cache file,
    returns `JoltHeightMap*`-compatible body handle;
  - replace the debug error string with real `result.GetError()` logging;
  - rebuild via the wrapper's `build.sh` (and `build-win.sh` when
    cross-building), relink the game.
- Engine: on tile READY → create the heightfield body (256²,
  offsetAABB = tile origin, scale = {8,1,8}); on eviction → destroy.
  Expose through the same physics plumbing that registers static bodies
  (check how `joltCreateHeightShape` bodies integrate with
  `joltGetActiveTransforms`/debug lines — heightfield bodies are static,
  so only `joltGetDebugLines` visibility matters).
- Character controller (`joltCharacterUpdate`) must walk, climb slopes up
  to `maxSlopeAngle`, and not sink/float across tile borders.
- Exit: full walk test in the Azgaar world — flat ground, rolling hills,
  a mountain (steep-slope limit engages, no tunneling), ocean coast
  (seabed collision), tile-border crossings; debug physics overlay shows
  the 25 heightfields; `run.sh play log 10000` clean.

### Phase 4 — Consumer migration + cutover

- Re-point `AzgaarGrass`, `AzgaarWater`, `AzgaarRoadCorridor`,
  `AzgaarRoadDecals`, `AzgaarCellTracker` to the new sample API.
- Deregister/remove `vulkanAzgaarTerrainPass` and delete
  `AzgaarTerrain.c`/`.h` mesh builder (keep `AzgaarWorld`, streaming
  math, water/grass systems). Default backend = heightmap.
- Exit: gameplay parity with the experimental world (grass density, water
  surface, road decals, cell tracking) via screenshots +
  `run.sh play log 10000`; GPU frame time (debug overlay) ≤ experimental
  (fewer triangles, no VBO uploads).

### Phase 5 — Polish (separate, later)

Candidates, not in the core scope:

- Per-tile disk cache of Jolt bodies / textures (wrapper file cache) to
  skip regeneration after eviction on large excursions.
- Steep-cliff side colliders (mesh band) where the heightfield surface
  leaves walkable cliffs transparent.
- Splatmap texture (multi-material blending) replacing biome tint.
- `.map`-driven auto placement of settlements/inns/dungeons (separate
  plan) + Blender heightmap export for hand-authored hero props
  (separate plan).
- Smaller physics window (3×3) if physics cost shows in profiling.

---

## Suggested file additions

```
c-engine/ecs/system/heightmap/HeightmapTerrain.h      # tile manager + API
c-engine/ecs/system/heightmap/HeightmapTerrain.c
c-engine/ecs/system/heightmap/HeightmapSource.h       # vtable
c-engine/renderer/vulkan/pass/heightmap_terrain/VulkanHeightmapTerrainPass.h
c-engine/renderer/vulkan/pass/heightmap_terrain/VulkanHeightmapTerrainPass.c
c-engine/data/pak_0_engine/shaders/pass/heightmap_terrain/heightmap_terrain.vert
c-engine/data/pak_0_engine/shaders/pass/heightmap_terrain/heightmap_terrain.frag
c-game/game/azgaar/AzgaarHeightmapSource.h/.c         # Azgaar adapter
cpp-thirdparty/jolt/wrapper/src/jolt_c_api.h/.cpp     # NoFile heightfield + error fixes
```

Removed after Phase 4:

```
c-engine/renderer/vulkan/pass/azgaar_terrain/          (pass + shaders)
c-game/game/azgaar/AzgaarTerrain.{h,c}                 (mesh tile builder)
```

## Build and validation checklist

- `./scripts/build.sh` — warning-strict build must stay clean, including
  the Jolt wrapper rebuild after Phase 3.
- `./scripts/run.sh play screenshot /tmp/shot.jpg` — in the Azgaar world
  (Phase 0 entry point); inspect terrain, biome tints, water, grass.
- `./scripts/run.sh play log 10000` — full window generated + evicted
  while flying across several tile borders; check `build/c-game/data/game.log`
  for tile timings, seam-check results, Jolt errors.
- A/B the experimental mesh pass vs the heightmap pass via the debug GUI
  backend switch (Phase 2–4).
- Crash during dev: `ENGINE_DEBUG=1` + gdb per AGENTS.md.
- Windows cross-check via `scripts/build-win-debug.sh` / release scripts
  at the end of Phase 4 (wrapper is cross-built).

## Milestone definition of done

- Walking (character controller) on heightmap-rendered terrain in the
  80 km Azgaar world: no cracks at tile borders, no clipping into hills,
  correct slope limits, seabed at the coast.
- Zero terrain data on disk; restart → identical world from the `.map`.
- GPU cost ≤ the experimental pass (≤ ~130 k visible tris after LOD
  rings, no per-tile VBO uploads).
- Grass, water, roads, debug tooling all functioning on
  the new backend; oghuzlands regular-mesh world unaffected.
