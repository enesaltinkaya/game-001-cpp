# Azgaar GPU Particle Weather Plan

_Real GPU-simulated weather particles (snow / rain / dust / leaves) —
compute-shader integration in a device-local buffer, depth-buffer ground
collision, camera-following wrap volume, instanced billboard rendering._

**Status:** implemented. Replaces the fullscreen-quad "Implementation v1" of
workstream F in `plans/azgaar-world-population.md` (that plan's F section
now points here). Wind wiring and the climate→condition table stay in the
world-population plan unchanged.

Implementation notes (vs. the sketches above):

- The NDC→uv mapping in `weather_update.comp` is `uv.y = 0.5 − ndc.y·0.5`
  (the engine renders through a Y-flipped viewport; same convention as the
  HiZ culling shader). A plain `ndc·0.5+0.5` mirrors the sample row and
  wrongly kills sky particles against near ground.
- Dust respawns in a fixed ±2 m band around camera height, not a fraction
  of the flattened box half-height (the camera is ~1.7 m above ground; a
  fraction of a 12 m half-box lands below the terrain and the depth kill
  would wipe every spawn within a frame).
- First activation snaps to the target state (nothing was on screen
  before, so there is no pop); the ~4 s cross-fade applies to condition
  changes afterwards.

---

## Goal

Weather that behaves like geometry in the world, not a screen effect:

- Particles live in **world space** inside a box around the camera
  (camera-following wrap volume → infinite field, zero CPU work per
  particle).
- A **compute shader** integrates positions each frame: terminal-velocity
  fall + wind advection + turbulence; particles **die on the scene depth
  buffer** (terrain, trees, buildings, water) and respawn above.
- Rendering is an **instanced billboard draw** reading the particle buffer:
  depth-tested, occluded by props, soft depth-fade at intersections,
  rain rendered as velocity-stretched streaks, all shapes procedural
  (no new assets).
- Type/density/wind come from the Azgaar climate sample at the camera +
  `winds[0]`, **cross-faded over ~4 s** on condition changes (old-type
  particles finish falling as their spawn type).

## Why not the fullscreen-quad version

The old v1 (per-pixel hash lattice in a fullscreen pass) is cheap but
fundamentally screen-space: no occlusion (flakes render in front of the
tree you stand under), no depth interaction (no soft ground contact),
parallax only approximated, cost scales with resolution instead of
particle count, and it can never grow into splashes / embers / fireflies.
The GPU particle version costs ~0.1 ms simulate + a small instanced draw,
gets all of the above right, and its volume + respawn infrastructure is
reusable for future ambient particle systems (P5).

## Current state (infrastructure this builds on — all verified in tree)

- **Passes** are `System` structs registered via `addPass()` in
  `c-engine/renderer/vulkan/Vulkan.c` (`vulkanInit`); a pass `update()`
  may record both compute and graphics work (the culling pass dispatches
  compute inside its update).
- **Compute pattern** (`VulkanCullingPass.c` + `scene_culling.comp`):
  `VulkanPipe` with `.comp = "shaders/pass/.../spv/x.comp.spv"`,
  `vulkanBindPipe` / `vulkanPush` / `vulkanDispatch`, 64-bit buffer
  device addresses in push constants (`GL_EXT_buffer_reference`), then a
  `compute-write → vertex-read` `vkCmdPipelineBarrier`.
- **Bindless global set 0** is auto-included in every pipeline
  (`vulkanCreatePipe`): sampled `textures[MAX_IMAGES]` by index, and
  `sceneBuffer` via the address buffer — giving compute direct access to
  `cameras[0]` (`position`, `viewProjection`, `zNear/zFar`, `viewport`),
  `time` / `prevTime` (ms) and every data block.
- **In-frame depth sampling** (`VulkanAzgaarWaterPass.c`): the pass
  transitions the scene depth image to
  `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` and passes
  `depthImage->sampledPoolIndex` as a push constant; its fragment shader
  linearizes depth. The weather compute (ground kill) and fragment (soft
  fade) reuse exactly this.
- **Instanced instance-rate vertex input** (`VulkanAzgaarPropsPass.c`):
  `VK_VERTEX_INPUT_RATE_INSTANCE` binding with a POD stride — the model
  for binding the particle buffer to the graphics pipeline.
- **SceneBuffer data blocks**: `WaterData`, `FogData`, `AzgaarPropsData`
  … in `globalset.shader` with CPU mirrors + setters in
  `VulkanResourceManager` — `WeatherData` joins them.
- **Game side**: `winds[6]` already parsed (`azgaarParseWinds`);
  `azgaarWorldSampleClimate` gives temp/prec/biome at any point;
  `azgaarWaterUpdate(camX, camZ)` is called per frame from `Player.c` —
  the wiring point for the weather module.
- **Shader utils** (`utils.shader`): `snoise` (2D/3D), `hash`, IGN —
  turbulence + GPU RNG.

## Design decisions

### D1 — One engine pass, two stages

`azgaar_weather` (`VulkanAzgaarWeatherPass`), registered in `Vulkan.c`
between `vulkanAzgaarWaterPass` and `vulkanOitAccumulatePass` (the slot
the old plan reserved). Its `update()` records, in order:

1. depth image transition to shader-read (water-pass pattern),
2. `weather_update.comp` dispatch (integrate + ground-kill + respawn),
3. barrier: particle buffer compute-write → vertex-shader-read,
4. instanced draw of the particle buffer (billboards / streaks).

Single command buffer, no extra queues, no separate update pass. Rendering
into the scene color target like the water pass → fog/tonemap/TAA apply
for free.

### D2 — One persistent in-place particle buffer, no ping-pong

Each compute thread reads and writes only `particles[gl_GlobalInvocationID]`
— no cross-thread access, so a single device-local buffer updated in place
is race-free (no per-frame-in-flight copies, no atomics, no compaction
pass).

```glsl
// 16 B, std430-clean (vec4 array)
struct GpuWeatherParticle {
    vec4 posSeed;   // xyz = world position; w = packed meta:
                    //      bits 0..7 = spawn type, bits 8..31 = per-particle seed
};
```

No stored velocity and no life field: velocity is **analytic** per type
(terminal fall + wind + seeded turbulence from `sceneBuffer.time`) —
physically right for small particles at terminal velocity, halves
bandwidth, and makes type cross-fades trivial (the `type` byte lets a
snowflake spawned before a warm front finish falling as snow while new
spawns are rain). Buffer: `MAX_WEATHER_PARTICLES = 65536` (1 MB), usage
`STORAGE | VERTEX | shader device address`, created once at pass init and
pre-seeded (particles distributed through the volume, mixed types, so
first activation doesn't spawn a "wall" of flakes at the box top).

### D3 — Camera-following wrap volume (torus)

The simulation domain is an AABB centred on the camera (half-extents from
`WeatherData`, defaults 90 m xz / 30 m y). Per update:

```
rel = mod(p.pos - camPos + half, 2*half) - half;   // wrap each axis
rel += velocity(type, seed, t) * dt;
p.pos = camPos + rel;
```

Wrapping preserves relative offsets → particles appear **world-stationary**
while the camera moves through an infinite field; camera motion generates
zero respawn traffic. Box edges are invisible because particle size ≪ box
size (risk 6).

### D4 — Ground collision via this frame's scene depth

After integration, project the wrapped position with
`cameras[0].viewProjection` → uv + view depth; sample the depth texture
(bindless, by index, transitioned per D1); linearize (copy the water
pass' code); if `particleViewZ > sceneViewZ + 0.05 m` → the particle is
inside opaque geometry → **respawn**: new seeded position in the top band
of the box (`y = camY + half.y·rand(0.6,1)`, xz uniform in the box), new
type rolled from the `WeatherData.types` weight vector, new seed. No uv →
offscreen particle: keep (it will wrap/die soon).

This kills snow on terrain **and** on tree canopies / building roofs for
free (they're in the depth buffer), which is exactly the occlusion
behaviour the fullscreen quad could never have. Camera under cover:
particles behind the roof get killed from the camera's viewpoint —
imperfect shelter, acceptable v1 (risk 4).

GPU RNG: `pcg2d(seed, frameIndex)` — deterministic per frame, no state.

### D5 — Density via respawn roulette, not draw-count changes

Each respawned particle rolls `enabled = rand < WeatherData.params.z`
(target density 0..1). Disabled particles keep simulating but collapse to
zero size in the vertex shader. The draw always issues `MAX` instances
(65 536 × 6 verts ≈ 0.4 M — trivial; zero-area prims are discarded before
rasterization, so disabled particles cost no fill). Density changes
therefore never pop: particles leave by dying on the ground, arrive as
new enabled spawns. (If profiling ever shows the constant draw cost, a
compute compaction pass is the measured follow-up — not in v1.) When
`look.w == 0` (weather off) the whole pass early-outs before the dispatch.

### D6 — Rendering: procedural billboards / streaks, water-pass recipe

- **Geometry**: no vertex buffer — `vkCmdDraw(6, MAX, 0, 0)` with
  `gl_VertexIndex` making the two-triangle quad corners; the particle
  `posSeed` arrives through an instance-rate input binding (props-pass
  pattern, stride 16).
- **Vertex**: unpack type/seed; camera-facing quad from `invView`
  right/up; **rain replaces "up" with the screen-projected velocity
  direction** (project `pos` and `pos + v·0.06 s`, use the screen-space
  delta as the long axis) → streaks that align with apparent motion and
  shear with the wind. Size = per-type base × seed jitter × `params.w`,
  clamped to ≥ 2 px (TAA); alpha fades from `look.z` (far fade) to the
  box edge.
- **Fragment**: procedural alpha shapes (soft disc for snow, streak
  gradient for rain, low-alpha soft blob for dust, pointed spinning
  ellipse for leaves — rotation from `seed + time`), `tint` from
  `WeatherData` (dust tinted with the local biome colour, CPU-side),
  cheap analytic light (hemispheric + sun dot), the **same fog block the
  water shader applies** so distant flakes sit in the haze, and a
  **soft-particle depth fade**: reconstruct scene view depth at the
  fragment, fade alpha over the last ~0.5 m — no hard clipping where
  flakes meet ground.
- **Pipeline**: premultiplied alpha blend, depth test on / depth write
  off, MSAA on — the water pass' transparent recipe verbatim
  (`VulkanPipeInfo` blend flags).

### D7 — `WeatherData` in the SceneBuffer

Single source of truth read by compute + vertex + fragment; CPU mirror +
`vulkanResourceSetWeather(...)` in `VulkanResourceManager` (the
`FogData`/`WaterData` pattern). Push constants carry only the mechanical
bits: particle buffer address, depth texture index, max count.

```glsl
struct WeatherData {
    vec4 wind;    // xy = dir (unit, world xz), z = speed m/s, w = turbulence 0..1
    vec4 types;   // spawn weights: x snow, y rain, z dust, w leaves (CPU-normalized)
    vec4 params;  // x = box half xz (m), y = box half y (m), z = density 0..1, w = size scale
    vec4 look;    // x = global opacity, y = fall speed scale, z = far fade start (m), w = enabled
    vec4 tint;    // rgb = particle tint (dust = biome colour), a unused
};
```

### D8 — Game-side state machine (`AzgaarWeather.{h,c}`)

- Every 500 ms: `azgaarWorldSampleClimate` at the camera → condition per
  the world-population plan's F table (snowfall `temp < −1`, drizzle
  `temp < 3 && prec > 60`, dust storm `biome 1 && temp > 25`, leaves
  `biome 6`) → target `types` weights + per-type `params`/`look`.
- Lerp every `WeatherData` field toward the target at ~0.25 /s (≈ 4 s
  transitions).
- Wind = `winds[0]` + a slow CPU gust noise; the **same gust value**
  feeds `WeatherData.wind`, `AzgaarPropsData.wind` (sway) and
  `WaterData.windAngle` (set in `AzgaarWaterUpdate`) so flakes, grass and
  waves stay coherent.
- `azgaarWeatherUpdate(camX, camY, camZ)` called from `Player.c` beside
  `azgaarWaterUpdate`; init/destroy wired in `LoadingAzgaar.c`
  (mirrored in the release path).
- `ENGINE_AZGAAR_WEATHER=0..4` forces none/snow/rain/dust/leaves and
  bypasses the climate logic.

### D9 — Determinism note

Parameters (climate, winds, type mix) are deterministic like the rest of
the Azgaar systems; individual particle positions are ephemeral GPU state
re-rolled per frame — nothing persists, nothing is read by gameplay. This
is by design (weather is visual only).

## Per-type parameters (initial)

| type   | fall (m/s)     | size (m)            | full-density fraction | behaviour                                                              |
| ------ | -------------- | ------------------- | --------------------- | ---------------------------------------------------------------------- |
| snow   | 1.2–2.0 (seed) | 0.04–0.10 disc      | 1.00 (65 536)         | strong turbulence sway, slow spin                                      |
| rain   | 9–11           | 0.03 × 0.5–0.9 str. | 0.35                  | near-vertical + wind shear; splash = P5                                |
| dust   | 0.3–0.8        | 0.2–0.6, α ≈ 0.06   | 0.50                  | box flattened (half-y ≈ 12 m), spawns in the lower third → hugs ground |
| leaves | 0.8–1.5        | 0.10–0.18 ellipse   | 0.08                  | flutter/spin; autumn palette = P5                                      |

Compute cost: 65 536 / 64 = 1024 groups — well under 0.1 ms.

## Shader sketches

### `weather_update.comp` (local_size_x = 64)

```glsl
layout(push_constant) uniform Push {
    uint64_t particleAddress;
    uint depthIndex;      // scene depth, bindless sampled pool
    uint maxParticles;
    uint _pad;
};
#include "../../includes/globalset.shader"
// particles via buffer_reference at particleAddress; sceneBuffer gives
// cameras[0], time/prevTime, weather.

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= maxParticles) return;
    GpuWeatherParticle p = particles[i];
    uint type = uint(p.posSeed.w) & 0xFFu;
    uint seed = uint(p.posSeed.w) >> 8u;

    float dt = min((sceneBuffer.time - sceneBuffer.prevTime) * 1e-3, 0.05);
    vec3  cam = sceneBuffer.cameras[0].position.xyz;

    // D3 wrap into the camera box
    vec3 rel = mod(p.posSeed.xyz - cam + half, 2.0 * half) - half;

    // D2 analytic velocity: terminal fall + wind + seeded turbulence
    vec3 v = fallVelocity(type, seed) * weather.look.y
           + vec3(weather.wind.xy * weather.wind.z, 0.0)
           + turbulence(rel, seed, sceneBuffer.time, weather.wind.w);
    rel += v * dt;

    // D4 ground test against this frame's depth
    vec4 clip = sceneBuffer.cameras[0].viewProjection * vec4(cam + rel, 1.0);
    vec2 uv   = clip.xy / clip.w * 0.5 + 0.5;
    if (uv in [0,1]) {
        float sceneZ = linearizeDepth(texture(textures[depthIndex], uv, SAMPLER_NEAREST),
                                      zNear, zFar);
        if (clipViewZ(clip) > sceneZ + 0.05) {
            // respawn at the box top; roll type from weather.types weights;
            // density roulette (D5) sets enabled via seed bit 31
            ...
        }
    }
    particles[i] = p;
}
```

### `azgaar_weather.vert`

Instance-rate `posSeed` in; unpack; quad corners from `gl_VertexIndex`;
axis = camera right/up, or projected velocity for rain-type streaks;
out world pos, view depth, type/seed, per-particle alpha (density
roulette bit, far fade, size clamp).

### `azgaar_weather.frag`

Procedural shape alpha by type (seed for variation/spin) → tint ×
analytic light → fog block copied from `azgaar_water.frag` → soft-particle
fade: `alpha *= clamp((fragViewZ - sceneViewZ) / 0.5, 0, 1)` with the
depth sampled at the fragment uv.

## Sync checklist

- **Depth image**: depth-attachment write (depth pass) → shader read
  (compute + fragment): the water pass' `vulkanTransition` before the
  dispatch; the draw reuses the same layout.
- **Particle buffer**: compute write → vertex read:
  `vkCmdPipelineBarrier` between dispatch and draw (culling-pass
  pattern).
- **`WeatherData`**: written CPU-side with the existing per-frame
  SceneBuffer upload — no extra sync.

## New & modified files

### New

| File                                                                         | Purpose                                                                         |
| ---------------------------------------------------------------------------- | ------------------------------------------------------------------------------- |
| `c-engine/renderer/vulkan/pass/azgaar_weather/VulkanAzgaarWeatherPass.{h,c}` | Pass: particle buffer, compute dispatch, instanced draw, `WeatherData` plumbing |
| `c-engine/data/pak_0_engine/shaders/pass/azgaar_weather/weather_update.comp` | Simulation (wrap, integrate, depth kill, respawn)                               |
| `c-engine/data/pak_0_engine/shaders/pass/azgaar_weather/azgaar_weather.vert` | Billboard / streak expansion from the particle buffer                           |
| `c-engine/data/pak_0_engine/shaders/pass/azgaar_weather/azgaar_weather.frag` | Procedural shapes, lighting, fog, soft depth fade                               |
| `c-game/game/azgaar/AzgaarWeather.{h,c}`                                     | Climate→condition state machine, cross-fades, wind coherence                    |

### Modified

| File                                        | Change                                                   |
| ------------------------------------------- | -------------------------------------------------------- |
| `c-engine/renderer/vulkan/Vulkan.c`         | Register `vulkanAzgaarWeatherPass` between water and OIT |
| `.../shaders/includes/globalset.shader`     | `WeatherData` block in `SceneBuffer`                     |
| `.../resources/VulkanResourceManager.{h,c}` | CPU mirror + `vulkanResourceSetWeather(...)`             |
| `c-game/game/player/Player.c`               | `azgaarWeatherUpdate(...)` next to `azgaarWaterUpdate`   |
| `c-game/game/loadingAzgaar/LoadingAzgaar.c` | Init/destroy wiring; `ENGINE_AZGAAR_WEATHER` force var   |
| `plans/azgaar-world-population.md`          | Section F points here (done in this change)              |

## Milestones

1. **Falling field**: pass skeleton + buffer + compute (wrap + analytic
   fall, no depth kill) + camera-facing billboards. Screenshot: snow
   falling around the camera (wrong intersections expected).
2. **Grounded**: depth kill + respawn + soft-particle fade + density
   roulette. Snow stops at terrain/roofs/canopies, fades softly.
3. **Types**: rain streaks, dust (flattened box, biome tint), leaves
   (spin/flutter), type cross-fade + `ENGINE_AZGAAR_WEATHER` forcing.
4. **World-driven**: climate state machine, gust-coherent wind across
   weather/props/water, debug logging.

Est. effort: 3–5 days engine + 1–2 days game wiring.

## Validation

Per `AGENTS.md`:

```bash
./scripts/build.sh                                        # compiles C + shaders
./scripts/run.sh play screenshot /tmp/azgaar_weather.png  # + ENGINE_CAM_TELEPORT to a biome
./scripts/run.sh play log 5000 && cat build/c-game/data/game.log
```

Checks:

- Taiga: snow drifts with `winds[0]`; standing under a conifer → flakes
  occluded by the canopy; no flakes rendering in front of near geometry;
  soft (not clipped) ground contact.
- Desert: dust hugs the ground, tinted by the biome colour.
- Forest (biome 6): sparse falling leaves, spinning.
- Teleport taiga → desert: ~4 s cross-fade, old snow finishes as snow.
- Sway/ripple direction matches flake drift (single gust source).
- Pass profile (`System.gpuElapsed`): sim ≤ 0.15 ms, draw ≤ 0.4 ms at
  1080p full-density snow.
- Zero new warnings.

## Debug levers (existing naming pattern)

| var                             | effect                                                    |
| ------------------------------- | --------------------------------------------------------- |
| `ENGINE_AZGAAR_WEATHER=0..4`    | force none/snow/rain/dust/leaves (bypass climate)         |
| `ENGINE_AZGAAR_WEATHER_COUNT=N` | pool size override (min(max, N))                          |
| `ENGINE_AZGAAR_WEATHER_DEBUG=1` | log condition transitions, type mix, density, sim/draw ms |

## Risks / open questions

1. **TAA ghosting on rain streaks** (fast bright-thin geometry).
   Mitigation: ≥ 2 px width, soft alpha edges; if still visible, shorten
   streaks or drop streak brightness. Revisit after milestone 3.
2. **Fill rate at 4K with full snow.** 65k small soft quads ≈ a few MP of
   fragments; caps via density/size; measure with the pass profile.
3. **Depth-kill pops** when an occluder newly enters the view between
   particle and camera — one wrong-frame death, invisible in practice;
   soft fade absorbs the common case.
4. **Shelter under roofs is approximate** (kill is camera-viewpoint
   based). Proper local occupancy needs the same infrastructure as local
   mist — noted for P5 together with it.
5. **`mod()` wrap seam**: a particle crossing the box edge teleports to
   the far side — with particle size ≪ box size this is imperceptible;
   verify at box edges (debug: `ENGINE_AZGAAR_WEATHER_DEBUG` could draw
   the box wireframe — optional).
6. **Compute-sampled depth + compute-written vertex input** are both
   established engine patterns (culling → scene; HiZ sampled in
   `scene_culling.comp`) — no new device features required.
7. **MSAA + alpha blend ordering** among particles: unsorted additive-ish
   premultiplied snow reads fine (matches water); OIT is overkill here —
   revisit only if dust/rain layering artifacts show up.
