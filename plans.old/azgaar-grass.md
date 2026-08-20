# Azgaar Grass Blades Plan

_Procedural scattered grass blades, tinted per FMG biome._

> **Status: superseded by `plans/azgaar-world-population.md`** (world
> population plan, workstream B). Its scatter architecture — clumped fBm
> gating, per-tile budgets, wind sway, biome-tinted instances — is folded
> into the unified props system there. Note: this plan targeted the old
> `AzgaarTerrain` 5×5 batch-build architecture; terrain is now the
> engine's streaming `HeightmapTerrain`, so the build-thread hook in this
> plan no longer applies as written.

---

## Goal

Scatter small procedural grass blades across the streaming 5×5 tile window so
land biomes read as distinct vegetation instead of a uniform grass texture.

Each blade's colour comes from the 13-biome table stored in the Azgaar `.map`
file (IDs 0–12: Marine … Wetland). Blades:

- Sit **exactly** on the rendered/collided terrain surface (reused from the
  terrain build's blurred height grid, decision 4).
- Are **procedurally generated** (fixed small blade geometry, decision 3).
- **Animate with wind** (vertex-shader sway driven by `sceneBuffer.time` +
  per-instance phase, decision 2 = include in v1).
- Are **culled per tile** (v1: CPU frustum test, decision 5; GPU culling in P2).

---

## Current state (so the plan is self-contained)

**Biome data — `c-game/game/azgaar/AzgaarWorld.{h,c}`**

- The `.map` file's 4th line is the biome table (13 entries, `i:0..12`, each
  with `name` + `color`). `AzgaarWorld.c` parses it into `world->biomes`
  (`world->biomeCount`) and classifies each Voronoi cell's biome via FMG's
  `biomesMatrix` from moisture/temperature/height.
- `azgaarWorldBiomeColor(world, biomeId, outRgb)` returns the biome's RGB.
- `world->cells[cellId].biome` is the per-cell biome ID.

**Terrain build — `c-game/game/azgaar/AzgaarTerrain.c`**

- Streaming 5×5 window: `AZGAAR_TILE_RADIUS = 2`, tiles are `2000 m`,
  `AZGAAR_TILE_GRID_SUBDIVISIONS = 192` (≈10.4 m grid step).
- `createTileChunk()` builds a watertight sampled height grid, applies a
  3×3 box blur (×2) into local `rawHeights` (193×193), builds the mesh, then
  **frees the grid** — the grid is currently local.
- `buildThreadMain()` runs on a `threadPoolAddWork` background thread;
  `azgaarTerrainBuildFinalize()` (main thread) uploads via
  `vulkanAzgaarTerrainSetMesh(...)`.

**Streaming — `c-game/game/azgaar/AzgaarStreaming.c`**

- `update()`: when the player crosses a tile boundary, `azgaarTerrainBuildStart()`
  queues a full 5×5 rebuild in the background; the next ready-frame calls
  `azgaarTerrainBuildFinalize()`.
- Same pattern on initial load in `LoadingAzgaar.c` (build start at the
  terrain stage, finalize when ready).

**Render passes — `c-engine/renderer/vulkan/Vulkan.c`**

- Pass order (L106–131): culling → depth → occlusion → hiz → shadow → gtao →
  contact_shadow → light_culling → terrain → **azgaar_terrain** → debug* →
  scene → skybox → **azgaar_water** → oit → ssr → …
- `vulkanCullingPass` already implements **GPU culling** (`scene_culling.comp`):
  frustum + HiZ against scene instance/transform buffers, writes indirect draw
  counts + culled buffers. Useful reference for P2.

**Reference plan:** `plans/azgaar-water.md` (separate pass + own upload path,
`WaterData` uniform, camera-following grid, phased roadmap).

---

## Design decisions & rationale

### 1. Scatter is computed in the background tile build

When a new 5×5 grid is built in the background (`AzgaarStreaming` / `LoadingAzgaar`
rebuild), the grass scatter for that window is computed **in the same build
thread**, right after the tile meshes are built. Only the small GPU upload
(`vulkanAzgaarGrassSetData`) happens on the main thread at finalize — same
lifecycle as `vulkanAzgaarTerrainSetMesh`, so no main-thread hitches.

### 2. Wind animation included in v1

Blades sway in the vertex shader: `sway = sin(time * windSpeed + instance.phase)
* (vertex.height * windStrength)`, applied in XZ. Per-instance `phase` (random
0..2π) desynchronises blades so the whole window doesn't move in unison.
Wind direction comes from `windAngle` (radians) in a small `GrassData` uniform.

### 3. Blade geometry: small procedural curved blade

A fixed, **procedurally generated** blade — not a texture:

```
vertices (5): base-left, base-right, mid-left, mid-right, tip
triangles (3): (bl, ml, br), (br, ml, mr), (ml, tip, mr)
```

- `y` goes 0 (base) → 1 (tip) as the "height" attribute used by the wind
  sway weight.
- Slight forward curve: mid/tip offset a few cm in +Y and -X (lean).
- Per-instance `scale` (per-biome base × jitter) sets blade height (≈0.3–1 m).
- 5 verts / 3 tris per blade → 1M instances = 3M verts max; trivial for the GPU.

### 4. Reuse the terrain build's blurred height grid

`createTileChunk()` currently frees its blurred grid at the end. Refactor:
return/expose the 193×193 blurred grid (out-parameter) so `AzgaarGrass` can
bilinear-sample the **exact** surface the terrain mesh was built from — blades
sit flush with the rendered *and* collided surface (no floating/embedded blades).

### 5. Culling: per-tile CPU frustum test in v1, GPU culling in P2

- **v1 (recommended):** each tile's `chunk->boundsMin/Max` already exists; the
  grass pass frustum-tests each tile (or per-instance, cheap) and skips drawing
  tiles outside the view frustum. Zero new engine machinery.
- **P2:** extend `vulkanCullingPass` (which already does GPU culling of scene
  instances with frustum + HiZ) with a second dispatch over the grass instance
  buffer → indirect draw counts + culled instance buffer, so hidden blades cost
  nothing. Deferred; v1 must stay simple.

### 6. Per-biome density (agreed targets)

| Biome ID | Biome (from .map) | Density (blades/m²) |
|----------|-------------------|----------------------|
| 4 | Grassland | 0.30 (was 0.05) |
| 3 | Savanna | 0.20 (was 0.03) |
| 9 | Taiga | 0.06 (was 0.005) |
| 10 | Tundra | 0.03 (was 0.002) |
| 0,1,2,5,6,7,8,11,12 | Marine/Deserts/Forests/Glacier/Wetland | 0 |

**Instance budget:** a fully-grassland 5×5 window is 10 km² = 10⁸ m² →
0.15/m² = **15M instances**, far above any sane buffer. `AZGAAR_GRASS_HARD_CAP`
raised 1M → **2M** (buffer allocated dynamically, so safe). Allocation order =
tiles by distance from the player (center tile first), stop at the cap; log a
warning when the cap is hit. This window lands at the 2M cap → far tiles are
grass-less. 

### 7. Clumped scatter: noise-gated grass tufts

Grass grows in **tufts**: a tight cluster of 8–32 blades packed within a
0.8 m disc of a single anchor point.  Anchor candidates are scattered
uniformly through each grid cell (≈10.4 m) and kept only if a world-space
fBm value-noise sampled at that exact position exceeds a threshold — so
tufts group into clumps a few metres across with clear bare ground between
them, instead of a uniform carpet of single blades.

- Noise: two-octave fBm (amplitudes 0.6/0.3), base frequency
  `1 / AZGAAR_GRASS_CLUMP_PERIOD_M` (10 m clump period), fixed seed
  `AZGAAR_GRASS_NOISE_SEED (0x9E3779B9u)` — sampled in world space at each
  anchor position, so the clump layout (which spots are clumps vs bare)
  is stable across streaming-window rebuilds.  The window RNG only decides
  which individual tufts/blades exist inside a clump.
- Gate: `AZGAAR_GRASS_ACCEPT_THRESHOLD (0.55)` on the normalised fBm [0,1];
  ≈40% of positions pass, so clumps cover roughly two-fifths of the ground
  and the rest stays bare.
- Tuft density table (anchors/m², pre-gate): Grassland 0.060, Savanna 0.040,
  Taiga 0.015, Tundra 0.008 — one tuft ≈ 8–32 blades in a ~1 m disc, so
  these read as dense grass patches (≈1.2 blades/m² in clumps, 0 in gaps).
- Near-player guarantee: within `AZGAAR_GRASS_FOCUS_FULL_RADIUS_M` (250 m)
  of the build-time player position every anchor is kept (full meadow),
  fading to 0 by `AZGAAR_GRASS_FOCUS_FADE_RADIUS_M` (600 m) via a
  probability roll, so the player never spawns in a bare gap.
- Instance budget / cap: `AZGAAR_GRASS_HARD_CAP` = **5M**, distributed as
  **per-tile budgets** weighted by proximity to the build-time focus
  (`AZGAAR_GRASS_BUDGET_FALLOFF_M`, weight = 1/(1+(d/1200m)²) over land
  tiles, shares sum to the cap).  A single global cap spent nearest-first
  let one dense grassland tile consume the whole budget and leave its
  neighbours with zero instances — a hard grassless wall at the tile border
  (player stood on one and reported the cutoff).  Per-tile budgets guarantee
  every land tile keeps a share (nearest tiles the most), so the shader's
  300 m falloff ring never runs into an empty neighbour; tiles that still
  exhaust their budget (dense grassland centre tile) are logged.
  Beyond ~2 km the blades are sub-pixel and the terrain albedo carries the
  look.
- Blade length: per-instance scale `0.30 + rng * 0.35` m with the existing
  ×0.7–×1.3 jitter; `azgaar_grass.vert` computes
  `width = max(height * 0.18, 0.015)`, so taller blades keep proportional
  width.
- Debug: `AZGAAR_GRASS_DEBUG=1` logs per-tile counts, sample instances and a
  100 m ring histogram of instance density around the player — used to
  verify the guarantee zone (≈1.2/m² inside 250 m) and the gated mid-field
  (≈0.2–0.4/m² beyond 600 m).

**Design history:** the original design multiplied per-cell density by a
smoothstep-shaped noise mask sampled once per cell.  That looked uniform:
the mask barely varied between the ~10 m cell samples, so it read as a
slow density gradient rather than distinct patches.  Per-blade gating fixed
the boundary resolution but still looked like a field of slightly
varying-density single blades.  The tuft design (cluster of blades per
noise-gated anchor) is what finally produces visually distinct clumps with
bare ground between them.  The biome density/colour fields were first
smoothed with the height grid's 3x3x2 kernel, which faded a biome border
over only ~30 m — a visible hard cutoff line at every km-scale Azgaar cell
border (reported by the player standing on one).  The fields are now built
on a ~40 m coarse sub-grid and blurred `AZGAAR_GRASS_FIELD_BLUR_PASSES`
times, so biome borders taper over a few hundred metres (`AzgaarGrass.c`:
`grassBlurField` / `grassSampleCoarse`).

---

## New / modified files

### New files

| File | Purpose |
|------|---------|
| `c-engine/renderer/vulkan/pass/azgaar_grass/VulkanAzgaarGrassPass.{h,c}` | System `vulkanAzgaarGrassPass`: owns blade mesh + per-window instance buffer, `vulkanAzgaarGrassSetData(...)`, per-tile frustum test, pipeline (opaque, alpha-test, depth-write on). |
| `c-engine/data/pak_0_engine/shaders/pass/azgaar_grass/azgaar_grass.vert` | Instance transform (pos + rotY(yaw) × (bladeVert × scale)) + wind sway (`sceneBuffer.time` + per-instance phase). |
| `c-engine/data/pak_0_engine/shaders/pass/azgaar_grass/azgaar_grass.frag` | Per-instance biome color × analytic Lambert + hemispheric (mirrors `azgaar_terrain.frag`), alpha-test discard. |
| `c-game/game/azgaar/AzgaarGrass.{h,c}` | CPU side: per-biome density table, scatter generation on the build thread (reusing the blurred grid), `azgaarGrassBuildStart/Finalize` mirroring `AzgaarTerrain`; pushes instances via `vulkanAzgaarGrassSetData` at finalize; `GrassData` uniform (wind params, enabled flag). |
| `plans/azgaar-grass.md` | This file. |

### Modified files

| File | Change |
|------|--------|
| `c-engine/renderer/vulkan/Vulkan.c` | Register `vulkanAzgaarGrassPass` right after `vulkanAzgaarTerrainPass` (L115) — grass is opaque (alpha-test) so it renders after terrain, before `vulkanAzgaarWaterPass` (transparent). |
| `c-engine/CMakeLists.txt` | Add `VulkanAzgaarGrassPass.c`. |
| `c-game/CMakeLists.txt` | Add `AzgaarGrass.c`. |
| `c-game/game/azgaar/AzgaarTerrain.c` | Small refactor: `createTileChunk` exposes its blurred 193×193 height grid (out-parameter) instead of only freeing it; `buildThreadMain` passes the grids to `azgaarGrassBuildStart`. |
| `c-game/game/azgaar/AzgaarStreaming.c` | Call `azgaarGrassBuildFinalize()` alongside `azgaarTerrainBuildFinalize()` in `update()`. |
| `c-game/game/loadingAzgaar/LoadingAzgaar.c` | Same: finalize grass next to the terrain finalize (terrain stage). |
| Shaders | `scripts/build.sh` compiles the two new GLSL (debug + release SPV). |

---

## Data layouts

### Instance buffer (CPU/GPU must match, 36 B)

```c
typedef struct {
    float pos[3];   // world position on the blurred surface
    float yaw;      // random 0..2π
    float scale;    // per-biome base × jitter (0.3–1.0 m)
    float color[3]; // biome RGB from azgaarWorldBiomeColor (linear)
    float phase;    // wind phase 0..2π
} GrassInstance;
```

### `GrassData` uniform (in `globalset.shader` + CPU mirror)

```glsl
struct GrassData {
    vec4 wind;        // xy = wind direction (unit), z = speed, w = strength
    vec4 jitter;      // x = hue jitter amount, y = scale jitter amount
    float windPhaseSpeed;
    float enabled;     // 0/1 kill switch (settingsGetBool("grassDisabled") later)
};
```

---

## Shader design

### `azgaar_grass.vert`

```glsl
// in: blade vertex (pos: xz local, y = height 0..1), instance (GrassInstance)
// 1. worldBase = instance.pos
// 2. rotated = rotY(instance.yaw) * (vec3(blade.x, 0, blade.z) * instance.scale)
//    y offset = blade.y * instance.scale
// 3. sway = sin(sceneBuffer.time * grass.wind.z + instance.phase)
//    lateral = vec2(cos(grass.wind.x), sin(grass.wind.x)) * sway * grass.wind.w * blade.y
// 4. worldPos = worldBase + rotated + vec3(lateral.x, 0, lateral.y)
// 5. gl_Position = viewProjection * worldPos
// out: worldPos (lighting), normal = rotY(yaw) * up
```

### `azgaar_grass.frag`

```glsl
// 1. Normal: world up rotated by instance yaw (constant per blade)
// 2. Lambert: max(dot(N, sunDir),0) * sunColor + hemispheric
// 3. color = instance.color * (diffuse + ambient)  (+ tiny hue jitter)
// 4. Opaque: alpha-test not needed (solid blade), depth-write ON
// outColor = vec4(color, 1)
```

---

## Phased roadmap

### Phase 0 — MVP (blades on screen)

1. `GrassData` uniform + `vulkanResourceSetGrassParams(...)` (mirror the
   `WaterData` pattern from the water plan).
2. `AzgaarGrass.c`: per-biome density table; scatter generation **inside the
   background build thread** (reusing the exposed blurred grid); 1M instance
   cap with center-tile-first allocation; `azgaarGrassBuildStart/IsReady/Finalize`
   mirroring the terrain API; `vulkanAzgaarGrassSetData(...)` at finalize.
3. Engine pass + pipelines (opaque, depth-write on) registered after
   `vulkanAzgaarTerrainPass`.
4. Blade mesh (5 verts / 3 tris) + instance attributes + wind sway in the
   vertex shader.
5. `./scripts/build.sh` (compiles sources **and** shaders).

**Done when:** grassland/savanna/taiga/tundra tiles show swaying, biome-tinted
blades sitting flush on the surface; zero blades on ocean/desert/glacier;
≤1M instances per window; per-tile frustum culling active.

### Phase 1 — Quality & tunables

- Per-blade hue + scale jitter (already in the instance layout).
- Slope rejection: skip scatter points on steep gradients (reuse chunk normals
  or finite-difference of the blurred grid).
- Distance falloff: reduce density with distance from camera (view budget).
- Blade shape variation (width/curve/height ranges per biome).
- Expose toggles/tunables in the debug GUI; add `settingsGetBool("grassDisabled")`
  kill switch.
- Wetland (ID 12) small density (e.g. 0.01) once wetland plants make sense.

### Phase 2 — GPU culling

- Extend `vulkanCullingPass`: add a second dispatch over the grass instance
  buffer (frustum + HiZ), write `grassDrawCount` + culled instance buffer →
  indirect draw. Hidden blades cost nothing.
- Optional: move blades into `VulkanScene` so the **existing** culling pass
  handles them directly (grass as a scene mesh with the grass material).

---

## Validation

Per `AGENTS.md`:

```bash
./scripts/build.sh                                   # compiles C + shaders
./scripts/run.sh play screenshot /tmp/grass.png      # visual check
./scripts/run.sh play log 5000 && cat build/c-game/data/game.log
```

Acceptance checklist:

- **P0:** blades visible on grassland/savanna/taiga/tundra, tinted with the
  13-biome colors from the `.map`; blades sit flush (no floating/embedding);
  ocean/desert/glacier have no blades; wind sway visible (blades not moving
  in unison); draw cost < ~2 ms; instance count ≤ 1M (log the count).
- **P1:** jitter + slope rejection + distance falloff look natural; GUI
  tunables respond.
- **P2:** GPU-culled instances; hidden blades produce zero GPU work.

Crashes: run with `ENGINE_DEBUG=1` + `gdb` (per `AGENTS.md`).

---

## Risks / open questions

- **Instance cap behavior:** if a window is mostly grassland the 1M cap is hit
  (5M wanted). Mitigation: center-tile-first allocation + a `warn()` log.
  Open: is 1M the right cap, or should it scale with the density of the
  actual biomes in the window?
- **Grid reuse refactor:** `createTileChunk` must expose `rawHeights` (the
  blurred grid) instead of freeing it — small, isolated change; verify no
  double-free and that the grid lifetime is bounded by the build thread.
- **MSAA:** grass writes into the MSAA `sceneColor` target (opaque pass, same
  as `azgaar_terrain`); confirm `vulkanCreatePipe`/`vulkanBeginRender` load
  (not clear) `sceneColor` like the terrain pass does.
- **Time unit:** confirm `sceneBuffer.time` scale (ms vs s) so `wind.speed`
  tuning is sane (same check as the water plan).
- **Blade vs water order:** grass renders **before** `azgaar_water` (opaque →
  transparent), so shore blades are correctly occluded by the translucent water
  surface; verify no z-fighting at the shoreline (offset blade base +1 cm).
