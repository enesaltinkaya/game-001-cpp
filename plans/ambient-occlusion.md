# Ambient Occlusion (Screen-Space GTAO, XeGTAO) — Implementation Plan

## Goal

Add a screen-space ambient occlusion pass that darkens crevices, corners, and
ground-contact areas. Screen-space (not baked) so it works with the streaming
heightmap terrain and dynamic objects with zero offline content.

## Approach: XeGTAO (GPU-GTAO) with HiZ early-out + temporal accumulation

Reference: NVIDIA GDC 2015 ("GPU-GTAO", Daniel Ráth) and the XeGTAO sample in
the Microsoft DirectX 12 SDK. Two compute dispatches per frame, both inside a
single `VulkanAOPass` system (multi-pipe-in-one-system precedent:
`VulkanHiZPass` runs copy + downsample pipes):

1. **Ray pass** (`ao.comp`) — per-pixel XeGTAO: ~32 jittered rays over a disk
   in view space; each ray is marched outward with *doubling* steps and uses
   the existing HiZ mip chain (`vulkanHiZGetMipSampledIndex(mip)`) to
   early-out — O(log N) depth samples per ray instead of a linear march.
   Each hit is converted to a horizon angle and integrated per pixel
   (weighted by `1 + dot(V, L)` and a range fade). Output is a noisy,
   single-frame occlusion factor (0 = occluded, 1 = open) in a new `ao`
   frame resource (R8).
2. **Temporal pass** (`ao_temporal.comp`) — the "G-TAO" part, mirroring
   `VulkanTaaPass`: reproject the previous accumulation with the `velocity`
   frame resource, 3×3 min/max clamp of the current sample against the
   history neighborhood (kills ghosting), depth-change rejection, then an
   EMA blend (history weight ≈ 0.9). Output is a ping-ponged `aoAccum`
   R16F image (`aoA`/`aoB`, same static-image + `swapchainCreated`
   recreation pattern as TAA's `taaA`/`taaB`), cleared to
   `(1.0, 0.0)` = "no occlusion, no history".

Why XeGTAO over the previous HBAO plan:

- The HiZ early-out machinery is the same either way; the ray-marched GTAO
  is the canonical reference implementation with a well-understood tuning
  surface, and it is *designed* around temporal accumulation. 32 rays/frame
  is intentionally under-sampled — the dedicated history clamp is what
  removes boiling and ghosting, instead of hoping generic TAA denoises a
  per-pixel integrated HBAO result.
- The dedicated temporal pass operates on the AO signal *before* lighting
  (in the composite), with its own tunables (weight, clamp, rejection) —
  cleaner than double-accumulation through the final TAA.
- Cost: worst case 32 rays × ~16 steps, but HiZ early-out means typical
  rays hit within 4–8 samples. Compare the current SSR pass, which
  linear-marches up to 232 depth samples per pixel (128 near + 96 coarse +
  8 refinement) with *no* HiZ. AO should come in well below SSR cost.

Note: this is independent of `plans/sdfgi-plan.md`. If SDFGI is ever built,
its probe-occlusion stage can replace this pass; until then XeGTAO is the
cheap, content-free answer.

## Pipeline placement

```
… depth → … → HiZ → … → OitComposite → SSR → AO (new: ray + temporal)
   → Volumetric → Decal → Composite (samples aoAccum) → TAA → FSR → …
```

- Register `vulkanAOPass` in `vulkanInit()` right after `vulkanSsrPass`
  (`Vulkan.cpp:204`). Inputs (`Depth`, `Normals`, HiZ current frame,
  `Velocity`) are all ready by then.
- The final TAA (after Composite) still accumulates end color; it now sees a
  *stable* AO input, so no double-accumulation concerns.
- AO is applied in `composite.comp` on the **geometry path only**
  (`!isSky`), after the SSR/reflection blend (step 7) and **before fog**
  (step 8), so fogged geometry fades to fog color instead of staying dark.

## Engine conventions (verified — copy, don't reinvent)

| Concern | Convention / helper |
| --- | --- |
| View space | RH, **camera looks down −Z** (cglm RH_ZO; `CameraSystem.cpp:205`). Disk rays point `(dx, dy, −1)` in view space; a march step behind the near plane (`hitPos.z > −zNear`, `cameras[0].zNear`) terminates the ray. |
| HiZ | R32G32_SFLOAT mip chain, **reverse-Z** (0 = far/empty, 1 = near): `.r` = min (farthest surface), `.g` = max (closest surface) (`hiz_copy_depth.comp`). Access: `vulkanHiZGetMipSampledIndex(mip)`, `vulkanHiZGetMipCount()`. |
| World position / normal | Unjittered-UV convention from `ssr.comp`: `uv + jitterOffset` → NDC → `invViewProjectionNoJitter`; normal = `OctDecode` of `Normals.rg`. |
| Jitter | 128×128 R8 blue-noise LUT at `sceneBuffer.ibl.blueNoiseIndex` (`VulkanIbl.cpp`, `BLUE_NOISE_SIZE = 128`); rotate the LUT lookup per frame with `cameras[0].frameIndex` (golden-angle ≈ 2.3999632) for temporal stability. |
| Velocity | `vulkanFrameResourcesGetVelocity()` — R16G16_SFLOAT, **pixel units**; reproject with `prevUv = uv − mv / res` (taa.comp convention). |
| Depth rejection | Reuse TAA's `depthToInv` (inverse view depth, affine in reverse-Z; taa.comp) — relative S-space comparison, tight threshold without false-firing on depth noise. |
| Ping-pong accumulator | `VulkanTaaPass` pattern: static images, recreated on `swapchainCreated` signal, transient-command clear on creation. |
| Multi-pipe pass | `VulkanHiZPass` pattern: two `vulkanCreatePipe`s in one `System`, per-pipe `vulkanBeginProfile`/`vulkanEndProfile`. |

## Files

### New

| File | Purpose |
| --- | --- |
| `c-engine/renderer/vulkan/pass/ao/VulkanAOPass.h` | `VulkanAOPass : System("ao")`, mirrors `VulkanSsrPass.h` (disabled getter/setter) plus `vulkanAOPassGetOutput()` returning the current `aoAccum` ping-pong image (mirrors `vulkanTaaPassGetOutput()`, used by the `ao` debug-dump token). |
| `c-engine/renderer/vulkan/pass/ao/VulkanAOPass.cpp` | Two pipelines (`ao`, `aoTemporal`). Ping-pong `aoA`/`aoB` (R16F, `SAMPLED\|STORAGE\|TRANSFER_DST`) like TAA, cleared to `(1.0, 0.0)` via transient command. `update()`: dispatch ray pass → dispatch temporal pass (swap ping-pong per frame). If disabled or inputs missing: clear the current `aoAccum` to `(1.0, 0.0)` and skip both dispatches; `vulkanAOPassIsDisabled()` drives the composite index (below). `ENGINE_AO_DISABLED` env in `added()`, exactly the SSR pattern. Push constants: ray pass — depth/normals/blueNoise index, per-mip HiZ sampled indices + `hizMipCount`, output index, width/height, `zNear`, `pixelsPerUnit`-derived mip math inputs; temporal pass — `ao` index, prev/curr `aoAccum` indices, velocity index, depth index, width/height, `historyWeight`. 8×8 workgroups. |
| `c-engine/data/pak_0_engine/shaders/pass/ao/ao.comp` | Ray pass (details below) |
| `c-engine/data/pak_0_engine/shaders/pass/ao/ao_temporal.comp` | Temporal accumulation (details below) |

No CMake edits: `c-engine/CMakeLists.txt` uses `file(GLOB_RECURSE "*.cpp")`
and `scripts/shaders.sh` picks up new `.comp` files automatically.

### Modified

| File | Change |
| --- | --- |
| `c-engine/renderer/vulkan/resources/VulkanFrameResources.h/.cpp` | New `ao` frame resource: `VK_FORMAT_R8_UNORM` (full res, `COLOR_ATTACHMENT` + `SAMPLED` + `STORAGE` usage), `vulkanFrameResourcesGetAO()`. (If the AMD DCC flicker from `docs/oit-amd-dcc.md` ever appears on this buffer — it shouldn't, since it's compute-written, not blend+renderpass — switch to `R16_UNORM`.) |
| `c-engine/renderer/vulkan/Vulkan.cpp` | `#include` + `addPass(&vulkanAOPass);` after `vulkanSsrPass` (line 204). Add an `"ao"` token to `vulkanDebugDumpFrameImages()` (map to `vulkanAOPassGetOutput()`) so `ENGINE_DEBUG_DUMP_IMAGES=ao,taa,…` dumps the accumulated AO next to screenshots. |
| `c-engine/renderer/vulkan/pass/composite/VulkanCompositePass.cpp` | Add `aoIndex` to `CompositePushConstants`: `vulkanAOPassIsDisabled() ? 0xFFFFFFFFu : (u32)vulkanAOPassGetOutput()->sampledPoolIndex` (same absent-sentinel pattern as `weatherMaskIndex`). |
| `c-engine/data/pak_0_engine/shaders/pass/composite/composite.comp` | Add `aoIndex` push constant. In the `!isSky` branch, after step 7: `if (pc.aoIndex != 0xFFFFFFFFu) composite *= texelFetch(ao, coord).r;` |
| `c-engine/renderer/Renderer.cpp` | `if (utils::settingsGetBool("aoDisabled")) vulkanAOPassSetDisabled(1);` — mirror line 73 (`ssrDisabled`). |
| `c-game/game/settingsGui/graphics/SettingsGraphicsGui.cpp` | AO On/Off toggle, exactly the `ssrDisabled` pattern (label static ~58/68, bind ~118, refresh ~168/340, `utils::settingsSetBool("aoDisabled", …)` ~352). |

## Shader details — ray pass (`ao.comp`)

Conventions: copy the `ssr.comp` header block (`#version 460` + four
`#extension` lines, `utils.shader` / `globalset.shader` includes,
`local_size_x/y = 8`).

Per pixel:

1. `depth = texelFetch(depth, coord).r`; `depth == 0` → sky: write AO = 1.0,
   return.
2. Reconstruct `worldPos` (unjittered-UV convention) and
   `N = OctDecode(Normals.rg)`; `viewPos = cameras[0].view * worldPos`,
   `distToCam = length(viewPos)`, `N_view = cameras[0].view * N`.
3. **Ray disk** (`RAY_COUNT = 32`, `diskRadius = 1/sqrt(RAY_COUNT)`): for ray
   `i`, `bn = tex(blueNoise, (coord % 128)/128 + (frameIndex * 2.3999632) %
   1)`;
   - `angle = 2π * (halton2(i) + bn.x)` — base-2 Halton is bit-reversal in
     GLSL (or substitute the golden-ratio sequence `fract(i * 0.618034)`;
     both are cheap equidistributed per-ray angles)
   - `r = diskRadius * sqrt((i + bn.y) / RAY_COUNT)`
   - `dirView = normalize(vec3(cos(angle), sin(angle), −1.0))` (−Z forward)
   - March in **world space**: `dirWorld = normalize((cameras[0].invView *
     vec4(dirView, 0)).xyz)` (one 4×4 multiply per pixel), so every step
     projects through `viewProjectionNoJitter` exactly like
     `ssr.comp:projectToScreen` + jittered-UV correction. (Equivalent
     alternative: march in view space and project steps through the
     *jittered* `projection` matrix straight to jittered HiZ UVs — either is
     consistent; pick one, stay consistent.)
4. **March with doubling + HiZ early-out**:
   `startDist = 0.005 + 0.01 * distToCam` (self-intersection offset, scaled
   by camera distance). Ray origin is offset from the surface along the
   normal: `startPos = worldPos + N * startDist`; the march distance `t`
   also starts at `startDist` (SDK convention: first sample is
   `startPos + dir * startDist`), then `step *= 2` per iteration, capped at
   ~16 steps; `maxDist = 0.05 * distToCam` (clamped, e.g. [0.5, 20] m —
   tune) is the march limit. At each step:
   - Project step position → UV → `texelFetch` HiZ.
   - Mip selection from step size: pixels-per-unit at this distance
     `pdu = renderHeight * projection[1][1] / (2 * distToCam)`
     (`projection[1][1] = 1/tan(fovY/2)`),
     `mip = clamp(floor(log2(step * pdu)) + 1, 0, hizMipCount − 1)`.
   - Sample HiZ `.g` (max/closest) and `.r` (min/farthest); compare the
     step's reverse-Z depth `D`:
     - `D < min` → behind every surface in the texel → **no hit**, continue
       marching;
     - `D > max` → in front of the closest surface → **hit** at this `t`;
     - otherwise → descend one mip and re-test (at mip 0: `D > max` ⇒ hit,
       else no hit).
5. **Hit → horizon angle** (view space; the normal offset of `startPos`
   cancels, so `toHit = dirView * hitT`):
   `parallel = dot(toHit, N_view)`,
   `perp = sqrt(max(length(toHit)² − parallel², 0))`,
   `horizonAngle = atan(perp / max(parallel, 1e−4))`.
6. **Integrate**: `occl += (1 + dot(V, dirView)) * horizonAngle *
   rangeFade(hitT)` where `V = normalize(−viewPos)` (view direction, camera
   at the origin in view space);
   `rangeFade = 1 / (1 + (hitT / maxDist)²)` (or the
   SDK's falloff — tune).
7. **Output**: `ao = clamp(1 − (occl / RAY_COUNT) * HORIZON_SCALE, 0, 1)`,
   `HORIZON_SCALE` starts at 1.0 (tune); `imageStore` R8.

Cost: 32 rays × ≤16 steps worst case, but early-out typically lands hits in
4–8 samples — well under the current SSR's 232-step linear march.

## Shader details — temporal pass (`ao_temporal.comp`)

Inputs: `ao` (current per-frame R8), prev `aoAccum` ping-pong (R16F:
`.r` = occlusion, `.a` = S-space inverse view depth, 0 = no data),
`velocity` (pixels), `depth`.

1. `prevUv = uv − velocity / res` (taa.comp convention).
2. Resample prev accumulation; reject (blend weight → 0) when: `prevUv`
   out of bounds, `.a == 0` (no data, e.g. right after create/resize), or
   the S-space depth (via TAA's `depthToInv`) changed more than a small
   relative threshold (~0.05 — reuse TAA's `depthThreshold` idea).
3. **3×3 min/max clamp** of the current per-frame AO against the resampled
   history neighborhood — the standard XeGTAO ghost-removal.
4. `accum = mix(clampedPrev, current, 1 − HISTORY_WEIGHT)`,
   `HISTORY_WEIGHT = 0.9` (tune); write `.r = accum`,
   `.a = depthToInv(depth)` (0 for sky pixels).
5. Sky pixels (`depth == 0`): write `(1.0, 0.0)`.

## Phases

1. **Skeleton (½ day)** — new `ao` frame resource, `VulkanAOPass` with the
   ray pass only (no temporal yet; composite samples per-frame `ao`
   directly), register + run. Goal: end-to-end wiring, AO visible in
   screenshots (expected to be noisy without the temporal pass).
2. **Temporal pass (½–1 day)** — ping-pong images + `ao_temporal.comp`,
   composite switches to sampling `aoAccum`; disabled path clears
   `aoAccum` to 1.0.
3. **Tuning + UI (½–1 day)** — tune ray count / `maxDist` /
   `HORIZON_SCALE` / `HISTORY_WEIGHT` against real-world shots; settings
   toggle; `ao` debug-dump token.
4. **Validation** —
   - `ENGINE_HIDE_GUI=1 ./scripts/run.sh play screenshot /tmp/ao.jpg` with
     the parked player, AO on/off A-B; `ENGINE_DEBUG_DUMP_IMAGES=ao,depth,taa`
     for the raw accumulated buffer.
   - Check: crevices/corners/ground contact darken; **sky stays untouched**
     (`depth == 0` pixels); **no boiling after convergence**
     (`ENGINE_SCREENSHOT_COUNT=8 ENGINE_SCREENSHOT_DELAY_MS=15000` multi-shot,
     per `docs/screenshot.md`); **no ghost trails on camera motion**
     (temporal clamp working); no DCC flicker on AMD.
   - Perf: pass-profile timing (`VulkanProfile`) + RenderDoc if needed
     (`docs/renderdoc-capture.md`); target a few % of frame at full res,
     and expect it to land below the SSR pass cost.
   - `ENGINE_AO_DISABLED=1` (or settings off) must leave the image
     pixel-identical to pre-AO (composite skips the multiply entirely via
     the `0xFFFFFFFF` index).

## Risks / open questions

- **Direct light gets darkened too** — v1 multiplies the whole geometry
  color (`sceneCol` already has direct + ambient baked in, so true "AO on
  ambient only" would need light decomposition). Acceptable for v1; revisit
  if it reads as too dark under sun. Optional cheap mitigation: gate AO by
  the `Material` roughness channel (strong AO on rough surfaces, weaker on
  glossy ones where SSR already does the work).
- **Light bleeding** through thin geometry / across cliffs — HiZ early-out
  is conservative (max = closest), plus range fade; the origin-pixel
  `depthEdgeFade` from `ssr.comp` is reusable to suppress silhouettes.
- **Ghosting on fast motion** — the 3×3 clamp handles most cases; if
  streaks remain (e.g. through leafy canopies), lower `HISTORY_WEIGHT` or
  ramp the history weight down with motion-vector magnitude (TAA already
  does the analogous thing with its ghost threshold; the two histories
  coexist but act on different signals).
- **TAA + AO-temporal interaction** — the final TAA now sees a stable AO
  input; no known issue, but watch for over-smoothed AO edges (double
  temporal filtering); verify with a TAA-off shot if AO looks mushy.
- **R8 banding on the per-frame buffer** — the per-frame result is
  discarded after accumulation, so 8-bit is fine; the *accumulated* buffer
  is R16F. If isolated per-frame AO dumps look banded, that's expected.

## Implementation notes (2026-08-22, XeGTAO v1 landed)

Deviations from the plan above, found during implementation/validation:

- **Ray disk points −Z (away from the camera), not +Z.** A camera-facing
  (+Z) disk lifts every march step above the surface; the lifted step then
  projects onto a *farther* ground point and the HiZ test false-hits the
  ground underneath it (flat terrain reads as fully occluded in the raw
  `aoFrame` dump). With the −Z disk, near steps pass *below* the surface
  (no hit at the texel) and only real occluders above the tangent plane
  register. The ray weight was re-based on the disk axis accordingly:
  `1 + dot(−V, dirView)` (the plan's `1 + dot(V, dirView)` is ≈0 for −Z
  rays and would kill the whole signal).
- **Horizon-angle gate `parallel > 0` is mandatory.** The plan's
  `atan(perp / max(parallel, 1e−4))` yields ≈π/2 for hits *below* the
  tangent plane (all ground self-hits) → black ground. Hits below the
  plane contribute 0 (the surface already blocks the back hemisphere);
  only hits above it occlude.
- **Temporal clamp direction: history → current-neighborhood box** (the
  plan's step 3/4 were self-contradictory — "clamp the current sample
  against the history neighborhood" deadlocks a cleared all-1.0 history:
  the accumulator can never leave 1.0). The box is built from the current
  per-frame AO taps at the *reprojected* position (TAA convention, stable
  across jitter phases) and the reprojected history is clamped into it —
  the plan's own step 4 (`mix(clampedPrev, current, …)`).
- **Accumulator format is `R16G16_SFLOAT`** (`.r` = AO, `.g` = S-depth),
  not single-channel R16F — the S-space depth needs a second channel.
- **`maxDist = clamp(0.15 * distToCam, 1.0, 20.0)`** — the plan's 0.05
  scale (0.7 m at 14 m) was too short for village-scale ground contact.
- **`HORIZON_SCALE` tuned 1.0 → 0.6** — the full-strength master read as
  too dark on the parked village scene (near-black ground contact under
  trees/houses, heavy mottling under canopies). 0.6 keeps the contact cues
  legible at ~60% strength; the composite multiply stays full (no softening
  there, so the raw `ao` dump stays a true signal).
- **`HORIZON_SCALE` 0.6 → 0.4 + `AO_FLOOR` 0.5 (2026-08-22)** — 0.6 still
  read as too strong (heavily mottled meadow, black pits at grass-tuft
  bases). Two changes: (a) master strength 0.4, and (b) a per-frame floor
  `ao = clamp(1 - occl * HORIZON_SCALE, 0.5, 1.0)`, because the signal
  clamps to 0 before the scale kicks in — scale-down alone still left ~6%
  of geometry pixels near-black (saturated). The floor caps any surface at
  50% darkening, turning the black pits into soft contact shadows without
  flattening the rest of the signal. Trade-off: the raw `ao` dump is now
  a floored (not "true") signal; the R8 per-frame range is [128, 255]
  (banding there is expected, the accumulated R16F output is unaffected).
- **Off-screen break**: steps whose UV leaves the screen terminate the ray
  (clamped UVs would false-hit border geometry).
- **Extra debug-dump token `aoFrame`** (raw per-frame R8) next to `ao`
  (accumulated) — essential for ray-pass debugging (caught the two
  self-hit bugs above).

Validation (parked village scene, 2880×1627, NVIDIA):
- `ENGINE_LOG_PASS_GPU=1`: **ao = 0.98 ms** (ray + temporal; 3rd heaviest
  pass after depth 1.49 / shadow 1.60; ~6% of the 16.6 ms 60 fps frame).
  Note the plan's "below SSR" expectation is scene-dependent: here SSR
  skips almost every pixel (rough terrain) and costs 0.04 ms.
- Multi-shot boiling test (`ENGINE_SCREENSHOT_COUNT=8`, 15 s settle):
  inter-frame mean diff 2.4–2.7/255 with AO vs 1.8–2.1 without — the AO
  temporal pass adds ≈0.5/255 of residual beyond the baseline pipeline.
- A/B screenshots: ground contact at house bases + under trees + player
  contact; grass mottling is the per-frame noise mean (tunable via
  `HORIZON_SCALE` / `RAY_COUNT` / `DISK_RADIUS` / `MAX_DIST_SCALE` in
  `ao.comp`); sky untouched; `ENGINE_AO_DISABLED=1` skips the composite
  multiply entirely (sentinel index) → pixel-identical to pre-AO.
- Open: visual ghost-trail check on camera motion (3×3 clamp + S-depth
  rejection are in place; needs a moving-camera look). AMD DCC check not
  possible on the dev box (NVIDIA ICD).