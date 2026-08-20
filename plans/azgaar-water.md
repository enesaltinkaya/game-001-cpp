# Azgaar Water Rendering Plan

_Animated translucent ocean surface for the Azgaar terrain pipeline._

---

## Goal

Render a believable ocean/lake surface over the underwater Azgaar seabed so
that water cells (`h < AZGAAR_SEA_LEVEL_HEIGHT`) show as a shimmering,
animated, reflective, depth-tinted surface instead of the current grass-textured
seabed visible at negative world-Y.

The surface must:

- Sit exactly at sea level (`azgaarSeaLevelMeters()` — currently `0 m`).
- Follow the player and stretch to the horizon (hidden by existing fog).
- Animate (vertex swell + fragment ripples) using `sceneBuffer.time`.
- Blend translucently over the seabed, getting darker with depth.
- Reflect the sky (cheap Fresnel sky reflection) and catch a sun specular.
- Produce foam where it meets the shore and near wave crests.

---

## Current state (so the plan is self-contained)

**Data — `c-game/game/azgaar/AzgaarWorld.{h,c}`**

- FMG heights: `Uint8` 0–100 per Voronoi cell. `h < 20` water, `h >= 20` land.
- `AZGAAR_SEA_LEVEL_HEIGHT = 20.0`, `AZGAAR_OCEAN_DEPTH_METERS = 60.0`,
  `AZGAAR_WATER_BLEND_HIGH = 25.0`.
- `azgaarHeightToMeters()` already maps water cells to a **negative seabed**
  (`0 m` at the coast `h=20`, shelving to `-60 m` offshore `h=0`). So the
  terrain mesh **already dips below sea level for water cells** — the seabed
  geometry we want to tint/shadow exists.
- `azgaarSeaLevelMeters(world)` → `0 m` (world Y of the surface). Source of
  truth for the plane height.
- `azgaarWorldSampleHeightSmooth()` gives the smooth heightfield (catmull-rom
  over `heightGrid`), usable for shoreline mask / depth queries.

**Terrain build — `c-game/game/azgaar/AzgaarTerrain.c`**

- Streaming 5×5 tile window (`AZGAAR_TILE_RADIUS = 2`), tile `2000 m`,
  `AZGAAR_TILE_GRID_SUBDIVISIONS = 192`. Watertight, box-blurred, smooth.
- Vertices are authored **directly in world space** (vert is pure passthrough).
- Water vertices currently get a **grass-textured green tint** because
  `debugHeightColorRgb()` clamps `h < 20` up to `20`. The geometry still dips;
  only the colour is wrong and there is no surface.

**Render pass — `c-engine/renderer/vulkan/pass/azgaar_terrain/VulkanAzgaarTerrainPass.{h,c}`**

- Opaque pass: writes `sceneColor (R16G16B16A16)`, `normals (R16G16 oct)`,
  `material (R8G8B8A8)`, `depth (D32)`. `noCull = 1`, depth-write on.
- Mesh uploaded via `vulkanAzgaarTerrainSetMesh()` on a worker thread.

**Shaders — `c-engine/data/pak_0_engine/shaders/pass/azgaar_terrain/`**

- `azgaar_terrain.vert`: world-space passthrough.
- `azgaar_terrain.frag`: grass + triplanar cliff, analytic Lambert + hemispheric.

**Globals — `c-engine/data/pak_0_engine/shaders/includes/globalset.shader`**

- `SceneBuffer` has `cameras[4]`, `directionalLight`, `time`/`prevTime`, `ibl`,
  `terrain (TerrainData)`, `fog (FogData)`. **No water uniforms yet.** `time`
  is the free animation clock.
- Bindless `textures[]` + `samplers[]` exist; depth/scene color are readable in
  compute/frag passes (GTAO reads depth, SSR reads scene color) — so a water
  frag can sample both.

**Pass order — `c-engine/renderer/vulkan/Vulkan.c` (~L112)**

```
... terrain → azgaar_terrain → debug* → scene → skybox → oit → ssr
       → volumetric → decal → composite → fsr → bloom → final → ...
```

**Assets**

- No water normal/foam textures yet. Phase 1 will be **fully procedural**; art
  passes can drop in textures later. (A `rock-moss-water_Albedo.png` exists but
  is unrelated.)

---

## Design decisions & rationale

### 1. Separate `vulkanAzgaarWaterPass`, NOT folded into the terrain pass

Water is transparent, must not write `normals`/`material`/`depth` (so it does
not pollute SSR/SSAO/velocity/FSR), and uses different pipeline state (blend on,
depth-write off, single-sided). Keeping it in its own pass + its own mesh keeps
the opaque terrain pass untouched and lets water render at the right point in
the frame. **Decision: new pass + new mesh upload path, mirroring the structure
of `VulkanAzgaarTerrainPass` (which is already a clean reference).**

### 2. Camera-following "infinite ocean" grid, NOT per-tile water

A 5×5-tile (10 km) fully-subdivided grid is wasteful and still too coarse near
the camera for Gerstner swell. Instead use a **camera-centric grid** (standard
infinite-ocean technique):

- One fixed-size plane (e.g. `1024 m` square, `256` subdivisions → `4 m` cells).
- Recenter on the camera each frame, **snapped to an integer multiple of the
  cell size** so the projected wave field does not swim/slide.
- Waves are a deterministic function of **world XZ + time**, so recentering
  produces no seams.
- The grid edge is hidden by the existing `FogData` system.

This is cheap (one draw, ~65 k verts), covers gameplay view distance, and
integrates trivially with the streaming tile window because it is independent of
which tiles are loaded.

> Alternative considered (emit water quads per terrain tile in `AzgaarTerrain`):
> rejected because mixing transparent geometry into the opaque terrain pass is
> messy, and per-tile density near the camera is too low for vertex swell.

### 3. Reflections: cheap sky Fresnel first (P0), planar/SSR later (P2)

SSR on a horizontal surface is notoriously artifact-heavy at grazing angles.
Phase 0 reconstructs the **sky colour analytically in-shader** (the skybox is
procedural — see `skybox.frag` — so we can reuse its gradient + sun disc) and
blends it by Fresnel. This already reads as "ocean" and is bullet-proof. Planar
reflections (flip-camera re-render) and/or sampling the SSR buffer are deferred
to an optional P2.

### 4. Depth-driven absorption + foam from the existing depth buffer

Water reads the scene depth texture (the terrain/seabed already wrote it).
Linearizing depth gives the seabed world-Y; `waterDepth = surfaceY - seabedY`.
Use it for:

- Beer-Lambert absorption: shallow → bright/turquoise, deep → dark navy.
- Shore foam: where `waterDepth < foamShallow` (smoothstep).
- Crest foam: from the wave-height factor.

### 5. Sea level from data, not hardcoded

The shader's surface Y comes from `azgaarSeaLevelMeters(world)` pushed into a
new `WaterData` uniform. Vertex displacement is added **on top of** that. This
keeps a single source of truth shared with the height→metres mapping.

---

## Architecture

### Pipeline position

Insert **after `vulkanSkyboxPass`** (so the water fragment can read the sky it
will reflect, and the depth buffer already holds terrain + seabed) and
**before `vulkanSsrPass`**:

```
... azgaar_terrain → scene → skybox → [azgaar_water] → oit → ssr → ...
```

Water does **not** write depth, so SSR/GTAO/velocity ignore it automatically.

```
vulkanAddPass(...)  // Vulkan.c, one new line after vulkanSkyboxPass
addPass(&vulkanAzgaarWaterPass);
```

### Outputs (water pass attachments)

| Attachment           | Action                             | Notes                                       |
| -------------------- | ---------------------------------- | ------------------------------------------- |
| `sceneColor` (+MSAA) | blend (SRC_ALPHA)                  | Translucent over terrain/sky; HDR linear.   |
| `normals`            | **discard**                        | Not written — water must not feed SSR/SSAO. |
| `material`           | **discard**                        | Not written.                                |
| `depth`              | depth-test on, **depth-write off** | So fish/seabed/SSR see through.             |

Implementation note: easiest is a pipeline with only the `sceneColor` color
attachment bound and `vulkanBeginRender` configured to load (not clear) it.
Verify `vulkanCreatePipe` supports an "attachment 0 only + keep depth" config;
if not, bind all three color attachments but write `outColor` only and rely on
write masks (`COLOR_COMPONENT_BIT` none for the others). The composite/skybox
passes already do partial-attachment rendering — follow their pattern.

---

## New / modified files

### New files

| File                                                                     | Purpose                                                                                                                                               |
| ------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| `c-engine/renderer/vulkan/pass/azgaar_water/VulkanAzgaarWaterPass.{h,c}` | System `vulkanAzgaarWaterPass`; owns the player grid mesh, pipeline, upload worker, frame draw.                                                       |
| `c-engine/data/pak_0_engine/shaders/pass/azgaar_water/azgaar_water.vert` | Camera-following grid; Gerstner/sum-of-sines vertex displacement; pass world XZ + wave derivatives to frag.                                           |
| `c-engine/data/pak_0_engine/shaders/pass/azgaar_water/azgaar_water.frag` | Fresnel sky reflection, depth absorption, sun specular, shore/crest foam, procedural ripples.                                                         |
| `c-game/game/azgaar/AzgaarWater.{h,c}`                                   | CPU side: builds/recenters the grid mesh, pushes sea level + params each frame via `vulkanAzgaarWater*` setters. (Mirrors the `AzgaarTerrain` split.) |
| `plans/azgaar-water.md`                                                  | This file.                                                                                                                                            |

### Modified files

| File                                                                            | Change                                                                                                                                         |
| ------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| `c-engine/renderer/vulkan/Vulkan.c`                                             | `addPass(&vulkanAzgaarWaterPass);` after skybox; include header.                                                                               |
| `c-engine/data/pak_0_engine/shaders/includes/globalset.shader`                  | Add `WaterData` struct + `WaterData water;` member to `SceneBuffer`.                                                                           |
| `c-engine/renderer/vulkan/resources/VulkanResourceManager.{h,c}`                | `WaterData` CPU mirror (match std430); `vulkanResourceSetWaterParams(...)`.                                                                    |
| `c-engine/renderer/vulkan/resources/VulkanFrameResources.{h,c}`                 | Expose `vulkanFrameResourcesGetSceneColor()` / `GetDepth()` if not already public (SSR/GTAO already read these — confirm and reuse).           |
| `c-engine/CMakeLists.txt`                                                       | Add the new `.c` pass + the new `AzgaarWater.c`.                                                                                               |
| `c-game/CMakeLists.txt`                                                         | Add `AzgaarWater.c`.                                                                                                                           |
| `scripts/shaders.sh` (or build script)                                          | Compile the two new GLSL → SPV (debug + release, as the other passes do).                                                                      |
| `c-game/game/loadingAzgaar/LoadingAzgaar.c` (or wherever the world lifetime is) | Call `azgaarWaterInit(world)` once the world is loaded and `azgaarWaterDestroy()` on teardown; call `azgaarWaterUpdate(cameraPos)` each frame. |
| `c-engine/renderer/Renderer.c`                                                  | Optional: `settingsGetBool("waterDisabled")` toggle like SSR/skybox.                                                                           |

---

## SceneBuffer additions (`globalset.shader`)

```c
struct WaterData {
    vec4  surfaceY;            // x = sea level world Y (from azgaarSeaLevelMeters)
    vec4  shallowColor;        // RGB shallow tint (linear), A = shallow depth (m)
    vec4  deepColor;           // RGB deep tint (linear), A = max absorption depth (m)
    vec4  foamColor;           // RGB foam, A = foam shore threshold (m)
    // Gerstner waves: direction.xy (normalized), amplitude, wavelength,
    // speed, steepness. Pack 4 waves into a mat4x4 / array.
    vec4  waveDirAmp[4];       // xy = dir, z = amplitude(m), w = wavelength(m)
    vec4  waveSpeedSteep[4];   // x = speed, y = steepness, zw = pad
    float fresnelPower;
    float fresnelScale;
    float normalStrength;
    float rippleScale;         // world-space freq of fragment ripples
    float windAngle;           // dominant wind direction (radians)
    float sunSpecularPower;
    float sunSpecularIntensity;
    float enabled;             // 0/1
};
```

CPU mirror in `VulkanResourceManager` with the same field order; setter
`vulkanResourceSetWaterParams(const WaterParams* p)`. `AzgaarWater.c` fills it
from `azgaarSeaLevelMeters(world)` + tunables (start from constants, expose via
the debug GUI later).

---

## Shader design

### `azgaar_water.vert`

Inputs: `vec3 inPosition` (flat grid in local XY, Z=0), `vec2 inUV`.

```glsl
// 1. Snap grid to camera: worldXZ = snap(cameraXZ, cell) + localXY
// 2. Flat surface Y from sceneBuffer.water.surfaceY.x
// 3. Sum of N Gerstner waves (gerstner(pos, dir, amp, wavelength, speed, steep, t))
//      -> accumulates vertical offset dz + horizontal offsets dx,dy
//      -> accumulates partial-derivative normals (analytic) into frag
// 4. worldPos = vec3(worldXZ.x + dx, surfaceY + dz, worldXZ.y + dy)
// 5. gl_Position = viewProjection * worldPos
// Outputs to frag: worldPos, worldXZ (un-displaced, for ripple UV), analytic
// normal (from Gerstner derivatives), waveHeightFactor (for crest foam).
```

Gerstner gives analytic normals for free — no need for normal-mapping the swell.

### `azgaar_water.frag`

```glsl
// Inputs: worldPos, worldXZ, baseNormal, waveHeightFactor, clipPos (for depth)
// 1. Depth: read scene depth at gl_FragCoord, linearize -> seabed world Y.
//    waterDepth = max(surfaceY - seabedY, 0). Early-out (discard) if the
//    geometry in front is ABOVE sea level (land poking through): seabedY >
//    surfaceY + eps => not water here.
// 2. Procedural ripples: 2-3 octaves of scrolling noise/gerstner normals in
//    worldXZ (animated by time), perturb baseNormal, scaled by normalStrength.
//    N = normalize(perturbedNormal).
// 3. View dir V. Fresnel = schlick(max(dot(N,V),0), F0=0.02).
// 4. Reflection color: analytic sky (reuse skybox.frag gradient + sun disc
//    fn), sampled along reflect(-V, N). skyRefl.
// 5. Sun specular: Blinn-Phong along reflect(sunDir) * sunSpecularIntensity.
// 6. Absorption: depthT = smoothstep(0, deepColor.a, waterDepth).
//    waterColor = mix(shallowColor.rgb, deepColor.rgb, depthT).
// 7. Foam: shoreFoam = smoothstep(foamColor.a, 0, waterDepth) (1 at shore).
//    crestFoam  = smoothstep(0.7, 1.0, waveHeightFactor).
//    foamA = clamp(max(shoreFoam, crestFoam), 0, 1) * foamColor.a.
// 8. Compose HDR linear:
//    color = mix(waterColor, skyRefl, fresnel) + sunSpec
//    color = mix(color, foamColor.rgb, foamA)
// 9. alpha = mix(0.35, 0.9, fresnel) // more solid at grazing angle
//    alpha = max(alpha, foamA)
// outColor = vec4(color, alpha)
```

Key reuse: lift the `skybox.frag` sky-gradient + sun-disc math into a shared
include (`includes/sky.shader`) so both skybox and water sample identical sky.

---

## Phased roadmap

### Phase 0 — MVP surface (gets ocean on screen, no reflection readback)

1. `WaterData` uniform + CPU setter; defaults wired in `AzgaarWater.c` from
   `azgaarSeaLevelMeters(world)`.
2. `AzgaarWater.c`: build a 1024 m / 256-subdiv grid once; `azgaarWaterUpdate()`
   recenters it (camera-snapped) each frame; push to the pass each time it moves.
3. Pass + pipelines (opaque-ish first, blend later) registered after skybox.
4. Vert: sum-of-sines vertical displacement only (skip Gerstner horizontal +
   analytic normals initially; use up-normal). Frag: depth absorption +
   Fresnel-to-analytic-sky + sun specular + simple shore foam.
5. Fix `debugHeightColorRgb()` so seabed (h<20) gets a sandy/mud tint instead of
   green, so the visible seabed under shallow water looks right.
6. Build shaders, run `./scripts/run.sh play screenshot /tmp/water.png`.

**Done when:** a translucent, depth-darkened, lightly animated blue plane
covers water cells, reflects the sky by Fresnel, and foams at the shore.

### Phase 1 — Surface quality

- Replace sum-of-sines with full **Gerstner** waves + analytic normals.
- Add procedural fragment ripples (multi-octave) + wind direction.
- Tune deep/shallow/foam colors and thresholds per map (coastline reads from
  `azgaarWorldSampleHeightSmooth` distance-to-shore if a smarter mask is needed).
- Expose toggles/tunables in the in-game debug GUI
  (`c-engine/renderer/gui/rmlui/guis/debugGui`): enable, wave amp, fresnel, foam.
- Add `settingsGetBool("waterDisabled")` kill switch.

### Phase 2 — Reflections (optional, choose one)

- **(a) SSR sampling:** sample the existing SSR reflection buffer in the water
  frag, mixed under Fresnel, for local reflections of terrain/objects near
  shore. Cheap; reuse existing SSR output. Watch grazing-angle artifacts.
- **(b) Planar reflection:** render the scene flipped about `surfaceY` into a
  half-res reflection target (one extra opaque draw, masked to the water area),
  sample with distortion. Highest quality, most expensive.
  Reference: `plans.old/planar-reflections.md` + the `sspr-summary.md` history.

### Phase 3 — Underwater & integration

- Underwater tint/fog when the camera is below `surfaceY`: add a screen-space
  tint + depth fog in `composite.frag` (gated by a uniform `cameraSubmerged` +
  `waterDepthAtCamera`), reusing `WaterData` colors. Disables sky reflection
  when looking up from underwater.
- Buoyancy / physics: clamp the player's feet to `max(terrainY, surfaceY)` in
  the player controller so you can wade at the shore and "swim" on the surface.
- Decals/objects (boats, docks) sort correctly against the transparent surface
  (consider routing water through the existing OIT pass if alpha conflicts show
  up with vegetation/decals).

---

## Validation

Per `AGENTS.md`, build with `./scripts/build.sh` (compiles sources **and**
shaders). For visual checks:

```bash
./scripts/build.sh
./scripts/run.sh play screenshot /tmp/water.png     # skip menu, capture a frame
./scripts/run.sh play log 5000 && cat build/c-game/data/game.log
```

Acceptance checklist per phase:

- **P0:** water cells show a translucent depth-tinted surface; no green seabed
  at shore; sky reflected at grazing angles; shore line has foam; no z-fighting
  vs terrain; no SSR/SSAO/FSR artifacts (water did not write normals/depth).
- **P1:** swell + ripples animate with `sceneBuffer.time`; grid edge hidden by
  fog; tunables work in the debug GUI.
- **P2:** reflections of near-shore terrain/objects appear, blended by Fresnel.
- **P3:** camera underwater shows tinted, fogged view; player floats at shore.

If anything crashes, run with `ENGINE_DEBUG=1` and `gdb` (per `AGENTS.md`); use
`./scripts/run.sh play log 5000` to capture a complete log.

---

## Risks / open questions

- **Partial-attachment pipeline config:** confirm `vulkanCreatePipe` /
  `vulkanBeginRender` support writing only `sceneColor` while keeping
  `normals`/`material`/`depth` loaded (write-masked). If not, fall back to
  writing all three but with `outNormal`/`outMaterial` write masks disabled —
  verify the depth-write-off path still lets the seabed show through.
- **MSAA resolve:** water blends into the MSAA `sceneColorMsaa`; make sure the
  resolve still happens for water pixels (mirror skybox/OIT handling).
- **Grid edge vs fog:** confirm `FogData` fog start distance is short enough to
  occlude a `1024 m` grid at the horizon; otherwise enlarge the grid or move it
  with a clipmap (deferred to P1 if visible).
- **Sea level assumption:** `azgaarSeaLevelMeters()` is currently a constant
  `0 m`. If a future map uses a non-zero offset, surfaceY is driven from data,
  so no shader edits needed.
- **Time unit:** confirm `sceneBuffer.time` is in milliseconds/seconds so wave
  `speed` tuning is sane (check how `FogData.fogTime` is produced and reuse the
  same scale).
