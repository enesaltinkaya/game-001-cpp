# Terrain Baked GI (Per-Tile Sky-Visibility Irradiance) — Implementation Plan

## Status (implemented 2026-08-28)

Phases 1–4 are in. Key deviations from the design above, all validated in
build + runtime (strict-warning build, `play` runs, A/B screenshots):

- **GI texture lives in the GLOBAL bindless array, not a second per-tile
  descriptor binding.** `vulkanCreateImage` (no `noPool`) auto-registers the
  GI image in `textures[MAX_IMAGES]` (the same pool every scene texture
  uses); `vulkanDestroyImage` frees the slot on eviction. The per-tile
  descriptor set stays `combinedImageSamplers = 1` (height only), so the
  placeholder-sampler hack and a second `vulkanUpdateDesc` are both gone.
  The pool index rides in the push constants: `pc.gi.x = giValid`,
  `pc.gi.y = bindless pool index` (exact as float for indices < 4096). The
  frag samples `textures[nonuniformEXT(uint(pc.gi.y))]` like every other
  scene texture. The global set's `UPDATE_AFTER_BIND` pool makes slot reuse
  safe for in-flight frames. (A second per-tile binding triggered a
  `set 1 binding 1 not declared` pipeline-layout validation error — the
  descriptor-layout helper creates one array binding, not N separate ones.)
- **Format is R8G8B8A8_UNORM (64 KB/tile), not R8G8B8.** The packed 3-byte
  format is not a linear-filterable sampled image on the RADV driver
  (`vkGetPhysicalDeviceImageFormatProperties2` → `FORMAT_NOT_SUPPORTED`).
  Alpha is always 255; the frag samples `.rgb`.
- **The bake runs in row-chunks (16 rows ≈ 2 s) and yields to pending grids
  jobs.** Measured bake is ~15 s per land tile on the worker thread (the
  1–3 s estimate was off: 16 384 texels × 256 dirs × up to 15 march steps of
  global `heightAt`). A monolithic bake on the single builder worker would
  have starved tile streaming (the spawn queue of 25 GI bakes ≈ 6 min of
  new-tile grid latency). The worker now (a) always selects the earliest
  non-GI (grids) job before any GI job, and (b) after each chunk re-checks
  the queue and requeues the GI job (with its partial buffer + row offset +
  the claimed height snapshot, all in the job) while grids work is pending.
  Rows are independent and the per-texel math is fixed-order, so a
  chunked/resumed bake stays bit-identical (determinism contract intact).
  `HeightmapJob.giOwned` tracks the `inFlight` slot across requeues;
  `destroyData` releases it for dropped queued jobs. Water tiles skip the
  march and bake in <1 s.
- **`pc.gi` is the 3rd vec4 of the scene pipe's PC block** (tile, flags, gi);
  the depth prepass pipe keeps its 2-vec4 block (separate pipeline, 256 B PC
  range). The C push-constant struct is 9 floats (48 B), pushed to all
  pipes (the prepass just ignores the last 16 B).

Tune-by-eye items left open (see Risks): `GI_SCALE` (1.0), direction count /
march range (bake cost), and — for a visible showcase — viewing a valley or
ridge rather than the flat settlement the camera is parked on (flat ground ⇒
uniform sky visibility ⇒ GI ≈ uniform IBL, which the A/B diff confirmed is
the correct behavior there).

## Goal

Replace the terrain's uniform IBL ambient (SH irradiance from the IBL resource)
with a **per-tile baked sky-visibility lightmap**: each terrain tile carries a
128² R8G8B8 texture of the sky irradiance *as the terrain sees it at each
point* — valleys read darker, ridges and open plateaus read brighter, cliff
faces lose most of their ambient. Static, zero runtime tracing, no DCC
pipeline: the "bake" is a pure CPU computation from the same
`HeightmapSource` the surface is built from.

## Why this works for the Azgaar world (no Blender, no meshes)

The scene's only occluder is the heightfield, and
`HeightmapSource::heightAt(userData, wx, wz)` is a **pure, global** function
(`c-game/game/azgaar/AzgaarHeightmapSource.h`: smoothed FMG pixel height +
world-anchored seeded fBm detail; see `docs/azgaar-terrain.md`). Consequences:

- Rays may leave the tile boundary freely — `heightAt` is valid for the whole
  80 km map. **No neighbour-tile dependencies, no padded sampling, no
  upload-ordering.**
- The bake is a pure function of `(source, tileX, tileZ)` → it satisfies the
  determinism contract of `plans/heightmap-terrain.md` exactly like the height
  grid: evicted tiles regenerate bit-identically, nothing is persisted.
- No new build-pipeline stage: it runs on the existing heightmap builder
  worker, in the background, never on the main thread.

## Approach: hemispheric sky-visibility bake (v1 = CPU)

Per tile, after its height grid is published READY, the builder worker runs a
second, **progressive** job (details below):

1. Grid: `HEIGHTMAP_GI_DIM = 128` (16 m per texel over the 2048 m tile),
   RGB, linear, `u8` (the v1 sky gradient stays within [0,1]; see Risks).
2. Per cell `(gx, gz)`:
   - Origin height = bilinear of the tile's own fine grid
     (`heightmapGridBilinear` on `tile->heights`) + 0.5 m lift — the exact
     rendered surface.
   - Underwater cells (h < ~0.5 m): write a flat deep-water ambient, skip
     the march (a ray from underwater self-hits the surface instantly).
   - 256 fixed stratified directions over the upper hemisphere (16×16
     lattice, golden-ratio jitter; a compile-time `static const` array —
     pure, identical for every tile/cell).
   - Per direction, march world-space: `t` from 4 m, `step *= 1.5`, up to
     3000 m (beyond the tile by ~1 km in the worst direction — fine, the
     source is global); blocked when
     `heightAt(px, pz) > py + 0.25f` (slab epsilon above the ray).
   - `color += unblocked ? skyColor(dir) : 0`, then `/ 256`.
3. `skyColor(dir)` is a **CPU port of `skyEvaluate` from
   `c-engine/data/pak_0_engine/shaders/includes/sky.shader`** (the shared
   procedural sky used by `skybox.frag` and the env-cube bake), **without the
   sun terms** (disc/glow/lobe): the disc integrates to ~0 and the lobe is
   sun-azimuth-dependent — excluding both keeps the bake static and lets the
   *direct* sun (already handled by the directional light + cascades) stay
   the only sun-dependent signal. Port note: the horizon wash's dependence on
   sun elevation must be pinned to a fixed reference value (noon), since the
   bake is static — the directional light is currently static too (no
   time-of-day system touches `sceneBuffer.directionalLight`), so nothing
   goes stale today. If a time-of-day system lands later, this plan's
   assumptions need revisiting (see Risks).
   Keep the port a small function with a comment pointing at `sky.shader` as
   the source of truth so the two don't drift.
4. Determinism: single-threaded, fixed loop order, `float` accumulation —
   bit-identical across evictions on the same build/machine, matching the
   existing contract for the height grids.

**Cost estimate:** 128² cells × 256 dirs × ~14 avg steps ≈ 70M `heightAt`
calls ≈ **1–3 s per tile** on the builder thread. Tiles are 2 km wide and
only 25 are ever resident, so this is background work nobody sees — but see
the "GPU compute (future)" section if measurements say otherwise.

## Architecture: progressive job, decoupled from READY

The GI bake must **not** gate tile readiness (the spawn hold
`heightmapTerrainHasBodyAt`, the Jolt body creation, and the first render all
key off READY; a 2 s bake on the central tile would delay spawn). Design:

```
builder worker (existing heightmapBuildThreadMain):
  job {ht, tileX, tileZ, kind}            // kind: GRIDS | GI (new field)
  GRIDS job → publish READY (as today) → immediately re-queue a GI job
  GI job    → bake (lock released) → under lock: copy vector into
              tile->gi, tile->giReady = true  (discard if tile evicted)

renderer pass (per frame, as today):
  height upload  (3/frame budget, unchanged)
  GI upload      (2/frame budget, new — for tiles whose GI arrived late)
```

Shader fallback: until the tile's GI texture is bound, the ambient block
keeps the current IBL/SH path — the terrain is lit from frame one and
quietly upgrades to baked GI a couple of seconds later.

## Files

### Modified

| File | Change |
| --- | --- |
| `c-engine/ecs/system/heightmap/HeightmapTerrain.h` | `#define HEIGHTMAP_GI_DIM 128`. `HeightmapTile`: `std::vector<u8> gi; bool giReady;` (freed/cleared with the tile in `heightmapTerrainFreeTile`). `HeightmapTileView`: `const u8* gi; u32 giDim; bool giReady;`. New public helper decl `heightmapTerrainBakeGi(HeightmapTerrain*, HeightmapTile*)` (runs on the builder thread; caller must have the tile claimed) if we keep the bake out-of-line; or keep it `static` in the .cpp — prefer the latter, no new public API. |
| `c-engine/ecs/system/heightmap/HeightmapTerrain.cpp` | (a) `HeightmapJob` gains `bool gi`. (b) In the publish path (after `tile->state = READY`), re-queue a GI job for the same tile (the builder already holds the lock; dedup guard: skip if a GI job for this tile is already queued). (c) Worker loop: for GI jobs, run the bake with the lock released, then under lock verify the tile still exists / is READY / is in this `ht` before publishing `tile->gi` + `giReady = true` (else discard the vector). (d) `heightmapTerrainSnapshotTiles` fills the new view fields. (e) `heightmapTerrainFreeTile` clears `gi`. (f) `ENGINE_GI_DISABLED=1` env (read once, `added()`-style static) skips the GI job entirely. The bake itself: a `static` function in this file using `ht->source.heightAt` + the static hemisphere sample table + the CPU `skyColor` port. |
| `c-engine/renderer/vulkan/pass/heightmap_terrain/VulkanHeightmapTerrainPass.cpp` | (a) `layoutHeightDesc` → `combinedImageSamplers = 2`. (b) `HeightmapGpuTile` gains `VulkanImage giTex; bool giValid;` (destroyed in `heightmapGpuTileDestroy` like `heightTex`). (c) `heightmapPassUploadTile`: when `view->giReady`, create the GI image (`R8G8B8_UNORM`, `HEIGHTMAP_GI_DIM²`, `noPool`, `SAMPLED|TRANSFER_DST`) and transient-copy `view->gi` (48 KB, same `vulkanTransientBegin/Copy/End` pattern); bind binding 1 to it. When GI is not ready yet, bind binding 1 to the **height texture** (guaranteed-valid placeholder sampler; the shader never samples it in that state) and leave `giValid = false`. (d) `HeightmapTerrainPushConstants` gains a 5th field `float giValid` (new `vec4 gi` in the GLSL PC blocks — see shader section). (e) Late-GI path in `preUpdate`: for cached tiles whose view now has `giReady` but the cache entry doesn't, re-upload GI + descriptor (own 2/frame budget; does NOT consume `HEIGHTMAP_PASS_UPLOADS_PER_FRAME`, and must not re-upload the height texture). `gpuTileHasView` gains a `giReady` comparison so a tile whose GI arrives after the height upload is re-acquired by this late path instead of matching stale. |
| `c-engine/data/pak_0_engine/shaders/pass/heightmap_terrain/heightmap_terrain.vert` | PC block: append `vec4 gi;` (x = giValid). No logic change. |
| `c-engine/data/pak_0_engine/shaders/pass/heightmap_terrain/heightmap_terrain.frag` | Same PC append. In the ambient block (details below). |
| `c-engine/data/pak_0_engine/shaders/pass/heightmap_terrain/heightmap_terrain_depth.vert/.frag` | **No change.** The depth prepass is a separate pipeline with its own `HeightmapPC` GLSL declaration (the Vulkan compatibility requirement only binds stages of the *same* pipeline), so only the scene + wireframe pair get the 5th vec4. **Verify at build time**: if `vulkanCreatePipe` or the push-constant range setup assumes one PC size across this pass' pipes, add the 5th vec4 to the prepass shaders too (harmless — it just goes unused). |

### New

Nothing (no new files in v1). No CMake edits: `file(GLOB_RECURSE)` +
`scripts/shaders.sh` pick everything up.

## Shader details — `heightmap_terrain.frag`

```glsl
layout(push_constant, std430) uniform HeightmapPC {
    vec4 tile;    // unchanged
    vec4 flags;   // unchanged
    vec4 gi;      // x = giValid (0/1), yzw unused
} pc;

layout(set = 1, binding = 0) uniform sampler2D heightTex;   // (VS today; frag may re-declare)
layout(set = 1, binding = 1) uniform sampler2D giTex;

#define HEIGHTMAP_GI_DIM 128.0
```

In `main()`, recompute the texel-centre UV (same addressing as the VS, with
the GI's own dimension — 16 m texels; bilinear filtering over 16 m is the
point):

```glsl
vec2 giUv = (local / HEIGHTMAP_GI_DIM) * (1.0 - 1.0/HEIGHTMAP_GI_DIM) + 0.5/HEIGHTMAP_GI_DIM;
// local = inWorldPos.xz - pc.tile.xz
```

In the ambient/IBL block: when `pc.gi.x > 0.5`, **replace the diffuse
irradiance** (the SH branch result `irradiance`) with the baked value:

```glsl
vec3 irradiance = /* SH path result, unchanged */;
if (pc.gi.x > 0.5) irradiance = texture(giTex, giUv).rgb * GI_SCALE;  // start 1.0, tune
```

Everything downstream (`ambientDiffuse = (1 - specFactor) * irradiance *
baseColor / PI * iblIntensity`, `shadowAmbientFade`, specular IBL via the
prefiltered cubemap, Forward+) stays untouched. Notes:

- Magnitude check: the baked irradiance is ∫ sky dΩ ≈ π · mean(sky gradient)
  ≈ 2–3, the same order as the SH L0 term — so `GI_SCALE` should need only
  small correction. Tune by eye against the parked scene, not by formula.
- Specular ambient (BRDF/specFactor path) keeps the IBL cubemap — terrain
  occlusion of *reflections* is out of scope for v1.
- The `shadowAmbientFade` (sun-shadow attenuation of ambient) still applies
  to baked GI — keep it; that's what prevents shadowed terrain from staying
  sky-lit.
- `debugHeightRamp` / wireframe paths are unaffected (they override the
  final color).

## Threading & lock rules (match existing invariants)

- GI jobs run on the **same** builder worker as grid generation — no second
  thread, no new locks. The worker is single-consumer, so a tile's GI job
  always runs after its grids job completed.
- Bake runs with `heightmapLock` **released** (it never touches tile state,
  only `ht->source` and the tile's already-published `heights` — the same
  "heavy work lock-free, publish under lock" pattern as
  `heightmapTerrainGenerateGrids`). Reading `tile->heights` is safe: a READY
  tile's grid memory is freed only by the main thread at eviction, and the
  GI job re-checks the tile's liveness under lock before *publishing* — the
  window where it might read a freed grid is the same one the existing seam
  check avoids via its "published" flag pattern; mirror that: re-validate
  `tile->state == READY` under lock at publish time and treat a stale tile as
  a discard.
  **Caveat:** unlike the grids job, a GI job may be *superseded* by eviction
  mid-bake (2 s of work discarded) — that is fine (it's background polish),
  and the dedup rule (one GI job per tile in the queue) prevents piling up.
- `destroyData` drops GI jobs from `buildQueue` (extend the existing
  `buildQueue[i].ht == ht` sweep) and the inFlight drain covers GI jobs as
  today (`inFlight` is incremented for both kinds).

## Validation

1. `./scripts/build.sh` (shader recompile included).
2. `./scripts/run.sh play log 5000` — log lines: grids "ready in X ms", new
   "gi baked in Y ms", late GI upload. Confirm: no main-thread hitches
   (`ENGINE_HITCH_DEBUG=1`), GI jobs land seconds after READY, evicted
   re-entry re-bakes with **identical** output (determinism: park player,
   travel > 10 km, come back, A/B screenshots must be pixel-stable after
   re-bake settles).
3. A/B with the parked player (per AGENTS.md — player/camera are parked on
   the object under test; do not move them):
   - `ENGINE_HIDE_GUI=1 ./scripts/run.sh play screenshot /tmp/gi.jpg`
   - `ENGINE_GI_DISABLED=1 ENGINE_HIDE_GUI=1 ./scripts/run.sh play screenshot /tmp/gi_off.jpg`
   - Expected: valleys, gullies and overhang undersides darker than the IBL
     baseline; ridge crests and open slopes brighter; sunlit faces
     unchanged (direct light path untouched); sky/water untouched; no
     16 m-blocky banding that reads as hard seams between tiles (the GI
     march is global, so adjacent tiles' borders *should* be continuous —
     verify at a tile border visually; a visible 1-texel seam there means a
     march-boundary bug).
4. No pop-in flash: the IBL→GI handoff is a per-tile color swap; both are
   static, so the delta is small. If it's visible, add a 1-frame lerp in the
   shader (open question, v1.1).
5. Perf sanity: bake time per tile in the log (target < ~5 s at 128²), no
   frame-time change (runtime cost is one extra texture sample + 48 KB/frame
   of streaming uploads amortized over 2/frame).

## Risks / open questions

- **8-bit banding** on smooth sky gradients at 16 m texels — acceptable for
  v1 (bilinear-filtered, and the ambient is a soft term); upgrade path is
  R16G16B16 (doubles 48 KB → 96 KB/tile, trivial).
- **GI handoff pop** (IBL → baked) when a tile's GI uploads late — likely
  subtle (both are static ambient terms); if it reads, lerp over a frame or
  two in the shader.
- **Static-sky assumption**: the bake excludes sun terms and pins the
  horizon wash. Safe today (no time-of-day), but a future sun-cycle system
  must either (a) scale the GI by a runtime sun-intensity ratio, or (b)
  re-bake per sun state — flag explicitly when that system lands.
- **Bake CPU time on weak hardware** — 128²/256 dirs is a worst-case
  choice; both are trivially tunable constants (64²/128 dirs = ~4× cheaper,
  32 m texels still invisible at the distances terrain is read). A coarse
  height pyramid (FMG downsampled, fast-reject march) is the intermediate
  lever if the worker starves grid generation.
- **GI job starvation**: grid jobs and GI jobs share one queue. On a 25-tile
  window fill, 25 GI jobs (~50 s of worker time) queue behind the next
  grid generation. Mitigation if it matters: interleave (always give a
  grid job priority over a GI job when both are pending) — cheap to add to
  the worker pop logic.
- **Prepass PC layout**: verify the depth prepass's GLSL `HeightmapPC` is a
  *separate* declaration (it is a separate pipeline; only scene + wireframe
  share the extended block). If the build shows a PC-size mismatch, add the
  5th vec4 to the prepass shaders too (harmless).
- **Descriptor binding 1 unused by prepass**: fine — Vulkan allows the
  pipeline layout to carry bindings a shader doesn't declare; the descriptor
  is still updated to a valid sampler (the height texture placeholder).

## GPU compute (future, only if CPU bake measures slow)

A compute-shader bake would be ~50–100× faster (~10–30 ms/tile) but changes
the data model: the GPU has no global `heightAt` (only R32F textures of
resident tiles, and rays reach ±2 tiles away). The viable design is **port
`heightAt` to GLSL**:

1. Upload the raw FMG map **once** at world load (static texture; bilinear
   filtering reproduces the smoothed pixel height).
2. Port the world-anchored fBm detail band (~30 lines of pure noise math,
   FNV seed) to a GLSL function.
3. `gi_bake.comp`: one thread per GI texel, same 256-dir stratified march
   against the GLSL `heightAt`, writing the per-tile R8G8B8 image directly
   (no CPU round-trip), dispatched per tile with a `readyStamp`-keyed image.

Determinism still holds (bit-reproducible per GPU hardware; the GI does not
need to match the CPU surface, only the determinism contract). Keep the CPU
path behind it as a fallback (`ENGINE_GI_CPU=1`); the CPU implementation is
the reference implementation the GLSL port is verified against (dump the
same tile from both, compare).
