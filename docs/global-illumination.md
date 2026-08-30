# Global Illumination — survey & recommendation

Status: survey (no code implemented). All engine facts below were verified against the
current tree (re-checked pass order, G-buffer formats, Forward+ config, scene.frag ambient
block, AO/volumetric temporal passes and orphan shader locations); pass order, G-buffer
formats and Forward+ configuration cite the source files they were read from.

Scope question: which GI methods are suitable for this renderer, and what would integrating
one look like?

---

## 1. Do we even need GI?

Today the renderer produces a complete image with IBL (image-based lighting) as the **only**
ambient source. In `scene.frag` (pak engine `shaders/pass/scene/scene.frag`) the ambient block
computes `ambientDiffuse` from a prefiltered radiance / SH-L1 irradiance / environment-map
fallback and `ambientSpecular` from the prefiltered map + BRDF LUT; the final line is

```glsl
vec3 color = (ambientDiffuse + ambientSpecular) * shadowDarkFactor + Lo;
```

where `shadowDarkFactor` only attenuates ambient by cascade shadow (with a grazing-angle
recovery) and `Lo` is the direct light term (sun + contact shadow + Forward+ point/spot).
So the entire "global" contribution in the image is:

- a **constant, scene-independent** irradiance from the environment map, and
- `shadowDarkFactor` suppression.

What IBL-only ambient does _not_ do: it ignores geometry. A floor inside a cave still gets
sky irradiance; a bright red wall does not tint its surroundings; soft shadow penumbrae from
non-shadow-casters (vegetation, props) are missing; bounce light from the terrain into
valley walls does not exist. The payoff of adding GI is exactly one term — a **scene-
responsive diffuse irradiance** — not a rewrite of lighting.

Cost side: every credible technique adds (a) a per-frame estimate pass, (b) a dedicated
temporal filter (the AO pass already establishes this pattern — see §2), and (c) a risk to
FSR 3.1's reactive mask, which suppresses accumulation when luminance changes are judged
unstable. If we accept those costs, the method must be cheap enough for the GTX 1080 Ti
floor: half/internal-resolution estimate, a few rays per hemisphere sample, no world-space
updates over a multi-kilometer streaming region.

Conclusion: it is worth doing, but only as a _diffuse-irradiance upgrade of the ambient
term_, single-bounce in quality, with the IBL environment remaining the sky/miss fallback.

---

## 2. Engine inventory (what a GI method can reuse)

### 2.1 Pass order (per-frame)

From `vulkanInit()` in `c-engine/renderer/vulkan/Vulkan.cpp`:

```
culling → depth → occlusion → hiz → shadow → contact_shadow → light_culling →
heightmap_terrain → azgaar_props → debug_navmesh → scene → skybox → azgaar_river →
azgaar_water → azgaar_weather → oit_accumulate → oit_composite → ssr → ao →
volumetric → decal → composite → taa → dof → fsr → bloom → final → lpm → lens →
debug_physics → rmlui
```

A screen-space GI pass can slot in **after `scene` (or after `oit_composite`, to keep
transparent objects out of the G-buffer it reads) and before `ao`/`composite`**, mirroring
where `ssr` and `ao` sit. Its irradiance output would be consumed two ways: (a) next-frame
`scene.frag` ambient term (ping-ponged through its own temporal pass, one frame of latency
hidden by the temporal filter — the same cadence as TAA/AO histories), and (b) the
`azgaar_props` (vegetation) forward pass ambient.

### 2.2 G-buffer contents (per `c-engine/renderer/vulkan/resources/VulkanFrameResources.cpp`)

All at the FSR-scaled internal resolution (`window.renderWidth/Height`):

| Slot        | Format              | Content                                                                                      |
| ----------- | ------------------- | -------------------------------------------------------------------------------------------- |
| albedo      | R16G16B16A16_SFLOAT | per-pixel base albedo                                                                        |
| normals     | R16G16_SFLOAT       | oct-encoded world normal                                                                     |
| material    | R8G8B8A8_UNORM      | roughness/metallic/alphaMask/0 (`vec4(roughness, metallic, alphaMask, 0)` from `scene.frag`) |
| view normal | R16G16_SNORM        | normalized view-space normal, **xy components only** (z reconstructed at use)                |
| velocity    | R16G16_SFLOAT       | pixel velocity from clip-space positions, xy only                                            |
| depth       | D32_SFLOAT          | main depth (strip/scene)                                                                     |

Every input a screen-space irradiance estimator needs (albedo, normals, depth, velocity,
camera jitter state via `jitteredUvFromUnjittered`, `prevViewProjection`,
`invViewProjection`) already exists. **Nothing new must be rasterized.**

### 2.3 Lighting architecture

- Forward+: 16-px screen tiles, `MAX_LIGHTS_PER_TILE 64`
  (`c-engine/renderer/vulkan/pass/light_culling/VulkanLightCullingPass.cpp`),
  `MAX_GPU_LIGHTS 1024` (`c-engine/ecs/system/light/LightComponent.h`).
- Direct sun: cascaded shadow maps (`shadow` pass) + screen-space contact shadow
  (`contact_shadow` pass).
- IBL: prefiltered radiance, SH-L1 irradiance, BRDF LUT, per-entity intensity
  (`VulkanIbl.cpp`, `scene.frag` ambient block). This is the term GI replaces for the
  diffuse channel; IBL **specular** stays as-is.
- Transparent objects: OIT accumulate/composite (see `docs/oit-amd-dcc.md` — AMD DCC
  intermittently mis-decompresses `R8_UNORM` multiplicative blending + fast-clear
  combinations; any new pass writing `R8` blends must be tested on that path).

### 2.4 Temporal / upscaling machinery

- TAA runs at internal resolution on the composite output; FSR 3.1 upscales to the window.
  Internal resolution is FSR-scaled, so **any new buffer must key off
  `window.renderWidth/Height`, not `window.width/Height`**.
- The AO pass is the precedent for a noisy estimate + dedicated temporal filter:
  CACAO spatial estimate, then `ao_temporal.comp` (jittered reprojection + velocity +
  depth clamp, `c-engine/renderer/vulkan/pass/ao/VulkanAOPass.cpp`). The volumetric light
  shafts pass has an analogous temporal pass. A GI estimate should ship the same way.
- GPU floor: GTX 1080 Ti class (Vulkan 1.3, RADV). Budget: half-res or internal-res
  estimate, ≤ 4–8 hemisphere rays per sample, no world-space per-frame updates.
- Per-pass GPU cost is measurable with `ENGINE_LOG_PASS_GPU=1`.

### 2.5 World constraints

- **No SDF, no world mesh.** Terrain is an implicit vertex-shader lattice over streamed
  heightmap tiles (512² heights per 2048 m tile), and the **determinism contract** forbids
  persisted per-tile state: evicted tiles are regenerated bit-identically from the source.
  Any world-space GI state is only viable if it is regenerated _with_ the tile from pure
  source data, and it must not cover per-instance vegetation (which is scattered per-tile
  and lives only on the GPU).
- Vegetation (`azgaar_props` pass) is a Lambert-only Forward+ path and currently gets the
  IBL ambient; a GI upgrade must cover it, or bright ground will read wrongly against
  un-lit props.

---

## 3. Candidate methods

Comparison scored against the concrete constraints in §2.

| Method                                                                                                                           | Produces                                   | GPU cost                                                         | VRAM                                                                                                             | Streaming terrain                                                                                                                      | TAA/FSR                                                                                                                 | OIT/transparents                                                                               | Vegetation                                                                              | Integration effort                                                                                       | Verdict                                                                                                                                                                                                               |
| -------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------ | ---------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **SSGI / SSGI++** (screen-space hemisphere radiance)                                                                             | 1-bounce diffuse irradiance + sky fallback | Low-medium (half/internal res, few rays)                         | ~30–60 MB (history)                                                                                              | Perfect (no world state)                                                                                                               | Clean (own temporal pass, AO precedent)                                                                                 | Reads OIT-composited G-buffer or pre-OIT; transparents simply unlit by GI (accepted)           | Yes (reuse G-buffer path in azgaar_props ambient)                                       | **Low** — inputs all exist; one estimate + one temporal pass + one `scene.frag`/`azgaar_props.frag` edit | **Primary pick**                                                                                                                                                                                                      |
| **Screen-space dynamic radiance probes** (probe grid over the viewport, SSGI-style or light-cone sampled, temporal)              | Bilinearly interpolated irradiance         | Low (coarser than per-texel SSGI)                                | Negligible                                                                                                       | Perfect                                                                                                                                | Clean                                                                                                                   | Same as SSGI                                                                                   | Yes                                                                                     | Low-medium                                                                                               | **Fallback** (if SSGI ray cost blows the 1080 Ti budget or artifacts dominate, probes give ~70% of the quality at ~30% of the cost)                                                                                   |
| **Streamed per-terrain-tile GI probes** (bake irradiance into each 2048 m tile at stream time, regenerated like heightmap tiles) | Multi-bounce, long-range soft ambient      | Near-zero runtime (sampler lookup); bake cost in tile generation | Small (per-tile probe grid)                                                                                      | OK **only** because tile streaming + determinism make regenerated probes cheap; bake cost multiplies tile (re)generation time          | Clean                                                                                                                   | N/A                                                                                            | **No** — per-instance props excluded; probes + screen-space term would be needed anyway | High (tile pipeline, Jolt physics already regenerates from heights; probes must stay pure f(source))     | **Optional phase 2** for long-range/multi-bounce ambience; not a replacement for screen-space                                                                                                                         |
| **Lumen-style SDF GI** (hardware rays vs. voxel SDF + screen-space fallback)                                                     | Full-quality diffuse GI                    | High (SDF ray tracing is the whole cost)                         | Very high (world-scale SDF grid; multi-GiB at our world scale, or a streaming SDF that must regenerate per tile) | Infeasible at practical quality: unbounded Azgaar world, no SDF representation exists, regeneration must obey the determinism contract | FSR jitter is handled in Lumen, but every extra input to the scene color re-tests the reactive mask                     | Ray-hits-transparent objects are not supported by SDF GI; our OIT path needs special treatment | No — vegetation density makes the SDF grid far too coarse/aliased                       | Very high                                                                                                | **Unsuitable** — rejected (no SDF infra, unbounded world, vegetation, and the 1080 Ti floor is simply below Lumen's realistic budget)                                                                                 |
| **Light propagation volumes / classic LPM** (volumetric light diffusion through a screen-space grid)                             | Bounce light from emissive/lit pixels      | Medium (3D grid blur over the view)                              | Medium (3D grid at reduced res)                                                                                  | In principle OK (screen-space), but…                                                                                                   | The volumetric grid fights TAA jittering badly; light bleeds across depth discontinuities and must be clamped per frame | N/A                                                                                            | Yes                                                                                     | Medium                                                                                                   | **Unsuitable** — its signature artifact (light leaking through thin occluders) is the worst fit for a terrain+vegetation world; SSGI gives the same result class with far less leakage and a clean temporal precedent |

### Method notes

- **SSGI / SSGI++**: per-texel (or half-res) hemisphere rays marched against the depth
  buffer, hit radiance = albedo × local radiance, ray miss falls back to the IBL sky
  (already the ambient term we're replacing — energy-consistent by construction). SSGI++
  adds the cheap temporal/spatial refinements (history reuse before reprojection,
  directional filtering). Depth-edge fades hide the screen-edge "void" problem; the
  IBL-sky fallback keeps sky-visible areas correct.
- **Screen-space dynamic probes**: probe grid (e.g. 32–64 columns) each sampling a small
  probe-local SSGI or a few light-cone rays; bilinear interpolation to pixels. Coarser
  but far cheaper; the natural fallback tier.
- **Per-tile probes** are the only way to get _multi-bounce_ ambience cheaply; they belong
  in the tile streaming pipeline (`HeightmapTerrain` service) and must be a pure function
  of the tile's source data — which is fine because the bake can read the same `.map`
  heights — but they cannot see vegetation, so they complement, not replace, the screen-
  space term.
- **SDF/Lumen rejection** is final: the determinism contract + infinite streaming terrain +
  per-instance vegetation + 1080 Ti floor each independently disqualify it.
- **LPM rejection**: light leakage through thin geometry is the dominant artifact and our
  scene (trees, grass, low walls) is full of thin geometry.

---

## 4. Recommendation

**Primary pick: SSGI / SSGI++ screen-space diffuse irradiance**, replacing the IBL
`ambientDiffuse` term in `scene.frag` (and the azgaar_props ambient), with the IBL sky as
ray-miss fallback.

**Fallback: screen-space dynamic radiance probes** — same buffers and integration points,
coarser estimate; switch over only if the SSGI ray cost or artifact budget fails on the
1080 Ti.

**Optional phase 2:** streamed per-tile irradiance probes for long-range/multi-bounce
ambience, regenerated with terrain tiles (determinism contract preserved).

### 4.1 Passes & buffers the SSGI design needs

New pass: `VulkanGiPass` (engine side, `c-engine/renderer/vulkan/pass/gi/`), registered
after `oit_composite` and before `ao` in `vulkanInit()`. Two compute shaders + one
consumer-side change:

| Item                     | Purpose                                                                                                                                                                                           | Format / size         |
| ------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------- |
| `gi_estimate.comp`       | Half-internal-res hemisphere ray march (4–8 rays) over the G-buffer; out = radiance + confidence (edge fade, depth-delta confidence)                                                              | write to `giCurrent`  |
| `gi_temporal.comp`       | Jittered reprojection (reuse `jitteredUvFromUnjittered` + velocity + depth clamp, modeled on `ao_temporal.comp`), confidence-weighted mix, luminance-change clamp for the FSR reactive mask       | ping-pong `giHistory` |
| `scene.frag` edit        | `ambientDiffuse = mix(iblDiffuse, giSample * shadowDarkFactor, giWeight)`; reduce CACAO AO weight when GI is on (SSGI encodes diffuse occlusion — avoid double darkening); IBL specular untouched | —                     |
| `azgaar_props.frag` edit | same GI ambient for vegetation                                                                                                                                                                    | —                     |

Buffers (all at `window.renderWidth/Height`, i.e. FSR-scaled internal resolution):

- `giCurrent` — R16G16B16A16_SFLOAT (RGB radiance, A confidence), internal res, single slot.
- `giHistory` — same format, 2 slots (ping-pong, one extra slot if we sample last-frame
  for `scene.frag` consumption while filtering this frame). At 2560×1440 internal that's
  ≈ 23 MB per slot — trivial next to existing G-buffer/velocity storage.

Inputs consumed: albedo, normals, material, depth, velocity (all §2.2) — **no new
rasterization, no new attachment on the scene pass**.

### 4.2 Relation to existing passes

- **Replaces (one term, not a pass):** the IBL `ambientDiffuse` in `scene.frag` and
  `azgaar_props.frag` becomes a _fallback_ (used where the GI estimate has no confidence,
  e.g. screen edges / ray misses).
- **Coexists, with a weight trade-off:** CACAO AO — GI already contains diffuse occlusion;
  scale AO down when GI is active. Today AO is applied as a plain `composite *= aoFactor`
  in `composite.comp` (no strength knob exists), so the attenuation would be a _new_
  uniform, e.g. `aoStrength` (default 1.0), multiplied into `aoFactor`.
- **Coexists, unchanged:** cascaded shadows + contact shadow (still multiply the direct
  term _and_ gate the GI output via `shadowDarkFactor` — keep GI from re-lighting
  shadowed areas), SSR (specular channel, complementary), volumetric light shafts
  (volumetric direct light, complementary), Forward+ (unchanged).
- **Runs alongside, same machinery:** TAA/FSR (GI history lives at internal res, same
  jitter phase as everything else; FSR sees the filtered output, not the noise).
- **Explicit non-goals:** OIT-transparent objects don't receive GI (accepted artifact —
  they keep IBL ambient); GI does not add multi-bounce (phase-2 concern); GI does not
  touch the terrain determinism contract (it is purely screen-space, no world state).

### 4.3 Must-be-true conditions (drop-in acceptance checklist)

1. GI runs at FSR-scaled internal resolution, before `scene`'s ambient is consumed, and
   feeds `scene.frag`/`azgaar_props.frag` via one sampler — using the same jitter/velocity
   machinery as `ao_temporal.comp`.
2. Energy consistency: GI ambient × `shadowDarkFactor` matches IBL ambient brightness in
   open sky (IBL-sky fallback guarantees this on ray miss), and AO is attenuated when GI
   is on (no double darkening).
3. Vegetation (`azgaar_props`, Lambert-only Forward+) receives the same GI ambient.
4. OIT-transparent objects receiving no GI is an accepted, documented artifact.
5. The FSR 3.1 reactive mask does not fire from GI shimmer — the temporal filter clamps
   per-texel luminance change (validate with FSR on, camera orbiting, `ENGINE_LOG_PASS_GPU=1`).

### 4.4 Verification plan

- `./scripts/build.sh` (compiles code + shaders + pak).
- Screenshots: `./scripts/run.sh play screenshot /tmp/gi.jpg`, with
  `ENGINE_HIDE_GUI=1` for a clean frame.
- Frame-buffer dumps for the new buffers via `ENGINE_DEBUG_DUMP_IMAGES`.
- Budget check: `ENGINE_LOG_PASS_GPU=1` — SSGI estimate + temporal together should stay
  well under the AO+SSR combined cost on the 1080 Ti.
- Debug switches, modeled on the AO ones: env-var toggle for GI on/off and for dumping the
  unfiltered estimate vs. the filtered history.

---

## 5. Prior work: orphan SSGI/GI shaders

`c-engine/data/pak_0_engine/shaders/pass/ssgi/spv/` and
`.../shaders/pass/gi/spv/` contain compiled **debug** SPIR-V with **no `.comp` sources and
no C++ passes**:

- `ssgi/spv/ssgi.comp.spv.debug`, `ssgi/spv/ssgi_temporal.comp.spv.debug`
- `gi/spv/gi_initial.comp.spv.debug`, `gi_gather.comp.spv.debug`,
  `gi_blur.comp.spv.debug`, `gi_temporal.comp.spv.debug`

Embedded symbols show a prior session prototyped exactly the recommended architecture:
hemisphere ray directions, depth ray trace, IBL-sky fallback on miss
(`sampleIblSky`), depth-edge fade, jittered reprojection (ssgi + ssgi*temporal), and a
second generation (gi*_) that moved the per-texel work into a `traceRay` gather shader
(`gi_gather`) with separate initial/blur/temporal stages. The artifacts are orphaned — no
sources means the next pak rebuild drops them — so treat them as a **reference design only**,
not something to decompile. They confirm the phase-1 plan was already exercised in this
codebase; the `gi\__` set in particular is the screen-space-probes fallback variant, which
is a nice match for the fallback recommendation.
