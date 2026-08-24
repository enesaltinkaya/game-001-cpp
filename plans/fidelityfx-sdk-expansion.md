# FidelityFX SDK Expansion — CAS, SPD, Lens, DOF, SSSR

High-level roadmap for adopting five more AMD FidelityFX SDK components from
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
- [x] Phase 1 — CAS (done 2026-08-22, **no new SDK component needed** —
      RCAS rides the existing FSR3 upscaler context; see phase notes)
- [x] Phase 2 — SPD (done 2026-08-22 — scope corrected during research,
      see phase notes: only the runtime-HDR-pyramid use is viable; first real
      consumer is SSSR in Phase 5)
- [x] Phase 3 — Lens (grain, vignette, chromatic aberration)
- [x] Phase 4 — DOF (depth of field / bokeh)
- [~] Phase 5 — SSSR (stochastic SSR, replaces custom SSR; needs Denoiser) —
      **reverted 2026-08-23**: SSSR + Denoiser were integrated and validated,
      then removed at the user's request (they preferred the old custom SSR,
      which was restored). `sssr`/`denoiser` dropped from
      `ENABLED_COMPONENTS` (registry blocks + SDK fork patches kept, so the
      phase can be re-attempted later); the old `ssr.comp` + HiZ refinement
      pass is back in place. See the Phase 5 section.
- [~] Phase 6 — Brixelizer + Brixelizer GI (sparse voxel GI): implemented
      2026-08-23 (phases 6.0–6.4), then **removed from the engine** on
      2026-08-24 (IBL was removed right after — GI is now intended as the
      sole ambient source). The `brixelizer`/`brixelizergi` registry blocks
      stay in `fsr3.1/build.sh` (components remain in the archive, unused).
      **Re-attempt planned: [`plans/brixelizer-gi.md`](brixelizer-gi.md)** —
      a restore-from-history (`be60942`) + post-IBL-removal adaptation,
      not a fresh integration.
- Deferred: Parallel Sort (see "Deferred" section)

## Current state

- Pipeline order today (`Vulkan.cpp` `addPass` sequence):

```
Culling → Depth → Occlusion → HiZ → Shadow → ContactShadow → LightCulling
→ HeightmapTerrain → AzgaarProps → Scene → Skybox → Azgaar{River,Water,Weather}
→ OIT{Accum,Composite} → SSR → AO(CACAO) → Volumetric → Decal
→ Composite → TAA → DOF → FSR → Bloom → Final → Lens → DebugPhysics → RmlUI
```

- `libffx_fsr3upscaler_vk.a` (linux + win) contains fsr3upscaler + cacao +
  spd + lens + dof + brixelizer + brixelizergi, compiled via `build.sh`
  (wine `FidelityFX_SC.exe` shader permutations → blob headers → static
  archive). The brixelizer/brixelizergi components are currently unused by
  the engine (the GI pass was removed 2026-08-24) but stay in the build so
  a re-attempt is a one-line re-enable. Unbuilt components
  are stubbed in `ffx_stubs.cpp`.
- Integration pattern is established (FSR pass, CACAO inside `VulkanAOPass`,
  SPD in `VulkanSpd`, Lens in `VulkanLensPass`, DOF in `VulkanDofPass`):
  `ffxGetScratchMemorySizeVK` + `ffxGetInterfaceVK`, static FFX context,
  `wrapImageResource`-style local helpers, dispatch in `update()`.

## Target pipeline (new components in **bold**)

```
... → Composite → TAA → **DOF** → FSR → Bloom → Final(**CAS**, **Lens**) → RmlUI
... → OIT{Accum,Composite} → **SSSR** → AO(CACAO) → ...
**SPD** = utility dispatch (not a scheduled pass; used inside IBL/bloom/texture paths)
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

_Done 2026-08-22 — design deviation, much cheaper than sketched below._

Outcome: AMD's RCAS kernel (the real CAS sharpener; compiled into the
FSR3 upscaler component since Phase 0 — the `APPLY_SHARPENING={0,1}`
permutations) runs inside the existing FSR3 upscaler dispatch.
**No `FfxCasContext`, no CAS entry in `ENABLED_COMPONENTS`, no new pass.**

- `VulkanFsrPass`: `dispatch.sharpness = rendererGetCasStrength()` (the
  `aaCasStrength` setting — slider + debug keys unchanged),
  `enableSharpening = strength > 0`. Removed the unused `fsrSharpness`
  static + `vulkanFsrPass{Set,Get}Sharpness` API and the old mutual-
  exclusion guard against the final pass's scalar CAS.
- **TAA/no-upscaler path (follow-up, same day):** the vendored RCAS kernel
  (`FsrRcasFilterF`, f32, `FSR_RCAS_DENOISE` on — the exact kernel + config
  the upscaler's RCAS pass uses) now also runs in `final.frag` via
  `shaders/includes/rcas.shader`, post-tonemap on the LDR result. Same
  `aaCasStrength` slider; `exp2(2*s - 2)` applies the identical FSR3
  strength remap so both placements feel the same. `VulkanFinalPass`
  pushes `rcasStrength = 0` while the upscaler is active (mutual
  exclusion — stacked kernels ring); the scalar imitation kernel stays
  deleted. Slider is enabled on all paths.
- Settings GUI: slider relabeled "Sharpening — RCAS" (always enabled),
  range extended 0–150% (player's choice; engine clamps `casStrength` to
  1.5). Above 100% (AMD reference max) applies only to the final-pass
  kernel — the FSR dispatch clamps its `sharpness` to [0,1] per SDK
  validation. To keep >1.0 multipliers safe, `rcas.shader` re-clamps the
  lobe to `-RCAS_LIMIT` _after_ the strength multiply (documented
  deviation: upstream only uses multipliers ≤ 1.0, where the resolve
  denominator can't flip; without the re-clamp, 150% crushed 52% of
  pixels to 0/255 — verified fixed: 0.375% baseline-level).
- Validated: FSR Native AA 40%/100% Laplacian variance 854/1754 (off=359);
  TAA path + RCAS100 visibly counters TAA blur (vision-checked, no
  ringing); FSR regression clean (single sharpening stage, 0 errors).
  Note: RCAS is more conservative than the old scalar kernel (its denoise
  stage suppresses sharpening on noise-like regions by design), so 100%
  here ≈ perceptually milder than the old scalar 100%.

_Original sketch (superseded, kept for context):_

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

_Done 2026-08-22 — with material scope corrections from investigating the
actual component._

**Corrections to the original sketch (kept below):**

- **IBL prefilter is NOT SPD territory** — `VulkanIbl::renderPrefilter`
  renders each mip as an independent GGX convolution of the environment
  (per-face roughness push constants), not a downsample chain. SPD's fixed
  MEAN/MIN/MAX filters cannot express it.
- **Load-time texture genMips is NOT SPD territory (on VK)** — the shipped
  shader's storage-image format qualifier is baked at SPIR-V compile time
  (`rgba32f` upstream); 8/16-bit UNORM/SRGB textures are format-class
  incompatible and per-format permutations would explode the blob system.
  The blit chain stays for texture loading.
- **What SPD is here: a runtime HDR mip-chain utility** for
  R16G16B16A16_SFLOAT images (fork patches the qualifier to `rgba16f`,
  matching every HDR render target). **First real consumer: SSSR (Phase 5)**
  — AMD's SSSR sample feeds SPD-generated color mips as the reflection
  input. Wiring a consumer earlier would be dead code.

**What landed:**

- Archive: `spd` added to `ENABLED_COMPONENTS` via a registry block (1
  shader, 3 perm axes × 4 variants). Two fork patches + one engine-side
  workaround, all documented in `docs/fsr3.1.md`: callbacks qualifier
  `rgba32f→rgba16f`; `FFX_SPD_CONTEXT_SIZE` Linux bump (the predicted
  `wchar_t` round — static assert caught 9300 < 17550 needed); backend
  global descriptor pool `poolSizeCount` 5→6 (upstream bug — dropped the
  STORAGE_BUFFER pool size; SPD's atomic counter is the first consumer).
- Engine: `renderer/vulkan/resources/VulkanSpd.{h,cpp}` — lazy MEAN/LOAD/LDS
  context, `vulkanSpdGenerateMips(cmd, img)` (validates format/mips/usage,
  GENERAL transition, `FFX_RESOURCE_USAGE_ARRAYVIEW` on the wrapped resource
  so the backend creates 2D_ARRAY views — the shader's `image2DArray` UAVs
  reject plain 2D views), teardown in `vulkanDestroyDelayed`.
- Validation: `ENGINE_SPD_SELFTEST=1` — 256×256 1×1-checker (0.25/0.75, exact
  f16) → SPD → readback mips 1..8, all must equal 0.5. **PASS**, zero
  validation errors, normal-path regression clean (SPD fully dormant when
  unused — context is lazy). The self-test also caught two test-side bugs
  along the way (2×2-block checker averaged to 0.25 at mip1; `onHeap`
  misuse in cleanup) — it stays in the tree as the regression gate for
  future SDK/fork updates, since nothing compile-time covers the format
  patch.

_Original sketch (superseded, kept for context):_

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

_Done (status was stale — the lens pass + component had already landed in
the engine and `ENABLED_COMPONENTS`; see `VulkanLensPass` and the lens
registry block in `fsr3.1/build.sh`)._

_Original sketch (kept for context):_

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

_Done 2026-08-22 — first pre-upscale effect; manual focus model._

**What landed:**

- Archive: `dof` added to `ENABLED_COMPONENTS` (5 shaders × 8 permutations
  × 4 variants). Two fork patches, documented in `docs/fsr3.1.md`:
  output UAV qualifier `rgba32f→rgba16f` (engine HDR targets are
  R16G16B16A16_SFLOAT; the internal UAVs keep `rgba32f`/`rg32f` because the
  VK backend forces `fp16Supported=false`, so the 32-bit permutations are
  selected and host allocations already match); `FFX_DOF_CONTEXT_SIZE`
  Linux bump 45674→88000 uint32s (the predicted `wchar_t` round —
  `FfxDofContext_Private` is 349096 B = 87274 uint32s; static assert
  catches regressions). No descriptor-pool or other backend changes needed
  (the SPD round's storage-buffer pool fix already covered DOF's atomic
  `rw_internal_globals` counter).
- Engine: `renderer/vulkan/pass/dof/VulkanDofPass.{h,cpp}` — lazy FFX
  context (recreated on resize / quality change), full-res
  R16G16B16A16_SFLOAT output, dispatch between TAA and FSR. Input color:
  TAA output when TAA is on, else the post-composite HDR color (the same
  image FSR would consume). Final + Bloom consume the DOF output in the
  non-upscaler path (upscaler path already consumed it).
- CoC model: thin-lens `ffxDofCalculateCoc{Scale,Bias}` against the
  camera's cglm RH_ZO reverse-Z projection (near/far swapped, so
  proj34 > 0, proj43 = −1); focus passed as negative view-space z;
  conversion in **half-res** pixels (the shader's CoC units), sensor width
  fixed at full-frame 36 mm; CoC limit factor 0.1 (≈ 1/10 screen height).
- FSR reactive mask: small engine compute shader
  (`shaders/pass/dof/coc_mask.comp`) max-blends a CoC-derived reactivity
  (smoothstep 2→8 half-res px, capped 0.5) into the FSR reactive mask,
  dispatched by the FSR pass right after its own reactive-mask generation
  (`vulkanDofPassApplyReactiveMask`) — bokeh pixels tell the upscaler not
  to accumulate detail the blur will destroy.
- Settings GUI: "Depth of Field" toggle + Focus Distance (0.5–100 m),
  Aperture (f/1.0–16), Focal Length (16–135 mm), Quality (1–8 rings)
  sliders; off by default. Env overrides for headless validation:
  `ENGINE_DOF_{ENABLED,FOCUS,FNUMBER,FOCAL,QUALITY}`.
- **Known limitation (accepted first cut, per plan):** transparent/OIT
  objects have no depth-buffer entry, so they render sharp over the
  blurred background. Follow-up: depth contribution for OIT or a
  transparency-aware composite.
- Auto-focus (screen-center / GPU min depth) deferred — manual model only.

**Validation:** parked-camera A/B screenshots (DOF off vs on: focus plane
at the player, foreground grass + far trees/water blurred); focus 2 m /
f/2.0 (whole scene soft) vs f/8 (near grass sharp, player + background
blurred) — focus distance and aperture both move the plane/DoF correctly;
FSR Native AA + DOF combined run clean (no validation errors, bokeh
upscaled correctly); DOF-off regression run clean (pass fully dormant —
context is lazy).

_Original sketch (superseded, kept for context):_

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

## Phase 5 — SSSR (+ Denoiser dependency) — REVERTED (2026-08-23)

_Reverted at the user's request: the SSSR result wasn't liked and the old
custom SSR (coarse linear march + HiZ binary refinement in `ssr.comp`) was
restored. `sssr` + `denoiser` were removed from `ENABLED_COMPONENTS` and the
archive rebuilt without them; the registry blocks in `fsr3.1/build.sh` and the
SDK fork patches (context-size bumps, callback qualifiers) were kept so this
phase can be re-attempted later without re-doing the fork work._

_What was implemented before the revert (kept for context):_

_Effort: medium. Replaces the custom SSR pass the user is unhappy with._

- SDK pieces: `ffx_sssr.h`, `ffx_sssr.cpp` (+ bundled blue-noise sampler
  cpp arrays), 5 shaders — **and** the **Denoiser** component
  (`ffx_denoiser.h/.cpp`, 8 shaders, reflection subset) because
  `ffxSssrContextCreate` internally creates a `FfxDenoiserContext`
  (`FFX_DENOISER_SHADOWS` mode is not needed — only reflections pass set).
- Engine: replaced `VulkanSsrPass` internals; kept its slot and its interface
  to composite (reflection buffer + where composite samples it today).
  Inputs: HDR color, depth, **normals — SSSR wants linear normals, ours are
  oct-encoded `rg`** (CACAO hit the same issue and reconstructs from depth;
  SSSR's `normalUnpackMul/Add` can't decode oct either) → decode to a
  linear-normal image in a tiny pre-pass (`normal_decode.comp`, oct→linear
  world normal, R16G16B16A16_SFLOAT). Motion vectors + camera matrices for
  denoiser reprojection (both already exist for TAA/FSR/CameraUbo).
- Remove: current `ssr` shaders/pipes and the HiZ consumer that only served
  SSR (HiZ stays — culling uses it). Done: old `ssr.comp` deleted.
- Settings GUI: kept the on/off toggle; SSSR params (roughness threshold,
  temporal stability, ray length, variance threshold) are env-var tunable
  (`ENGINE_SSSR_*`) for now — a GUI slider set is a follow-up.
- Roughness semantics: the G-buffer stores *perceptual* roughness (the
  engine's PBR squares it inside `DistributionGGX`), so the dispatch passes
  `isRoughnessPerceptual = true` — SSSR/denoiser square it themselves when
  reading the material channel. (First cut wrongly passed `false`, which
  read the perceptual value as pre-squared α and made everything glossier
  than authored; fixed and re-validated.)
- **Terrain reflection test**: `heightmap_terrain.frag` scales the base
  (grass/cliff) roughness by 0.1 (floor 0.05) before the snow/beach mixes,
  sets `outMaterial.g = 1` (metallic, so the composite blends the SSSR
  radiance at ~90% instead of a dielectric's 4% fresnel), and disables the
  micro-band normal tilt — all three lines marked `SSSR TEST`, easy to
  revert. Result: the terrain reads as a noisy mirror showing the sky +
  distant terrain; see the reflection-quality note below.
- **Character-reflection investigation (2026-08-23, RenderDoc)**: with the
  mirror terrain, the character's reflection does **not** resolve in the
  final image. Frame-capture analysis (frame ~480, parked camera):
  - Ray classification spawns 2.5 M rays; 21 k originate directly below the
    character's feet. ✓
  - The character is present in the SSSR depth hierarchy (mips 0–2) and in
    the main depth buffer. ✓
  - G-buffer normals + `SssrNormals` (decoded) are unit length and correct;
    material buffer reads roughness 0.078 / metallic 1.0 below the feet. ✓
  - Python re-simulation of the SDK ray math from the captured
    matrices/HiZ/normals: screen-space ray directions match rasterizer
    ground truth exactly (cos = 1.000); world-space geometry says ~27% of
    below-feet mirror rays pass within 0.4 m of the character's legs with
    100% clear line-of-sight (terrain does NOT occlude). ✓
  - The intersection output (radiance ping-pong, identified by its
    meter-scale ray-length alpha) contains only a faint noisy dark smudge
    under the feet; median world ray length is ~0.34 m — most rays
    terminate on nearby ground/grass long before the 1.2 m to the legs.
    The stochastic 1 ray/quad sampling at a grazing mirror angle plus the
    denoiser's spatial prefilter washes the smudge into the sky-dominated
    background. Sweeping `ENGINE_SSSR_SAMPLES=4`, `STABILITY=0` (temporal
    off), `THICKNESS=2`, `MAX_TRAVERSAL=256`, `MIN_OCCUPANCY=0` did not
    surface a figure.
  - Conclusion: the pipeline is correct end-to-end; a grazing-angle floor
    mirror of a 1.7 m figure 5 m away is simply at the edge of what
    1-spp screen-space tracing + this denoiser resolves at this angle.
    A better test vantage (camera low, character near a ledge/water) would
    show it. Follow-up options: raise `samplesPerQuad`, try the SDK's
    `PERFECT_REFLECTIONS` permutation for mirrors, or test on the azgaar
    water/river surfaces once they write depth.
- **Validation**: SSSR context created + dispatched at render res, no
  validation errors. The denoised reflection buffer was dumped directly
  (`ENGINE_SSSR_DUMP`): frame 0 is black (temporal history starts at zero),
  by frame 60 it shows correct scene reflections (bright ground reflecting
  sky, character + trees reflected). A/B screenshots (SSSR on/off) with the
  reduced-roughness terrain show clear sky-sheen + character reflection on
  the ground vs a matte floor when off. The default parked view is a poor
  water test because the **water is a pure color pass with no depth
  attachment**, so screen-space reflections (old SSR and SSSR alike) cannot
  reflect off it — same engine-wide property as the DOF note. A higher
  roughness threshold (`ENGINE_SSSR_ROUGHNESS=0.95`) makes the whole scene
  reflective and confirms the ray-march + denoiser chain is correct.
  Note: dump the reflection buffer late enough (`ENGINE_SSSR_DUMP_FRAME`
  ≥ ~500) — the first ~2 s of a `play` run are still loading (black scene),
  and `vulkanSaveImage`'s per-channel [min,max] normalization lets a few
  HDR sun glints crush an otherwise healthy buffer to near-black; inspect
  the logged float range and re-expose if needed.
- **Known limitation**: SSSR's environment fallback does not apply the
  engine's `envRotation` (IBL rotation); acceptable for a first cut.

_Original sketch (superseded, kept for context):_

- **Validation**: side-by-side vs old SSR on water/azgaar river and wet props;
temporal stability while strafing; noise under flickering light; RenderDoc on
the denoiser chain.

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
- **Classifier** — superseded 2026-09: the raster half of FFX Hybrid
  Shadows (classifier + shadow denoiser, no RT) is now a workstream —
  see `plans/ffx-hybrid-shadows-raster.md`.
- **Denoiser (shadows)** — the shadow-mode subset is pulled in by
  `plans/ffx-hybrid-shadows-raster.md`; the reflections subset remains
  SSSR's dependency (not built).
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
