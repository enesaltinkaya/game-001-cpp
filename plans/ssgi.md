# Screen-Space Global Illumination (SSGI / SSGI++) — Implementation Plan

## Goal

Replace the scene-independent IBL `ambientDiffuse` term with a **screen-space,
single-bounce diffuse irradiance estimate** so crevices, canyons and canopies
actually modulate ambient light, and bright surfaces bounce light into their
surroundings. The IBL environment stays as the **sky / ray-miss fallback**
(energy-consistent by construction), the IBL **specular** term is untouched,
and the terrain determinism contract is untouched (purely screen-space, no
world state).

Method: SSGI / SSGI++ — per-texel hemisphere ray march against the depth
buffer, hit radiance = `albedo × local radiance`, temporal filter with
depth/normal edge continuity, spatial refinement, injection into the ambient
term of `scene.frag` and `azgaar_props.frag`.

This plan builds on the survey `docs/global-illumination.md` (method selection
and engine inventory; read it first). Where the plan below differs from the
survey, the deviation is called out in "Survey corrections & fact checks".

## Status

- [ ] Phase 1 — GI estimate + debug visibility
- [ ] Phase 2 — Temporal filter (ping-pong history)
- [ ] Phase 3 — Ambient injection (`scene.frag` / `azgaar_props.frag`) + AO attenuation
- [ ] Phase 4 — Vegetation/props coverage + FSR reactive-mask validation

## Background

- Today the entire "global" contribution is a constant, scene-independent
  IBL irradiance plus `shadowDarkFactor` suppression. The payoff of GI is
  exactly one term: a scene-responsive diffuse irradiance (survey §1).
- The AO pass is the structural precedent: noisy estimate + dedicated
  temporal filter with its own reprojection history, ping-pong accumulators,
  absent-sentinel output consumed by a later pass (`VulkanAOPass` →
  `composite`). `VulkanGiPass` mirrors that shape exactly (survey §2.4, §4.1).
- All inputs the estimator needs already exist at FSR-scaled internal
  resolution (`window.renderWidth/Height`): albedo (`R16G16B16A16_SFLOAT`),
  oct-encoded world normals (`R16G16_SFLOAT`), material
  (`R8G8B8A8_UNORM`), velocity (`R16G16_SFLOAT`, pixels), depth
  (`D32_SFLOAT`) — no new rasterization, no new scene-pass attachment
  (survey §2.2, verified against `VulkanFrameResources.cpp`).
- Prior prototypes exist as **orphan compiled debug SPIR-V only** (no sources,
  no C++ passes; the next pak rebuild drops them): `ssgi/{ssgi,
  ssgi_temporal}.comp.spv.debug` and `gi/{gi_estimate, gi_initial, gi_gather,
  gi_blur, gi_temporal}.comp.spv.debug` under
  `c-engine/data/pak_0_engine/shaders/pass/`. Treat them as a reference
  design (hemisphere rays, depth ray trace, IBL-sky miss fallback, depth-edge
  fade, jittered reprojection), not something to decompile (survey §5).
- GPU floor: GTX 1080 Ti class (Vulkan 1.3, RADV). Historical reference
  from the original AO landing (parked village, 2880×1627): `ao = 0.98 ms`
  on a 16.6 ms 60 fps frame. These numbers come from the **removed XeGTAO
  implementation** — `plans/ambient-occlusion.md` is marked SUPERSEDED and
  `plans/cacao-ao.md` records no GPU cost for the current CACAO pass — so
  P1 re-baselines the `ao` pass with `ENGINE_LOG_PASS_GPU=1` before any
  budget comparison.

## Survey corrections & fact checks

Facts re-verified against the current tree while writing this plan. Items 1
and 2 are corrections to `docs/global-illumination.md`; items 3–5 are the
residual checks the survey left implicit.

1. **AO strength knob exists** — the survey's §4.2 claim "no strength knob
   exists, attenuation would be a new `aoStrength` uniform in
   `composite.comp`" is **stale**. CACAO already has a host-side strength:
   `settings.shadowMultiplier = aoEnvFloat("ENGINE_AO_STRENGTH", 1.0f)`
   (`VulkanAOPass.cpp:258`), applied inside `ffxCacaoUpdateSettings`. GI-on
   AO attenuation is a small **runtime setter** on `VulkanAOPass` (see
   decision D5), not a composite uniform.
2. **Orphan shader inventory was incomplete** — `gi/spv/` also contains
   `gi_estimate.comp.spv.debug` (survey listed only initial/gather/blur/
   temporal). Full set as listed in Background.
3. **Shader build picks up new `.comp` automatically** — `scripts/build.sh`
   sources `scripts/shaders.sh` per module (`cd c-engine` → `shaders.sh`,
   `cd c-game` → `shaders.sh`). `shaders.sh` does
   `find ./data/pak_*/shaders -name "*.comp"` and compiles every source to
   `<dir>/spv/<name>.comp.spv.debug` (`.spv.release` in release) via `glslc`
   with `-I <pak>/shaders/includes`; `data.sh` then zips the pak into
   `build/c-game`. A **new `.comp` file has no existing spv, so it is
   always compiled** (the mtime/`doFullCompile` fast path only skips
   unchanged existing sources). No CMake change: `c-engine/CMakeLists.txt`
   uses `file(GLOB_RECURSE cSrc CONFIGURE_DEPENDS "*.cpp")`.
4. **`VulkanAOPass` temporal push constants** (`AoTemporalPushConstants`,
   `VulkanAOPass.cpp:328-342`) — 13 fields, all passed as one push-constant
   struct: `u32 aoIndex` (current CACAO output, sampled), `velocityIndex`,
   `depthIndex`, `prevIndex` (sampled), `outIndex` (**storage**), `width`,
   `height`; `float blendWeight` (default `ENGINE_AO_TWEIGHT` 0.92),
   `depthThreshold` (`ENGINE_AO_TDEPTH` 0.05), `clampSlack`
   (`ENGINE_AO_TCLAMP` 0.35), `clampFloor` (`ENGINE_AO_TFLOOR` 0.15),
   `devStart` (`ENGINE_AO_TDEV0` 0.12), `devEnd`
   (`ENGINE_AO_TDEV1` 0.50). The accumulator images `temporalA/B` are
   `R16G16B16A16_SFLOAT` (`.r` = AO, `.g` = inverse view depth; 0 = no
   history, cleared black on create), dispatched at CACAO's internal
   `cacaoWidth/cacaoHeight` with 8×8 workgroups. The GI temporal pass copies
   this struct shape (with a confidence channel added) and this env-knob
   family.
5. **`vulkanAOPassGetOutput()` NULL-sentinel contract** (`VulkanAOPass.h:34-37`,
   `.cpp:533-541`) — returns the current ping-pong `temporalOutput` when the
   temporal filter is enabled, the raw CACAO buffer when it is not, and
   **NULL** until the context exists (before the first enabled frame after
   swapchain creation) or while disabled. Consumers convert NULL to the
   absent-sentinel push-constant index: composite does
   `vulkanAOPassIsDisabled() ? 0xFFFFFFFFu : (vulkanAOPassGetOutput() ?
   (u32)output->sampledPoolIndex : 0xFFFFFFFFu)`
   (`VulkanCompositePass.cpp:99-103`) and the shader skips its work on
   `index == 0xFFFFFFFFu` (`composite.comp:21,115`). `VulkanGiPass` adopts
   the identical getter + sentinel contract.
6. **The "FSR reactive mask" is the engine's own shader, not the SDK
   autogen pass** — `docs/fsr3.1.md` has no dedicated reactive-mask section;
   it only lists the SDK's `ffx_fsr3upscaler_autogen_reactive_pass` /
   `ffx_fsr3upscaler_luma_instability_pass` among built shaders (plus the
   `rw_luma_history` `rgba8→rgba16f` patch, doc lines 181–184). The actual
   per-pixel reactive signal this engine feeds to FSR is
   `shaders/pass/fsr/reactive.comp` (pass `fsr_reactive`,
   `VulkanFsrPass.cpp:108-110`), which writes a render-res `R32F` mask
   exposed via `vulkanFsrPassGetReactiveMaskImage()` (the DOF pass
   max-blends its CoC mask into the same image). Its terms: (1) planar
   reflection, (2) specular/view-dependent, (3) **composite-difference
   fallback** (for `roughness >= 0.25`: relative luminance difference
   between opaque scene color and composite color, `smoothstep(0.10, 0.30)`
   × 0.25), (5) terrain grazing. This is what P4 must validate: GI changing
   ambient changes the opaque/composite colors, which can trip term 3 —
   see P4 and Risks. FSR's *internal* luma-instability handling (SDK) is
   the second, undocumentable layer; it is validated empirically.

## Design decisions

### D1 — Pass name, location, registration

New engine pass **`VulkanGiPass`** in
`c-engine/renderer/vulkan/pass/gi/VulkanGiPass.{h,cpp}` (System pattern,
multi-pipe like `VulkanAOPass`: `giEstimate` + `giTemporal` pipes).
Shaders under `c-engine/data/pak_0_engine/shaders/pass/gi/`:
`gi_estimate.comp`, `gi_temporal.comp`. Registered in `vulkanInit()`
(`Vulkan.cpp:293-323`) **between `addPass(&vulkanSsrPass)` (line 310) and
`addPass(&vulkanAOPass)` (line 311)** — after `oit_composite` so the
G-buffer it reads is final, before `ao`/`composite` so the temporal output
exists for later passes. No CMake/pipeline-script edits needed (fact check 3).

### D2 — Buffers, formats, resolutions

All keyed off `window.renderWidth/Height` (FSR-scaled internal), never
`window.width/Height` (survey §2.4).

| Buffer | Owner | Format | Size | Content |
| --- | --- | --- | --- | --- |
| `giCurrent` | pass-owned static image (AO-output pattern, not a per-frame `VulkanFrameResources` entry) | `R16G16B16A16_SFLOAT` | **half** internal (W/2 × H/2) | RGB = radiance estimate, A = confidence (edge/depth-delta fades). Single slot, overwritten per frame. |
| `giHistoryA` / `giHistoryB` | pass-owned static, ping-pong (TAA/AO pattern: recreated on `swapchainCreated`, cleared on creation) | `R16G16B16A16_SFLOAT` | full internal | RGB = filtered irradiance, G = inverse view depth (disocclusion test), A = confidence. Cleared 0 = "no history". |

VRAM at 2560×1440 internal: estimate (1280×720×8 B) ≈ 7.3 MB, each
full-res history slot (2560×1440×8 B) ≈ 29.5 MB, two slots ≈ 59 MB —
~66 MB total, comparable to a single G-buffer color slot and small next
to the existing G-buffer/velocity storage.
Compute-written, no blending, no renderpass fast-clear → not in the AMD DCC
trigger class (`docs/oit-amd-dcc.md`), but P4 still verifies on RADV.

Deviation from survey §4.1: the survey listed the buffers "all at
`renderWidth/Height`" while its own pass table says the estimate is
half-res. **Resolution: estimate at half internal, temporal history at full
internal** — same shape as CACAO's half-res block → full-res temporal output,
and the cheapest option that keeps the `scene.frag` sample at full res.

### D3 — Estimate shader (`gi_estimate.comp`)

Conventions copied from `ssr.comp` (header block, includes,
`local_size = 8`) — the only ray-marching compute shader left in the tree
after the original `ao.comp` was removed in the CACAO migration
(`plans/cacao-ao.md`; its design is documented in the superseded
`plans/ambient-occlusion.md`):

1. Dispatch over the half-res grid. Origin pixel = 2×2 block of the G-buffer;
   reconstruct world position via the unjittered-UV convention
   (`uv + jitterOffset` → NDC → `invViewProjectionNoJitter`), `N =
   OctDecode(Normals.rg)`, view direction `V` as in `ssr.comp`.
2. `depth == 0` → sky: write `giCurrent` = IBL-sky irradiance for N
   (same chain `scene.frag` uses), confidence 1.0.
3. **Hemisphere rays** (`RAY_COUNT = 4–8`, default 6): per-ray direction
   from a blue-noise LUT (128×128, per-frame rotation). The
   `sceneBuffer.blueNoiseIndex` slot exists (`globalset.shader:227`) but
   is currently unpopulated (`VulkanIbl.cpp:107`, marked unused), so the
   GI pass supplies its own LUT image (or falls back to deterministic
   hash directions). March in view space with doubling steps and **HiZ
   early-out**: the HiZ chain and `vulkanHiZGetMipSampledIndex(mip)` /
   `vulkanHiZGetMipCount()` are still in the tree
   (`VulkanHiZPass.cpp:408-412`), but no current shader uses HiZ early-out
   (`ssr.comp` carries a `hizIndex` push constant yet does a linear
   march) — the early-out math (mip from step-size × pixels-per-unit;
   `.r` = min/farthest, `.g` = max/closest test; off-screen steps
   terminate the ray) is documented in the superseded
   `plans/ambient-occlusion.md` for the removed `ao.comp`. Start offset
   along N, `startDist ≈ 0.005 + 0.01 × distToCam`; max distance
   `clamp(0.15 × distToCam, 1.0, 20.0)` — village-tuned starting defaults
   from that same removed pass, not from CACAO — with
   `1/(1+(t/maxDist)²)` range fade.
4. **Hit**: `L = hitAlbedo × (kD_sun × shadowlessDirect + IBLsky(hitN))` —
   i.e. albedo × the local *unoccluded* radiance (direct sun + sky); the
   CACAO AO of the hit pixel is *not* sampled (AO already encodes the
   occlusion the GI term replaces; see D5). **Miss**: IBL-sky for the ray
   direction. Accumulate over rays, normalize.
5. **Confidence** (`.a`): product of (a) depth-edge fade of the origin
   texel (4-neighbor depth delta vs distance — reuse `ssr.comp`'s
   `depthEdgeFade` idea) and (b) per-hit depth-delta confidence. Screen
   edges → 0 → consumer falls back to IBL.

### D4 — Temporal shader (`gi_temporal.comp`)

Modeled on `ao_temporal.comp` (push-constant struct shape and env-knob
family per fact check 4):

- Inputs: `giCurrent` (half-res), prev `giHistory`, `velocity`, `depth`.
  Output: current `giHistory` (full-res).
- `prevUv = uv − velocity / res` (taa.comp convention); resample prev
  history bilinearly. Reject (blend weight → 0) when: `prevUv` out of
  bounds, prev `.g == 0` (no data), or inverse-view-depth change exceeds
  `depthThreshold` (TAA's `depthToInv` convention).
- **Upsample** `giCurrent` with a 3×3 bilinear tap set (half → full).
- **3×3 clamp — direction matters** (the AO implementation note: clamping
  the *current* sample against a cleared history deadlocks): build the box
  from the current estimate's neighborhood at the reprojected position and
  clamp the **reprojected history** into it (`clampSlack` / `clampFloor`).
- Confidence-weighted, deviation-damped blend: `w =
  blendWeight × confidence × damp(deviation, devStart, devEnd)`;
  `out = mix(clampedPrev, current, 1 − w)`. Write `RGB = out`,
  `G = depthToInv(depth)` (0 for sky), `A = confidence`.
- **Luminance-change clamp** (FSR protection): after the blend, clamp the
  per-texel luminance delta vs the reprojected history to
  `maxLumaDelta` (env `ENGINE_GI_TLUMA`, default e.g. 0.15) — bounds the
  per-frame signal change FSR's luma-instability layer sees. This is the
  SSGI++ "stable signal before the upscaler" step; the AO precedent has no
  equivalent and it is specific to GI because the signal enters the *color*
  path before FSR.

### D5 — Consumer-side contract (sentinel + one-frame latency)

- `vulkanGiPassGetOutput()` returns the **previous frame's** temporal
  `giHistory` output (see D6) — NULL-sentinel contract identical to
  `vulkanAOPassGetOutput()` (fact check 5). Plus `vulkanGiPassSetDisabled/
  IsDisabled` and an `ENGINE_GI_DISABLED=1` env (AO pattern, read in
  `added()`).
- `scene.frag` (ambient block, lines 195–227) and `azgaar_props.frag`
  (lines 171–180) sample the GI texture from the scene buffer with the
  jittered UV and fetch:

  ```glsl
  if (sceneBuffer.giIndex != 0xFFFFFFFFu) {
      vec4 gi = texelFetch(giTex, ivec2(jitteredUv * giRes), 0);
      ambientDiffuse = mix(iblDiffuse, gi.rgb * shadowDarkFactor,
                           gi.a * giIntensity);
  }
  ```

  `giIntensity` global master (env `ENGINE_GI_INTENSITY`, default 1.0).
  IBL specular, `shadowDarkFactor`, and `Lo` are untouched.
- **AO attenuation:** GI output already contains diffuse occlusion; leaving
  CACAO at full strength double-darkens. Add
  `vulkanAOPassSetStrength(float)` to `VulkanAOPass` (runtime override of
  `settings.shadowMultiplier`, which today is *reassigned from*
  `ENGINE_AO_STRENGTH` every frame in `cacaoUpdate()`
  (`VulkanAOPass.cpp:258`) — the setter therefore needs an override member
  that takes precedence over the per-frame env read, not a one-shot
  assignment). When GI is enabled
  and not disabled, drive it to a reduced value (env `ENGINE_GI_AO_SCALE`,
  default 0.5). Fallback if the setter proves awkward: a new `aoStrength`
  uniform in `composite.comp` (the survey's original idea).
- `Renderer.cpp` settings wiring: `utils::settingsGetBool("giDisabled")`
  → `vulkanGiPassSetDisabled` (mirror the `aoDisabled` line), settings GUI
  toggle in `SettingsGraphicsGui.cpp` (AO-toggle pattern). GI ships
  **off-by-default** in settings until P4 validation passes, then flips.

### D6 — One-frame latency wiring

`scene.frag` (pass index 10, 0-based in the `vulkanInit()` list) runs
*before* `VulkanGiPass` (index 18 once inserted between `vulkanSsrPass`
and `vulkanAOPass`), so a frame's ambient uses **last frame's** filtered
history — the same cadence as TAA/AO histories; the temporal filter hides
the one-frame lag. Mechanism:

- The scene pass's shader bindings get the GI texture as a scene-buffer
  texture: a new `giIndex` field on the `SceneBuffer` struct
  (`shaders/includes/globalset.shader`) indexing the existing
  `textures[]` pool — see Open questions for the exact touch points.
- `VulkanGiPass` holds a `lastOutput` pointer (the `giHistory` image from
  the *previous* completed frame); the scene pass binds
  `lastOutput ? sampledPoolIndex : 0xFFFFFFFFu` each frame — first frame
  after startup/resize/disabling renders with IBL ambient (sentinel), from
  frame 2 on it switches. History is only ever sampled in
  `SHADER_READ_ONLY_OPTIMAL` (the temporal dispatch already transitions its
  output there at the end of `temporalDispatch`, `VulkanAOPass.cpp:444` —
  copy the transition order so no extra barrier pass is needed; the ping-
  pong write goes to the *other* slot, so the bound slot is never written
  while bound).
- Disable→enable edge: reset history (`temporalDestroyAccumulators`
  equivalent) exactly like `VulkanAOPass::update()` does on re-enable
  (`.cpp:465-470`).

### D7 — Debug surface

- `ENGINE_DEBUG_DUMP_IMAGES` tokens: `gi` (current temporal output) and
  `giEstimate` (raw per-frame half-res estimate — the original AO pass
  kept a separate raw per-frame `aoFrame` dump next to the temporal `ao`
  and it proved essential for ray debugging; that token was removed with
  XeGTAO in the CACAO migration, but the same split applies here). Add
  both to `vulkanDebugDumpFrameImages()` (`Vulkan.cpp:165-271`).
- Env knobs (family mirrors `ENGINE_AO_*`): `ENGINE_GI_DISABLED`,
  `ENGINE_GI_TEMPORAL=0` (debug view = raw estimate via
  `vulkanGiPassGetOutput()` falling back to `giCurrent`, exactly the AO
  getter's raw-buffer fallback), `ENGINE_GI_TWEIGHT` / `_TDEPTH` /
  `_TCLAMP` / `_TFLOOR` / `_TLUMA`, `ENGINE_GI_RAYS=<4-8>`,
  `_DIST_SCALE`, `_INTENSITY`, `_AO_SCALE`.
- `ENGINE_LOG_PASS_GPU=1` per-pass timing; `vulkanGiPass.gpuElapsed` in the
  debug GUI (AO/SSR pattern).

## Phases

Each phase is independently shippable and verifiable with the commands
listed in it. **Parked player/camera: do not move** (saved transforms in
`build/c-game/data/db/db.db`) — all screenshot A/Bs rely on the parked
vantage.

### Phase 1 — GI estimate + debug visibility

Goal: the estimate exists, is measurable, and is visible as a dump — no
consumer wired yet.

Deliverables:
- `pass/gi/VulkanGiPass.{h,cpp}`: skeleton System, `giEstimate` pipe,
  `giCurrent` image (half-res, D2), registration in `vulkanInit()`
  (D1), disabled/env handling, getter stubs, profile timing.
- `shaders/pass/gi/gi_estimate.comp` (D3).
- Debug tokens `giEstimate` (+ `gi` pointing at the same buffer while
  temporal is absent) in `vulkanDebugDumpFrameImages`.
- No scene/composite changes; the image is produced but unconsumed.

Verification:
- `./scripts/build.sh` compiles the new `.comp` (check
  `shaders/pass/gi/spv/gi_estimate.comp.spv.debug` appears and the pak
  rebuilt).
- `ENGINE_HIDE_GUI=1 ./scripts/run.sh play screenshot /tmp/gi0.jpg` +
  `ENGINE_DEBUG_DUMP_IMAGES=giEstimate,depth,albedo` — dump shows a
  plausible radiance field (sky ≈ IBL brightness; canyon floors darker;
  no black sky pixels).
- `ENGINE_LOG_PASS_GPU=1` — estimate alone within budget (cost table below).
- `ENGINE_GI_DISABLED=1` skips the dispatch entirely (context/images not
  even created), frame cost returns to baseline.

### Phase 2 — Temporal filter

Goal: the boiling estimate becomes a stable history.

Deliverables:
- `gi_temporal.comp` (D4) + `giTemporal` pipe in `VulkanGiPass`;
  ping-pong `giHistoryA/B` (full-res, cleared 0, `swapchainCreated`
  recreation, re-enable reset per D6).
- `vulkanGiPassGetOutput()` now returns the temporal output (raw estimate
  when `ENGINE_GI_TEMPORAL=0`); `gi` debug token.
- Push-constant struct + `ENGINE_GI_T*` env knobs (D7).

Verification:
- `ENGINE_DEBUG_DUMP_IMAGES=gi` multi-shot:
  `ENGINE_SCREENSHOT_COUNT=8 ENGINE_SCREENSHOT_DELAY_MS=15000`
  (`docs/screenshot.md`) — inter-frame mean diff of the `gi` dump must
  converge toward the no-GI pipeline baseline (the original AO temporal
  pass added ≈0.5/255 beyond baseline, `plans/ambient-occlusion.md`;
  target similar magnitude).
- Camera-motion look for ghost trails (3×3 clamp + depth rejection in
  place); tune `ENGINE_GI_TWEIGHT/_TDEPTH/_TCLAMP` as needed.
- `ENGINE_GI_TEMPORAL=0` vs `=1` A/B dumps show noise→convergence.

### Phase 3 — Ambient injection + AO attenuation

Goal: GI is visible in the final image.

Deliverables:
- `sceneBuffer` GI texture slot + jittered-UV fetch in `scene.frag`
  (ambient block 195–227) and `azgaar_props.frag` (171–180) per D5, with
  the `0xFFFFFFFF` sentinel.
- One-frame-latency binding (`lastOutput`) + transition order (D6).
- `vulkanAOPassSetStrength()` + GI-driven attenuation (D5).
- Settings: `giDisabled` in `Renderer.cpp` + `SettingsGraphicsGui.cpp`
  toggle (off by default); `ENGINE_GI_INTENSITY` master.

Verification:
- `ENGINE_HIDE_GUI=1 ./scripts/run.sh play screenshot /tmp/gi_on.jpg` vs
  `ENGINE_GI_DISABLED=1 ... /tmp/gi_off.jpg` (parked player) — ambient
  response: canyon/cave interiors darker and tinted by local albedo,
  meadow under canopies mottled, sky-facing areas **unchanged** (IBL
  fallback at confidence 1).
- Energy check: open-sky ambient brightness on/off within a few %
  (IBL-sky miss fallback guarantees equality on ray miss).
- No double darkening in crevices (AO attenuation working); A/B
  `ENGINE_GI_AO_SCALE=0.5` vs `1.0`.
- First-frame / re-enable frames render clean IBL (sentinel path).
- `ENGINE_LOG_PASS_GPU=1` — estimate + temporal total within budget.

### Phase 4 — Vegetation/props coverage + FSR reactive-mask validation

Goal: ship-level validation, then flip the setting default to on.

Deliverables:
- Confirm `azgaar_props.frag` GI sample works for grass/leaf cards
  (discard-gated: depth/normals exist only for surviving pixels, so the
  estimate sees exactly the visible silhouette — same property the AO disk
  review relied on, `plans/ambient-occlusion.md` implementation notes).
- Reactive-mask validation (see below).
- Docs: update `docs/fsr3.1.md` with a short "reactive mask" section
  documenting `reactive.comp`'s terms (fact check 6 — the doc currently
  has none), and `docs/global-illumination.md` status line (plan →
  implemented).

Verification:
- **Reactive mask** (fact check 6): with FSR on, dump
  `vulkanFsrPassGetReactiveMaskImage()` (add a `reactive` token to
  `vulkanDebugDumpFrameImages`) with GI on vs off, camera orbiting —
  expect **no new reactive regions** from GI; if term 3
  (composite-difference) trips on ambient changes, mitigate by lowering
  `ENGINE_GI_TLUMA` (slower luminance convergence) rather than editing
  `reactive.comp`. Cross-check final-frame stability with the multi-shot
  boiling test from P2 and a temporary `vulkanFsrPassSetReactiveMask(0)`
  hook (the setter exists, `VulkanFsrPass.cpp:657`, but no env/settings
  variable wires it today — there is no `ENGINE_FSR` toggle) as a
  diagnostic.
- Budget: `ENGINE_LOG_PASS_GPU=1` — estimate + temporal combined vs the
  re-baselined current `ao` cost (the 0.98 ms figure is the historical
  XeGTAO reference) and the 16.6 ms frame; document the numbers here.
- RADV run (Vulkan 1.3): no DCC-style flicker in GI-modified regions
  (compute-written buffers shouldn't trigger it, but confirm).
- AMD DCC / OIT spot-check on transparent objects (accepted: they get no
  GI — verify they at least don't *darken* against GI-lit neighbors).
- Flip settings default to on; A/B the main menu → world transition.

## Cost budget (GTX 1080 Ti class)

Reference (historical): `ao = 0.98 ms` (ray + temporal, full internal) on
a 16.6 ms 60 fps frame at 2880×1627 — measured with the *removed* XeGTAO
implementation (the "32 rays × ≤16 HiZ steps" descriptor belongs to that
`ao.comp`, not the current CACAO pass). Re-baseline the current `ao` cost
with `ENGINE_LOG_PASS_GPU=1` in P1 and use that number for the
comparisons below.

| Item | Estimate basis | Budget |
| --- | --- | --- |
| `gi_estimate` (half-res, 6 rays × ≤16 HiZ steps) | CACAO full-res ray dispatch ≈ 0.7 ms (historical AO total minus temporal); ×0.25 pixels (half-res) × ray-count ratio (6 vs CACAO's quality taps) ≈ 1/7 | **≤ 0.2 ms** |
| `gi_temporal` (full-res, 3×3 taps + 3×3 clamp) | `ao_temporal` portion of the 0.98 ms ≈ 0.3 ms | **≤ 0.3 ms** |
| **Total** | | **≤ 0.5 ms** hard target, **1.5 ms** fail line (revisit ray count / probe fallback per survey §3) |
| VRAM | half-res estimate 7.3 MB + 2× full-res history 29.5 MB (at 2560×1440 internal) | ~66 MB |

Measured with `ENGINE_LOG_PASS_GPU=1`; record actuals in the Status section
of this file at each phase landing (AO plan convention). If the estimate
blows the budget on 1080 Ti, the fallback is the survey's screen-space
dynamic-probe tier (same buffers, coarser estimate) — the orphan `gi_*`
gather-shader set was a prototype of exactly that.

## Risks / open questions

- **Screen-edge void** — rays leaving the screen miss and fall back to
  IBL sky: edge pixels blend to IBL via confidence (D3 step 5). Accepted
  and hidden by the fade; verify no bright rim at the frame border.
- **Light bleeding** across cliffs/thin geometry — HiZ max (closest) test
  is conservative, plus range fade; origin depth-edge fade suppresses
  silhouettes (same toolbox as AO).
- **Vegetation ghosting** — `azgaar_props` does not write velocity, so GI
  history over canopies relies on depth rejection alone (AO has the same
  exposure and lives with it). If ghost trails under canopies are visible
  in P4, lower `ENGINE_GI_TWEIGHT` or gate confidence down on
  alpha-mask material (`Material.b`).
- **One-frame latency** — GI cannot be A/B'd in single frames and pops
  in over one frame at spawn/teleport (history cleared). Accepted
  (TAA/AO have the same cadence); the settle-time screenshots
  (`ENGINE_SCREENSHOT_DELAY_MS`) must always be used.
- **Reactive-mask term 3** (fact check 6) — GI changes opaque/composite
  ambient, which the composite-difference fallback measures. Mitigations
  in order: `ENGINE_GI_TLUMA` convergence clamp (already in the design),
  then re-scoping which pixels the fallback covers. The SDK-internal
  luma-instability layer is not inspectable — validation is empirical
  (boiling test + final-frame stability).
- **OIT transparents get no GI** — accepted, documented artifact; they
  keep IBL ambient.
- **`sceneBuffer` GI index field** — not a binding-budget problem: the
  scene pass samples through the `textures[MAX_IMAGES]` (4096) /
  `samplers[MAX_SAMPLERS]` (11) uniform arrays
  (`globalset.shader:3-4,374-375`), so there is no per-pass binding
  ceiling to hit. The real P3 work is adding a `giIndex` uint to the
  `SceneBuffer` buffer-reference struct (`globalset.shader:247-264`;
  host-side mirror `VulkanSceneBuffer`,
  `VulkanResourceManager.cpp:107`), registering the GI image in the
  sampled pool, and filling the sentinel index each frame.
- **Orphan `gi_*` shaders** — reference only; do not attempt to reuse the
  debug SPIR-V (no sources, and the pak rebuild drops them anyway).
- **AMD DCC** — buffers are compute-written R16F (no blend/fast-clear
  class), but P4 verifies on RADV per `docs/oit-amd-dcc.md`.
