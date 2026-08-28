# Global Illumination (No Raytracing) — Implementation Plan

> **Status: PROPOSED.** No code written yet. This is the design + phased
> roadmap for adding diffuse bounce light ("GI") to the engine without
> hardware ray tracing. The recommendation is a **hybrid**: per-tile
> sky-visibility + SH tint (offline, deterministic) + screen-space GI
> (near) + streamed SH probe grid (far), keeping the existing sky IBL for
> specular and CACAO AO on top.

## Why / what is missing today

The lighting model in `scene.frag` (and `oit_accumulate.frag`) already has:

- **Static sky IBL** (`pass/ibl/`, `VulkanIbl.h`): env cube + irradiance cube
  + prefiltered mips + BRDF LUT, with L0/L1 SH fallback
  (`evaluateSHIrradiance`, `scene.frag:58`). This is *ambient from the sky
  only* — it is never occluded by geometry and never carries color from one
  object onto another.
- **CACAO AO** (`pass/ao/`, `plans/cacao-ao.md`) + `ao_temporal.comp`:
  occlusion that only *subtracts* light.
- **SSR** (`pass/ssr/`): specular, screen-limited.
- **Volumetric light shafts** (`pass/volumetric/`): sun in-scattering.

The gap is **diffuse interreflection**: light that leaves one surface and
lights another (green grass tinting tree trunks, a warm wall bouncing into a
floor, sky light leaking into a cave from its opening). Nothing in the current
pipeline adds that. This plan adds it.

## Guiding constraints (from the codebase)

- **No RT hardware, no software BVH.** Everything is raymarch / baked /
  rasterized. No new acceleration-structure dependency.
- **Streamed open world**, no persistence. Terrain is an *implicit heightmap
  lattice* (`pass/heightmap_terrain/`, `plans/heightmap-terrain.md`): 2048 m
  tiles, 512² R32F height textures, **never written to disk** — evicted tiles
  regenerate bit-identically from a `HeightmapSource` (`heightAt(wx,wz)`).
  Any per-tile baked GI must be a **pure function of (source, world xz)** to
  respect that contract, or be streamed as data like the navmesh/vegetation
  bakes.
- **Buffers already present** that a GI pass can reuse: depth prepass covering
  terrain + props (`heightmap_terrain_depth.*`, `azgaar_props_depth.*`),
  oct-encoded normal buffer, material-index buffer (albedo/roughness lookup),
  HiZ mip chain (`vulkanHiZGetMipSampledIndex(mip)`, `pass/hiz/`), velocity
  `R16G16_SFLOAT` from the FSR/TAA path, and a working temporal-reprojection
  pattern (AO + TAA).
- **Vulkan 1.3**, bindless `descriptorBindingSampledImageUpdateAfterBind`
  enabled (`Vulkan.cpp`). Compute passes are the house style.
- **Single queue** (`Vulkan.h` exposes only `graphicsQueue`). GI compute
  serializes with graphics — a real argument for ½/¼-res GI and a rolling
  probe-update budget. (Optional: add an async compute queue; see Phase 0.)
- **AMD DCC gotcha** (`docs/oit-amd-dcc.md`): before writing any 8-bit
  multiplicative-blend or renderpass-fast-clear accumulate target, read that
  doc — that combination intermittently mis-decompresses on AMD.

## Pipeline placement

GI runs after geometry (needs depth/normals/albedo) and before the composite
that assembles the final image:

```
... scene → oit → ssr → ao → [ SSGI (new) ] → volumetric → decal
    → composite  (adds: tile skyVis×SH tint, SSGI term, probe irradiance)
    → taa → dof → fsr → bloom → final → lpm → lens → rmlui
```

- **Tile sky-visibility + SH tint** (Phase 1) and **probe grid** (Phase 3)
  are *world-space* and get blended into the ambient/IBL term in
  `scene.frag`/`oit_accumulate.frag` (they modulate the same `irradiance`
  value already sampled there — `scene.frag:214`).
- **SSGI** (Phase 2) is a compute pass producing an `rgbf` bounce buffer at
  ½-res, accumulated with its own temporal history (same pattern as AO), and
  consumed as an *additive* ambient term in the composite (next to the existing
  AO sample at `composite.comp:117`).

The clean rule: **near-field bounce from SSGI, far/off-screen from probes,
specular from SSR + sky IBL, occlusion from CACAO AO.** Blends are weighted by
screen-space confidence (SSGI rays that miss fall back to probes) so there are
no seams.

## Data & buffer budget (steady state, per frame)

| Buffer | Type | Where | Lifetime |
|---|---|---|---|
| `tileSkyVis` (per world tile) | R8 512² | streamed like height tiles | persistent per tile, deterministic |
| `tileSH` (per world tile) | L0–L1 SH RGB (4×`vec4`) | scalar per tile (or low-res 32²) | persistent, baked |
| `ssgiA` / `ssgiB` | R16G16B16A16_SFLOAT, ½-res | new frame resource | ping-pong (TAA pattern) |
| `ssgiOut` (upsampled) | R16G16B16_SFLOAT, full res | new frame resource | single |
| probe block (Phase 3) | 3D texture of L2 SH or irradiance, N³ | streamed per block | persistent per block, baked |

No new full-res HDR buffers beyond the SSGI pair; ½-res keeps bandwidth and
the single-queue cost down.

---

## Phase 0 — prerequisites (small, optional but recommended)

1. **GI slot in the scene buffer.** Add an `IblData`-adjacent struct (or extend
   `VulkanIblData`) for: `tileSHEnabled`, per-tile lookup params (tile size,
   origin), `ssgiEnabled`, `probeGridOrigin/spacing/count`, `probeEnabled`,
   and blend weights. Push via a `vulkanResourceSetGi(...)` mirroring
   `vulkanResourceSetIbl`.
2. **Composite + fragment shader ambient hook.** In `scene.frag:214-226`
   (and `oit_accumulate.frag`'s mirror), factor the ambient so it can be
   `mix(skySH, tileSH, skyVis)` and later `+ ssgiBounce` / `+ probeIrradiance`.
   Keep the sky IBL path exactly as-is when GI is disabled (bit-identical
   fallback).
3. **½-res frame resource plumbing.** Add `ssgi`/`ssgiOut` frame resources
   following the TAA `taaA`/`taaB` + `swapchainCreated` recreation pattern.
4. **(Optional) Async compute queue.** If profiling shows the SSGI/probe
   compute hurts frame time, enable a second compute `queueFamily`. Defer
   unless measured.
5. **Build wiring.** New compute shaders land under
   `c-engine/data/pak_0_engine/shaders/pass/gi/` and are compiled by the
   existing `scripts/build.sh` shader step (no CMake change for shaders).
   New offline tools (Phases 1 & 3) go under `tools/` + a
   `scripts/build-<name>.sh`, invoked from `build.sh` with a `SKIP_<NAME>=1`
   env escape hatch, exactly like `build-navmesh.sh` / `build-vegetation-builder.sh`.

## Phase 1 — per-tile sky-visibility + SH tint  (cheapest big win, do first)

**Goal:** make terrain look *grounded* and let each tile contribute a cheap
baked ambient tint, with zero runtime ray work and full determinism.

**Bake (offline, deterministic).** A `tools/` builder reads the Azgaar `.map`
(via the game's `HeightmapSource` adapter, `c-game/game/azgaar/AzgaarHeightmapSource.h`)
and, for every world tile (2048 m), bakes:

- **`tileSkyVis`** — per-texel **horizon / sky-visibility**: for each of the
  512² columns, march a small set of up directions against the tile's own
  heightfield (plus a 1–2 tile halo from neighbors for edge cases) and store
  the fraction of the sky hemisphere that is open (0 = in a pit, 1 = open).
  This is the classic terrain "sky light" term. R8, 512².
- **`tileSH`** — L0–L1 SH (4×`vec4`) of the *average light leaving this
  tile*: sky irradiance attenuated by `tileSkyVis`, plus the dominant neighbor
  albedo (grass/snow/cliff from the climate maps) folded in. Stored as a tiny
  per-tile scalar block (no texture needed, or a 32² map if per-region tint is
  wanted).

Because `heightAt` is a pure function of (source, wx, wz), the bake is fully
reproducible and the runtime can recompute an evicted tile on demand — it never
needs to touch disk, matching the determinism contract.

**Runtime.** Stream `tileSkyVis` alongside the height tiles in
`c-engine/ecs/system/heightmap/` (a parallel R8 image per resident tile, or a
2nd channel in an extended tile texture). In `scene.frag`, replace the ambient
irradiance with:

```
skyVis   = texture(tileSkyVisTex, worldXZ->uv).r;
irradiance = mix(skySH /*current IBL*/, tileSH, clamp(skyVisOcclusion,0,1));
```

so pits/caves darken toward the baked tint instead of full sky light, and open
terrain keeps the live sky IBL (so weather/time-of-day still animate it).

**Validation:** `ENGINE_HIDE_GUI=1 ./scripts/run.sh play screenshot
/tmp/gi1.jpg` on a known vantage (a valley + a cave mouth). Compare against
today's screenshot. Determinism check: run twice from a parked player, diff the
two images byte-for-byte (must be identical).

**Files:** `tools/skyvis-builder/`, `scripts/build-skyvis.sh`, a small
`HeightmapTerrain` extension for the parallel R8 image, `scene.frag` /
`oit_accumulate.frag` ambient hook, `build.sh` hook.

## Phase 2 — Screen-Space GI (near field)

**Goal:** real per-pixel diffuse bounce from *visible* geometry (crevice and
contact color bleed), no offline content, works with the streaming world.

**Pass:** new `engine::VulkanGIPass` (`pass/gi/`), two compute dispatches —
same shape as `VulkanAOPass`:

1. **Ray pass** (`ssgi.comp`) — ½-res. Per pixel: reconstruct normal (sample the
   oct normal buffer; do *not* re-derive from depth, the normal buffer is
   oct-encoded and the AO comment notes it is incompatible with CACAO's affine
   unpack, but plain oct decode is fine here) and albedo (material-index
   buffer → material → baseColor). Fire ~16–24 jittered rays over a disk in
   view space; march with **HiZ early-out** (`vulkanHiZGetMipSampledIndex`)
   just like the removed XeGTAO (`plans/ambient-occlusion.md`). On a hit, gather
   the hit's albedo and add `hitAlbedo * sourceAlbedo / PI * rangeFade` into a
   per-pixel bounce sum (single-bounce; optionally a 2nd ray for crevices).
   Output the noisy sum to `ssgiA`.
2. **Temporal pass** (`ssgi_temporal.comp`) — reproject `ssgiB`→`ssgiA` with the
   FSR/TAA velocity buffer, 3×3 min/max clamp against history neighborhood,
   depth-change rejection, EMA (weight ≈0.9). Identical structure to
   `ao_temporal.comp`. Write the ½-res result, then a small atrous/box
   upsample into full-res `ssgiOut`.

**Confidence / fallback:** the ray pass emits a per-pixel coverage (fraction of
rays that resolved). Low coverage (screen edge, overdraw, occluded by
off-screen geometry) means SSGI is unreliable there — the composite weights the
SSGI term by coverage and falls back to the ambient/probe path, so there is no
hard seam.

**Composite:** additive ambient term next to the existing AO sample
(`composite.comp:117`), multiplied by the same `shadowDarkFactor` so GI does
not leak into direct-shadowed areas (matching the IBL shadowing behavior in
`scene.frag:178-190`).

**Env debug knobs** (house pattern): `ENGINE_SSGI_DISABLED=1`,
`ENGINE_SSGI_RAYS=<n>`, `ENGINE_SSGI_RANGE=<m>`, `ENGINE_SSGI_WEIGHT=<f>`,
`ENGINE_SSGI_TWEIGHT=<f>`, `ENGINE_SSGI_HALFRES=0/1`.

**Cost target:** well under the SSR pass (which linear-marches ~232 depth
samples with no HiZ). ½-res + HiZ early-out + 16–24 rays.

**Validation:** screenshot a ground-contact / corner case; enable/disable the
knobs and confirm no black-flicker on AMD (the DCC doc — SSGI writes an
unblended storage image, so it should be safe, but verify); confirm
temporal stability by capturing 30 frames and checking there is no boil.

**Files:** `pass/gi/VulkanGIPass.{h,cpp}`, `shaders/pass/gi/{ssgi,ssgi_temporal,ssgi_up}.comp`,
`Vulkan.cpp` pass registration (after `vulkanAOPass`, before volumetric),
`composite.comp` consume, scene-buffer GI slot.

## Phase 3 — streamed SH probe grid / irradiance volume (far + off-screen)

**Goal:** the main feature. Fill the hole SSGI cannot — off-screen occlusion
and long-range bounce — with a world-space grid of probes that streams exactly
like the heightmap tiles.

**Model:** a regular 3D grid of **L2 SH irradiance** probes (or, if specular
in-probes is wanted later, a low-res octahedral radiance card per probe — that
is EEVEE's "sphere probe" and is optional). Diffuse-only L2 SH is the
default: cheap, 9×3 coeffs, interpolates well, and plugs straight into the
Lambert-convolved SH evaluation the engine already has
(`evaluateSHIrradiance`, `scene.frag:58`).

**Bake (offline, `tools/`).** Cast rays from each probe over the *same* world
the height bake uses (Azgaar `.map` heightfield + `tools/` prop placements from
the vegetation/props pipeline), collecting:

- sky irradiance (from the same IBL used at runtime, for consistency),
- occluded direct-light contribution,
- nearest-surface albedo for diffuse bounce.

Bake per **block** (e.g. a 64 m × 64 m × 64 m block holds a 4×4×4 = 64-probe
cube of L2 SH). Output is a deterministic binary/pak block keyed by block
coordinates. This is structurally identical to the navmesh bake
(`scripts/build-navmesh.sh`, `SKIP_NAVMESH=1`) and the vegetation bake
(`build-vegetation-builder.sh`) — reuse that exact flow: `tools/gi-probe-builder`
+ `scripts/build-gi-probes.sh`, run from `build.sh` with `SKIP_GI_PROBES=1`.

**Runtime streaming.** A new component (next to `HeightmapTerrain`)
streams probe blocks around the camera, evicting/regenerating them, and exposes
`probeInterpolate(worldPos) -> L2 SH` (trilinear over the block's 8 corners,
then the SH eval). Blocks load from the baked pak; the *heights* still come
from the deterministic source, so a missing/fresh block can be re-derived
consistent with the contract (probe data is the one thing we *do* persist —
it's baked content, like navmesh, which is allowed; the *surface* stays
pure-functional).

**Integration.** In `scene.frag`, the final ambient becomes:

```
irrProbe   = evaluateSH(probeInterpolate(worldPos));
irrLocal   = mix(skySH, tileSH, skyVis);      // Phase 1, near/terrain
irradiance = mix(irrProbe, irrLocal, ssgiCoverage /*or a blend weight*/);
```

Probe provides the far baseline; Phase 1 refines terrain; SSGI adds the
near-field color bleed on top (additive or a screen-confidence blend).

**Budget:** a rolling update is unnecessary for the *static* bake (it's just a
texture fetch + interp), so this phase is cheap at runtime. The cost is the
offline bake time and the streamed block memory — both bounded by block size.

**Files:** `tools/gi-probe-builder/`, `scripts/build-gi-probes.sh`,
new `ProbeVolume` streaming component (mirrors `HeightmapTerrain.h`),
`scene.frag`/`oit_accumulate.frag` ambient hook, scene-buffer grid params,
`build.sh` hook.

## Phase 4 — (optional, later) dynamic reflection probes

If dynamic *specular* bounce and moving-occluder response become needed:
render the scene into a small set of low-res cube FBOs (raster — **no RT**),
N probes per frame on a rolling schedule, validated/reprojected temporally.
This is the one GI technique here that changes with runtime motion. Defer
until Phases 1–3 are proven and the specular requirement is real.

## Risks & caveats

- **Single graphics queue:** SSGI + probe compute run inline. Mitigate with
  ½-res SSGI, a bounded probe grid, and (Phase 0.4) an async compute queue if
  profiling demands it.
- **AMD DCC** on any new 8-bit blend/fast-clear target — read
  `docs/oit-amd-dcc.md` before the first composite change.
- **Terrain is implicit:** any ray query against terrain must use
  `HeightmapSource::heightAt` (CPU) or a shader-side height lookup — never
  assume a terrain mesh. SSGI is fine because it reads the *depth prepass*,
  which the terrain already produces.
- **Determinism:** Phase 1 must be a pure function of (source, xz) — verify
  with a two-run screenshot diff from a parked player. Phases 2/3 runtime are
  not persistence-sensitive (SSGI is per-frame; probes are baked content).
- **Do not disturb the parked player/camera** (`db.db` `transform` table) when
  validating — screenshots frame a specific object for a reason.
- **No git from agents.** All version control is the user's.

## Definition of done

- `ENGINE_SSGI_DISABLED` / GI knobs toggle cleanly with no warnings
  (`-Wall -Wextra -Wpedantic -pedantic-errors`).
- Screenshots (valley, cave mouth, ground contact, snow/forest) show visible
  bounce that reads as correct, stable (no boil/flicker), and absent when GI
  is disabled (bit-identical to today's path).
- `./scripts/build.sh` builds code + shaders + the new `tools/` bake(s);
  `SKIP_GI_PROBES=1` / `SKIP_SKYVIS=1` work.
- Two-run screenshot diff from a parked player is byte-identical (determinism).
- Frame-time cost measured and within the SSGI<SSR target; probe streaming
  memory bounded by block size.

## Not doing

- Hardware/software ray tracing, BVH/acceleration structures.
- Voxel cone GI, ray-marched SDF GI, progressive radiosity, baked lightmaps
  (rejected in favor of the probe hybrid above; revisit only if the hybrid
  proves insufficient).
