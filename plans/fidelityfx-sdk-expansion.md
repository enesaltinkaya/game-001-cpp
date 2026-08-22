# FidelityFX SDK Expansion — CAS, SPD, Lens, DOF, SSSR, Brixelizer GI

High-level roadmap for adopting six more AMD FidelityFX SDK components from
the already-vendored SDK (`cpp-thirdparty/fsr3.1`). The existing custom build
(`fsr3.1/build.sh`) compiles only the FSR3 Upscaler + CACAO; the SDK source
tree ships 21 components. Each phase below gets its own detailed plan before
implementation starts.

## Status

- [x] Phase 0 — Build infrastructure generalization (done 2026-08-22:
  `fsr3.1/build.sh` restructured into a `comp_<name>_` registry +
  `ENABLED_COMPONENTS` list; blob accessors & `-DFFX_<NAME>` defines wired
  automatically; `validate_component` fail-fast guard; single-archive name
  kept (`libffx_fsr3upscaler_vk.a`); no SDK patches were needed this phase;
  validated by symbol-identical archives on both platforms (nm diff vs
  pre-refactor = identical), full game build + `play log 5000` + screenshot
  (CACAO contact shadows + FSR output intact). Known behavior: re-running
  the build reorders entries in generated `*_permutations.h` wrappers —
  benign, documented in `docs/fsr3.1.md` "Re-run note".)
- [ ] Phase 1 — CAS (Contrast Adaptive Sharpening)
- [ ] Phase 2 — SPD (Single Pass Downsampler)
- [ ] Phase 3 — Lens (grain, vignette, chromatic aberration)
- [ ] Phase 4 — DOF (depth of field / bokeh)
- [ ] Phase 5 — SSSR (stochastic SSR, replaces custom SSR; needs Denoiser)
- [ ] Phase 6 — Brixelizer + Brixelizer GI (sparse voxel GI)
- Deferred: Parallel Sort (see "Deferred" section)

## Current state

- Pipeline order today (`Vulkan.cpp` `addPass` sequence):

```
Culling → Depth → Occlusion → HiZ → Shadow → ContactShadow → LightCulling
→ HeightmapTerrain → AzgaarProps → Scene → Skybox → Azgaar{River,Water,Weather}
→ OIT{Accum,Composite} → SSR → AO(CACAO) → Volumetric → Decal
→ Composite → TAA → FSR → Bloom → Final → DebugPhysics → RmlUI
```

- `libffx_fsr3upscaler_vk.a` (linux + win) contains fsr3upscaler + cacao,
  compiled via `build.sh` (wine `FidelityFX_SC.exe` shader permutations →
  blob headers → static archive). Unbuilt components are stubbed in
  `ffx_stubs.cpp`.
- Integration pattern is established (FSR pass, CACAO inside `VulkanAOPass`):
  `ffxGetScratchMemorySizeVK` + `ffxGetInterfaceVK`, static FFX context,
  `wrapImageResource`-style local helpers, dispatch in `update()`.

## Target pipeline (new components in **bold**)

```
... → Composite → TAA → **DOF** → FSR → Bloom → Final(**CAS**, **Lens**) → RmlUI
... → OIT{Accum,Composite} → **SSSR** → AO(CACAO) → ...
**SPD** = utility dispatch (not a scheduled pass; used inside IBL/bloom/texture paths)
**Brixelizer** = async/background scene voxelization + GI update, sampled in scene/composite shaders
```

Key ordering decisions:

- **DOF before FSR**: the upscaler then sharpens/upscales in-focus detail;
  DOF-displaced pixels should also feed the FSR **reactive mask** (the FSR
  pass already wires `reactiveMaskImage` — DOF writes to it) so bokeh isn't
  misinterpreted as detail change.
- **CAS after tonemap** (display-referred domain): our existing scalar CAS
  in `final.frag` documents why — the shaping term assumes [0,1]. FSR3's
  built-in RCAS (`APPLY_SHARPENING` permutations are already compiled) stays
  **off**; one CAS at the end of the chain replaces both.
- **Lens after Final**: operates on LDR output; must not affect UI (RmlUI
  after it).
- **SSSR replaces `vulkanSsrPass` in place** (between OIT composite and AO);
  it owns its own depth downsample + blue noise + denoise internally.

## Phase 0 — Build infrastructure generalization

Every later phase reuses this. Done once, here.

- Restructure `fsr3.1/build.sh` from "fsr3+cacao special case" to a
  per-component table: component name → source list, shader list, perm args
  (from each `gpu/<name>/CMakeCompile*Shaders.txt`), includes. Loop over all
  enabled components; keep the wine parallel shader pool, relative
  `-output` workaround, and per-object archive verification.
- Decide archive layout: keep the **single** `libffx_fsr3upscaler_vk.a`
  (blob dispatch `ffx_shader_blobs.cpp` already routes by effect id) and add
  components into it. Per-component archives are unnecessary.
- Update `ffx_stubs.cpp`: each newly built effect loses its stub.
- Expect an SDK **patch round per component** (commit to the fsr3.1 fork),
  based on the CACAO precedent:
  - `FFX_<NAME>_CONTEXT_SIZE` bumps on Linux (`wchar_t` = 4 bytes —
    CACAO pattern; verify each header's static assert at compile time).
  - Storage-image format qualifiers in `gpu/<name>/ffx_<name>_callbacks_glsl.h`
    may mismatch the host allocations (CACAO had 6 mismatches → validation
    warnings/UB).
  - Any MSVC-only code paths for MinGW, `std::wstring_convert`-style
    portability fixes in component sources.
- Keep both `build-linux/` and `build-win/` green each phase (release.sh
  depends on the win archive).
- Extend `docs/fsr3.1.md` with one section per component as they land.

**Validation**: clean rebuild of the archive, `./scripts/build.sh`,
`./scripts/run.sh play log 5000`, regression screenshots of FSR + CACAO
output unchanged.

## Phase 1 — CAS

_Effort: small. Replaces a hand-rolled approximation._

- SDK pieces: `ffx_cas.h` (context API), `ffx_cas.cpp`, 1 shader
  (`ffx_cas_sharpen_pass`, 4 variants). Perm args are trivial.
- Engine: new `VulkanCasPass` (or fold into `VulkanFinalPass` like CACAO
  folded into AO — prefer a small standalone pass writing to an intermediate
  LDR image): input = Final pass tonemapped output, dispatch at display res
  with `sharpness` from settings.
- Remove the scalar `casStrength` path from `final.frag` (keep `contrast` —
  CAS's contrast handling covers it; decide in the detailed plan).
- Settings GUI: reuse the existing sharpness slider, now driving
  `ffxCasContextDispatchGammaOnly=false` sharpen-only mode.
- **Watch**: CAS has no motion vectors — sharpening runs after TAA/FSR so it
  is temporally stable; verify no shimmer on foliage at default sharpness.

**Validation**: A/B screenshots (scalar CAS vs FFX CAS) on vegetation +
thin geometry; check with FSR on and off (native 1.0x too).

## Phase 2 — SPD

_Effort: small. Utility component — no visible feature, unlocks perf/quality._

- SDK pieces: `ffx_spd.h` (context or standalone dispatch),
  `ffx_spd.cpp`, 1 shader with 3 perm axes
  (`LINEAR_SAMPLE{0,1}` × `WAVE_INTEROP_LDS{0,1}` ×
  `DOWNSAMPLE_FILTER{0,1,2}`) × 4 variants.
- Engine: thin helper `vulkanSpdDownsample(src, outMips...)` wrapping
  context creation (formats are part of the context description — likely one
  SPD context per format/usage, recreated on resize).
- Concrete first users (in order of value):
  1. **IBL prefilter mip chain** — `VulkanIbl::renderPrefilter` currently
     does per-mip dispatches; SPD gives one dispatch + better filtering.
  2. **Texture `genMips`** — replace the `vkCmdBlitImage` chain in
     `VulkanImage` (bilinear-only today) with SPD compute; better quality,
     fewer barriers, load-time win.
  3. **Runtime scene-color mip pyramid** (half-res chain, `SAMPLED|STORAGE`)
     for rough-reflection fallback sampling and future consumers.
- **Not** a replacement for: HiZ (min-reduction, SPD averages), the Kawase
  bloom chain (its filter is not a plain mip chain). Evaluate bloom's
  initial downsample only.

**Validation**: RenderDoc capture comparing mip output vs old path (IBL
prefilter, a test texture); perf profile of load-time genMips.

## Phase 3 — Lens

_Effort: small. One fullscreen pass, big look-change-per-line-of-code._

- SDK pieces: `ffx_lens.h`, `ffx_lens.cpp`, 1 shader (`ffx_lens_pass`,
  4 variants).
- Engine: `VulkanLensPass` between Final (or CAS output) and RmlUI.
  Params: grain scale/intensity, vignette, chromatic aberration amount,
  dispersion if enabled by perm. All in a settings GUI group; default off
  (aesthetics are a user choice).
- **Watch**: keep it strictly post-tonemap LDR, after CAS (CA offsets would
  re-blur sharpened edges otherwise — or intentionally before CAS, decide in
  the detailed plan with screenshots).

**Validation**: screenshot grid over parameter range; confirm zero effect on
RmlUI UI elements.

## Phase 4 — DOF

_Effort: medium. New capability; first "pre-upscale" effect._

- SDK pieces: `ffx_dof.h`, `ffx_dof.cpp`, 5 shaders
  (downsample color/depth, dilate, blur, composite — 4 variants each).
  No cross-component dependencies.
- Engine: `VulkanDofPass` between TAA and FSR. Inputs: HDR color
  (post-composite), depth, camera near/far. Half-res internal chain,
  composite back to full-res HDR before FSR.
- Game side: focus model — start manual (settings GUI: focus distance,
  aperture, bokeh shape/scale), add auto-focus (screen-center depth readback
  or GPU min-copeland depth) later in the phase.
- Wire DOF into the FSR **reactive mask** (see ordering decisions above).
- **Watch**: depth of transparent/OIT objects is absent from the depth
  buffer (known engine-wide property) — first cut accepts it (transparents
  render sharp over blurred background), note as follow-up.

**Validation**: parked-camera screenshots near/far focus; sky vs foreground
separation; motion stability with FSR on; RenderDoc for the half-res chain.

## Phase 5 — SSSR (+ Denoiser dependency)

_Effort: medium. Replaces the custom SSR pass the user is unhappy with._

- SDK pieces: `ffx_sssr.h`, `ffx_sssr.cpp` (+ bundled blue-noise sampler
  cpp arrays), 5 shaders — **and** the **Denoiser** component
  (`ffx_denoiser.h/.cpp`, 8 shaders, reflection subset) because
  `ffxSssrContextCreate` internally creates a `FfxDenoiserContext`
  (`FFX_DENOISER_SHADOWS` mode is not needed — only reflections pass set).
- Engine: replace `VulkanSsrPass` internals; keep its slot and its interface
  to composite (reflection buffer + where composite samples it today).
  Inputs: HDR color, depth, **normals — SSSR wants linear normals, ours are
  oct-encoded `rg`** (CACAO hit the same issue and reconstructs from depth;
  SSSR's `normalUnpackMul/Add` can't decode oct either) → decode to a
  linear-normal image in a tiny pre-pass or feed `generateNormals`-style
  reconstruction per the detailed plan. Motion vectors + camera matrices for
  denoiser reprojection (both already exist for TAA/FSR/CameraUbo).
- Remove: current `ssr` shaders/pipes and the HiZ consumer that only served
  SSR (HiZ stays — culling uses it).
- Settings GUI: replace current SSR quality options with SSSR ones
  (ray length, roughness threshold, temporal stability).

**Validation**: side-by-side vs old SSR on water/azgaar river and wet props;
temporal stability while strafing; noise under flickering light; RenderDoc on
the denoiser chain.

## Phase 6 — Brixelizer + Brixelizer GI

_Effort: XL. Real workstream — multi-phase plan of its own. Do last._

- SDK pieces: `ffx_brixelizer{,_raw}.h`, `ffx_brixelizer.cpp` +
  `ffx_brixelizer_raw.cpp` (~biggest component), `ffx_brixelizergi.h/.cpp`
  (GI layer drives a **raw** Brixelizer context), shader sets for both.
- **Supersedes `plans/sdfgi-plan.md`**: Brixelizer GI provides the same
  class of result (dynamic diffuse GI, no baking) with maintained AMD
  shaders instead of a from-scratch Godot-inspired port. Keep the SDFGI doc
  as reference; mark it superseded when this phase starts.
- Integration shape:
  - Scene proxy streaming: ECS static meshes + `AzgaarProps` scatter objects
    → instance batches per cascade. **Terrain is the open question**: the
    heightmap surface is shader-enumerated, not a mesh — options are (a)
    CPU-side triangle batch generation from `AzgaarHeightmapSource` heights
    at coarse resolution per cascade (fits the determinism contract — pure
    function of source), or (b) ship terrain without GI initially. Decide in
    the detailed plan.
  - Updates: budgeted per-frame voxelization jobs (SDK job/instance API),
    cascade invalidation on prop streaming.
  - Sampling: scene shader samples GI cascade (diffuse irradiance) +
    existing IBL specular unaffected; CACAO stays for local occlusion.
  - Debug: cascade visualization overlay (SDK debug dispatch modes).
- Likely preceded by its own sub-phases: build+demos → proxy pipeline →
  scene integration → terrain decision → tuning.

**Validation**: GI on/off screenshots across a full day cycle; light-bleed
checks through walls; streaming pop-in around the parked camera; perf budget
ms report per cascade.

## Deferred — Parallel Sort

Not scheduled. Concrete triggers that would revive it:

- Moving off weighted-blended OIT to depth-sorted transparency
  (`docs/oit-amd-dcc.md` context).
- GPU particle depth sort (weather/particle systems).
- Any future GPU-driven list building that outgrows CPU sort.

## Explicitly out of scope

- **Frame interpolation / optical flow** — excluded by design (see
  `docs/fsr3.1.md`), invasive swapchain replacement.
- **FSR1/FSR2** — superseded by FSR3.
- **LPM** — we have the AgX-LUT tonemap pipeline; a downgrade.
- **Classifier** — RT-shadow classification; engine is fully raster.
- **Denoiser (shadows)** — only the reflection subset is built, as SSSR's
  dependency.
- **VRS, Breadcrumbs, Blur** — no current need (Breadcrumbs revisitable as a
  dev tool if device-loss forensics ever becomes a problem).

## Cross-cutting concerns (every phase)

- **Settings GUI**: each feature gets an on/off toggle + params group in
  `SettingsGraphicsGui`; debug dump/heatmap entries where relevant.
- **Validation recipe**: `./scripts/build.sh` → `./scripts/run.sh play log`
  (≥5000 ms) → screenshots (parked camera, `ENGINE_HIDE_GUI=1` variants) →
  RenderDoc when formats/barriers are in question → linux **and** win
  archive builds.
- **SDK fork commits**: every header/source patch lands in the fsr3.1 git
  fork, and gets a bullet list in `docs/fsr3.1.md` (CACAO precedent).
- **Perf discipline**: profile before/after each phase (pass profiles
  already exist: `vulkanCreateProfile("pass_%s")`).
