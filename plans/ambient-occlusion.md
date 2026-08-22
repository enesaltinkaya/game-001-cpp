# Ambient Occlusion (Screen-Space AO) — Implementation Plan

## Goal

Add a screen-space ambient occlusion pass that darkens crevices, corners, and
ground contact areas. Screen-space (not baked) so it works with the streaming
heightmap terrain and dynamic objects with zero offline content.

## Approach: HBAO with HiZ early-out

Horizon-based ambient occlusion (Crytek HBAO), using the existing HiZ mip
chain for early-out of the horizon search:

1. Per pixel, reconstruct world position + world normal (exact same
   unjittered-UV / jitter-offset convention as `ssr.comp` and
   `composite.comp`).
2. Cast ~24–32 rays over the upper hemisphere (jittered per frame), each ray
   marches outward and uses the HiZ chain (`vulkanHiZGetMipSampledIndex(mip)`,
   mip 0 = full res; HiZ is reverse-Z: `.g` = closest, `.r` = furthest) to
   binary-search the first hit — O(log N) depth samples per ray instead of
   linear march.
3. Convert each ray hit to a horizon angle, integrate over slices
   (HBAO per-pixel integration), apply range fade + normal scaling.
4. Write an occlusion factor (0 = occluded, 1 = open) to a new `AO` frame
   resource; `composite.comp` multiplies it into the geometry color.

Why HBAO over ray-marched SSAO: the HiZ chain already exists for shadow
culling/SSR, horizon search is far cheaper than per-ray march + refinement,
and per-pixel integration gives cleaner results than per-ray accumulation.

Note: this is independent of `plans/sdfgi-plan.md`. If SDFGI is ever built,
its probe-occlusion stage can replace this pass; until then HBAO is the
cheap, content-free answer.

## Pipeline placement

```
… depth → … → HiZ → … → OitComposite → SSR → AO (new) → Volumetric → Decal
   → Composite (samples AO) → TAA → FSR → …
```

- Register `vulkanAOPass` in `vulkanInit()` right after `vulkanSsrPass`.
  Inputs (`Depth`, `Normals`, HiZ current frame) are all ready by then.
- AO is applied in `composite.comp` on the **geometry path only** (`!isSky`),
  after the SSR/reflection blend (step 7) and **before fog** (step 8), so
  fogged geometry fades to fog color instead of staying dark.
- Because AO feeds `CompositeColor` before TAA, TAA's temporal accumulation
  denoises the per-frame jittered HBAO for free.

## Files

### New

| File | Purpose |
| --- | --- |
| `c-engine/renderer/vulkan/pass/ao/VulkanAOPass.h` | `VulkanAOPass : System("ao")`, mirrors `VulkanSsrPass.h` (disabled getter/setter) |
| `c-engine/renderer/vulkan/pass/ao/VulkanAOPass.cpp` | Compute dispatch. Inputs: `vulkanFrameResourcesGetDepth()`, `GetNormals()`, `vulkanHiZGetMipSampledIndex(i)` / `vulkanHiZGetMipCount()`. Output: new `ao` frame resource via `vulkanAOPassGetOutput()`. Push constants: depth/normals/hiz index per mip, `hizMipCount`, output index, width/height. 8×8 workgroups. If disabled or inputs missing: clear the AO image to 1.0 (no occlusion) — same pattern as SSR clearing its output black. `ENGINE_AO_DISABLED` env in `added()`. |
| `c-engine/data/pak_0_engine/shaders/pass/ao/ao.comp` | HBAO shader (see below) |

No CMake edits: `c-engine/CMakeLists.txt` uses `file(GLOB_RECURSE "*.cpp")` and
`scripts/shaders.sh` picks up new `.comp` files automatically.

### Modified

| File | Change |
| --- | --- |
| `c-engine/renderer/vulkan/resources/VulkanFrameResources.h/.cpp` | New `ao` resource: `VK_FORMAT_R8_UNORM` (full res, `COLOR_ATTACHMENT` + `SAMPLED` + `STORAGE` usage), `vulkanFrameResourcesGetAO()`. (If the AMD DCC flicker from `docs/oit-amd-dcc.md` ever appears on this buffer — it shouldn't, since it's compute-written, not blend+renderpass — switch to `R16_UNORM`.) |
| `c-engine/renderer/vulkan/Vulkan.cpp` | `#include` + `addPass(&vulkanAOPass);` after `vulkanSsrPass`. |
| `c-engine/renderer/vulkan/pass/composite/VulkanCompositePass.cpp` | Add `aoIndex` to `CompositePushConstants`, pass `vulkanFrameResourcesGetAO()->sampledPoolIndex` (or `0xFFFFFFFF` if absent). |
| `c-engine/data/pak_0_engine/shaders/pass/composite/composite.comp` | Add `aoIndex` push constant. In the `!isSky` branch, after step 7: `composite *= texture(...ao...).r;` |
| `c-game/game/settingsGui/graphics/SettingsGraphicsGui.cpp` (+ `DebugGui.cpp` if it mirrors SSR) | AO On/Off toggle + `utils::settingsSetBool("aoDisabled", …)`, exactly the `ssrDisabled` pattern (line ~352 in settings gui). |

## Shader details (`ao.comp`)

Conventions (copy from `ssr.comp`):

- `#version 460` + the four `#extension` lines, include `utils.shader` /
  `globalset.shader`, `local_size_x/y = 8`.
- World pos: unjitter `uv + jitterOffset`,
  `invViewProjectionNoJitter * vec4(ndc, depth, 1)`; normal: `OctDecode` of
  `Normals.rg` via `texelFetch` at the jittered coord (same as SSR step "World
  normal").
- `depth == 0` → sky: write AO = 1.0, return.
- Depth sampling for the march: `texelFetch` on jittered UVs (SSR's
  `sampleDepth` helper is directly reusable — copy it).
- HiZ early-out: for ray at distance `t`, pick starting mip from
  `log2(t * pixelsPerUnit / 2)`, clamp to `hizMipCount`; query mip max
  (`.g`, reverse-Z = closest). If the ray's depth is in front of the HiZ max,
  descend one mip and binary-search; terminate when ray depth crosses buffer
  depth.
- Jitter: 2D blue-noise LUT (`sceneBuffer.ibl.blueNoiseIndex`, already loaded
  by IBL) rotated by `sceneBuffer.cameras.frameIndex` (Frostbite-style),
  offset the ray grid + start distance.
- Parameters (push constant or shader constants to start):
  - hemisphere rays: 24–32 (tune);
  - max radius: ~1.5 m world units;
  - normal scale: 0.2 (HBAO standard);
  - range fade: `1 / (1 + (t / radius)^2)`-style;
  - intensity: 1.0 (v1).
- Output: `imageStore` R8 occlusion factor, clamped 0..1.

## Phases

1. **Skeleton (½ day)** — new frame resource, `VulkanAOPass` with a
   trivial 16-ray linear-march SSAO (no HiZ), composite multiplication,
   register + run. Goal: end-to-end wiring, AO visible in screenshots.
2. **HBAO + HiZ (1–2 days)** — replace march with horizon search +
   early-out, per-pixel integration, blue-noise jitter, range fade.
3. **Tuning + UI (½–1 day)** — tune ray count / radius / normal scale
   against real-world shots; settings + debug GUI toggle; optional debug
   view showing the raw AO buffer (the `ENGINE_SCREENSHOT` token loop in
   `Vulkan.cpp` can gain an `"ao"` token for free).
4. **Validation** —
   - `ENGINE_HIDE_GUI=1 ./scripts/run.sh play screenshot /tmp/ao.jpg` with
     the parked player, AO on/off A-B.
   - Check: crevices/corners/ground contact darken; **sky stays untouched**
     (depth == 0 pixels); no flicker after TAA settles; no DCC flicker on AMD.
   - Perf: pass-profile timing (`VulkanProfile`) + RenderDoc if needed
     (`docs/renderdoc-capture.md`); target: a few % of frame at full res.
   - `ENGINE_AO_DISABLED=1` must leave the image pixel-identical to
     pre-AO (composite multiplies by 1.0).

## Risks / open questions

- **Direct light gets darkened too** — v1 multiplies the whole geometry
  color (sceneCol already has direct + ambient baked in, so true
  "AO on ambient only" would need light decomposition). Acceptable for v1;
  revisit if it reads as too dark under sun.
- **Light bleeding** through thin geometry / across cliffs — HiZ early-out
  is conservative (max = closest), plus range fade and the edge-fade idea
  from `ssr.comp` (`depthEdgeFade` is reusable).
- **TAA ghosting**: HBAO is temporally stable after jitter settles; if
  ghosting on motion appears, add a cheap velocity-aware clamp in the AO
  pass or lower TAA exposure — check after phase 3.