# FFX Hybrid Shadows (raster-only) — Tile PCF + Temporal Shadow Denoiser

Adopt the **raster half** of AMD's FidelityFX Hybrid Shadows sample
(`cpp-thirdparty/fsr3.1/git/samples/hybridshadows`) without any ray tracing.
Concretely: the FFX **Classifier** (shadow mode) plus the FFX **Denoiser**
(shadow mode), sitting on top of our existing CSM (`VulkanShadowPass`) to
produce a temporally filtered full-resolution shadow mask that all lit passes
consume.

This revives two items that `plans/fidelityfx-sdk-expansion.md` listed under
"Explicitly out of scope" (_Classifier — "RT-shadow classification; engine is
fully raster"_ and _Denoiser (shadows)_), in a strictly raster form: the
sample's RT dispatch is never issued, and the classifier's RT-only outputs
(minT/maxT, work queue) are allocated but never consumed. No BVH/TLAS, no
`traceshadows.hlsl`, no `BuildRayTracingAccelerationStructure`.

## What we deliberately do NOT take

- **`RasterShadowRenderModule`** (the sample's cascaded shadow-map
  rasterizer, `framework/rendermodules/rastershadow/`): it is cauldron's
  standard CSM writer, tightly coupled to the cauldron scene/content system.
  Our `VulkanShadowPass` already does this better (texel-snapped stable
  light-view basis, quantized extents, 2 active cascades on a 2048² D32
  array, tuned raster + receiver biasing). Keep ours.
- **The RT path**: `traceshadows.hlsl` indirect dispatch, TLAS binding,
  blue-noise ray jitter. The classifier still _queues_ indeterminate tiles
  into a GPU work-queue buffer; we simply never dispatch it.

## Data flow (raster-only)

```
vulkanShadowPass (existing CSM — unchanged)
    2048² D32, 2 active cascades (SHADOW_ACTIVE_CASCADE_COUNT),
    per-cascade cascadeViewProj (zero-to-one light depth)
        │  per-cascade shadow-map layer SRVs
        ▼
[NEW] FFX Classifier (FFX_CLASSIFIER_SHADOW | FFX_CLASSIFIER_CLASSIFY_BY_CASCADES)
    inputs : depth (D32, depth pre-pass), world normal (new attachment),
             shadowMaps[0..1], lightDir, sunSizeLightSpace, blockerOffset,
             cascadeScale/Offset (derived from cascadeViewProj), viewToWorld
    work   : per 8×4-tile pixel — cascade select, 24-tap Poisson-disk PCF
             (imageLoad, sun-size disc, blocker offset), lane verdict
             "definitely lit / definitely shadow / indeterminate"
    output : rwt2d_rayHitResults — 32-bit lit-mask, one texel per 8×4 tile
             (rsb_tiles / rwb_tileCount work queue: allocated, never consumed)
        ▼
[NEW] FFX Denoiser (FFX_DENOISER_SHADOWS)
    inputs : tile lit-mask, depth, velocity (px, × 1/renderSize → UV),
             world normal, shadow-mask UAV, camera matrices
             (invProjection, reprojectionMatrix, invViewProjection, eye, frameIndex)
    work   : prepare (tile mask → per-pixel 0/1) → static/dynamic tile
             classification (velocity + depth + normal z-alignment) →
             3 normal/depth-guided soft-shadow filter passes (16×16 LDS,
             half precision) with ping-pong temporal history
    output : full-res RGBA8 shadow mask (.r = lit, 1 = fully lit)
        ▼
[REWIRE] lit fragment shaders: sampleShadowFull() samples the mask at
gl_FragCoord/viewport (contact-shadow precedent) instead of the 9-tap 3×3
hardware-bilinear PCF + texel-scaled normal bias. Map path stays compiled
behind a UBO index check (fallback + debug).
```

Pass placement in `Vulkan.cpp` `addPass` order: **after** `vulkanShadowPass`
(line 201 — CSM layers must be in `SHADER_READ_ONLY`) and **before**
`vulkanScenePass` (line 207 — first mask consumer). The depth pre-pass
(line 198) already produced depth + velocity (+ new world normal) earlier in
the frame, so no other ordering constraints.

## Why (benefit over the status quo)

- **Per-fragment cost**: today every lit fragment shader (`scene.frag`,
  `heightmap_terrain.frag`, `azgaar_props.frag`, `oit_accumulate.frag`,
  `triangle.frag`) pays a 9-tap 3×3 comparison PCF + texel-scaled normal
  bias + cascade-blend second lookup, _per cascade-blend region a second
  full PCF_. The FFX chain moves shadow sampling out of the (expensive) lit
  fragment shaders into 5 relatively cheap compute dispatches, and the
  fragment cost becomes one mask texture fetch.
- **Quality**: the denoiser's temporal history yields far more effective
  samples than the classifier's 24 taps — a real soft-sun angle becomes
  practical (we're hard-coded to `lightSize = 0`, PCSS was removed), or we
  can drop the shadow map 2048 → 1024 and hold far-cascade quality.
- **Direction of travel**: the engine already leans temporal (TAA/FSR,
  CACAO + custom temporal AO); the denoiser consumes the same motion-vector /
  depth / camera-matrix inputs those systems use.

## SDK work (`cpp-thirdparty/fsr3.1`)

### `denoiser` — re-enable existing registry block

The `comp_denoiser_` block already exists in `build.sh` (kept from the
Phase-5 SSSR work; it compiles all 7 denoiser shaders including the 5
shadow passes, perm axis `FFX_DENOISER_OPTION_INVERTED_DEPTH={0,1}` × 4
variants). The Linux `FFX_DENOISER_CONTEXT_SIZE` bump (73098 → 140000
uint32s) and the `-Wc++11-narrowing` `static_cast<uint32_t>` patches are
already in the fork. Re-add `denoiser` to `ENABLED_COMPONENTS`.

Engine selects the `INVERTED_DEPTH=0` permutation (our depth is zero-to-one,
D32 near=0/far=1, `clearDepth = 1.0` — matches the classifier/denoiser
non-inverted convention, unlike the DX12 sample which used reverse-Z).

### `classifier` — new registry block

- `comp_classifier_DEFINE=FFX_CLASSIFIER`
- `SOURCES`: `src/components/classifier/ffx_classifier.cpp` (the blob
  accessor `src/backends/shared/blob_accessors/ffx_classifier_shaderblobs.cpp`
  is wired automatically; it routes by effect id and selects the
  `CLASSIFIER_MODE` permutation from the context flag).
- `BASE_ARGS`: `-compiler=glslang -e CS --target-env vulkan1.2 -S comp -Os
-DFFX_GLSL=1 -reflection -DFFX_GPU=1` (the CMake
  `CMakeCompileClassifierShaders.txt` BASE args are SC-tool-agnostic
  (`-reflection -deps=gcc -DFFX_GPU=1`); mirror the denoiser block's
  glslang args).
- `PERM_ARGS`: `-DFFX_CLASSIFIER_OPTION_INVERTED_DEPTH={0,1}`,
  `-DFFX_CLASSIFIER_OPTION_CLASSIFIER_MODE={0,1}` (2 × 2 = 4 perms/shader;
  2 shaders × 4 variants = 32 permutation headers — small pool).
- `SHADERS`: `ffx_classifier_shadows_pass`, `ffx_classifier_reflections_pass`
  (the reflections pass compiles too — the accessor includes all its shaders
  unconditionally; we only ever dispatch the shadows pass).
- `INCLUDES`: `-I"$GPU_DIR" -I"$GPU_DIR/classifier"`.

Engine selects the `(INVERTED_DEPTH=0, CLASSIFIER_MODE=1)` permutation
(classify-by-cascades) via the context flags.

### `ffx_stubs.cpp`

No classifier/denoiser stubs exist (it only stubs breadcrumbs +
frame-interpolation) — nothing to remove.

### Expected patch round (document in `docs/fsr3.1.md` when it lands)

- **`FFX_CLASSIFIER_CONTEXT_SIZE`** (18500 uint32s): verify against the Linux
  `FfxClassifierContext_Private` (its `ResourceBinding` tables carry
  `wchar_t name[64]`, 4 bytes on Linux — the CACAO/SPD/DOF/denoiser
  precedent). The SDK's `FFX_STATIC_ASSERT` in `ffxClassifierContextCreate`
  reports the exact needed size at build time; apply the
  `#if defined(_WIN32)` platform-conditional bump if it fires.
- **GLSL callback UAV/SRV format qualifiers** for classifier + denoiser
  shadow passes (CACAO had 6 mismatches → validation warnings/UB). Audit
  `gpu/classifier/ffx_classifier_shadows_callbacks_glsl.h` and
  `gpu/denoiser/ffx_denoiser_shadows_callbacks_glsl.h` internal resource
  declarations against the host allocations in `ffx_classifier.cpp` /
  `ffx_denoiser.cpp`; patch qualifiers to match the host (lean formats,
  DX12 parity) rather than fattening host allocations.
- Keep both archives (`build-linux/` + `build-win/`) green each step
  (`release.sh` depends on the win archive).

## Engine work

### New pass: `renderer/vulkan/pass/shadow_denoise/VulkanShadowDenoisePass.{h,cpp}`

Standalone system (AO-pass template, `plans/fidelityfx-sdk-expansion.md`
integration pattern), registered in `Vulkan.cpp` between `vulkanShadowPass`
and `vulkanHeightmapTerrainPass`.

- **FFX contexts**: own `FfxInterface` + scratch (`ffxGetScratchMemorySizeVK
(phys, 2)`), a `FfxClassifierContext`
  (`FFX_CLASSIFIER_SHADOW | FFX_CLASSIFIER_CLASSIFY_BY_CASCADES`) and a
  `FfxDenoiserContext` (`FFX_DENOISER_SHADOWS`). Recreate both on
  `swapchainCreated` (they own render-resolution GPU resources internally).
  Dormant (no contexts) while the feature is off.
- **Scratch resources** (recreated on resize):
  - `rayHit` — tile-res R32_UINT UAV texture (8×4 tiles) — the lit mask /
    denoiser input.
  - `workQueue` — buffer, `4 × uint32` per tile (UAV; `AllowIndirect`
    not needed since we never do the RT indirect dispatch).
  - `workQueueCount` — 3 × uint32 buffer (initialized `{0, 1, 1}` via
    `vkCmdFillBuffer`/write — the sample uses `WriteBufferImmediate`; we
    write from a small constant buffer or a 1-thread dispatch).
  - `shadowMask` — full-res RGBA8 (SAMPLED | STORAGE) — denoiser output;
    added to the sampled image pool (bindless) like the contact-shadow
    texture.
- **Dispatch parameters** (per frame):
  - Classifier: `lightDir` (toward-scene sun, `vulkanIblGetExtractedSun`
    negated, as in `VulkanShadowPass`); `viewToWorld` =
    `camera->cameraUbo.invViewProjectionNoJitter`; `cascadeCount = 2`,
    `cascadeSize = 2048`; `bRejectLitPixels = true` (required — otherwise
    every lit pixel is "active" and the raster mask degenerates to all
    shadow); `bUseCascadesForRayT = false` (the RT minT/maxT path is dead,
    so `lightView` / `inverseLightView` may carry cascade-0's basis as
    dummies); `sunSizeLightSpace` + `blockerOffset` from settings/env (see
    below).
  - `cascadeScale[i]` / `cascadeOffset[i]`: the classifier computes
    `shadowCoord = lightView * world * scale + offset` and expects [0,1]²
    UV + zero-to-one depth. Our per-cascade world→shadow-UV transform is
    affine: `uv = 0.5·(cascadeViewProj[i]·world).xy + 0.5·(1, -1)` (Y flip
    matching `shadow.shader`'s remap), `depth = (cascadeViewProj[i]·world).z`.
    So `cascadeScale[i]` = ½ × the world→NDC xy/z rows of
    `cascadeViewProj[i]`, `cascadeOffset[i]` = the matching constants
    (0.5 / 0). Work out the exact row extraction during Phase 2 and verify
    with a UV-dump debug (the `SHADOW_DEBUG` stages in `shadow.shader` are
    the reference for correct UV behavior).
  - Denoiser: `hitMaskResults` = `rayHit`; `depth` = frame D32 (SRV-able —
    CACAO already feeds D32 into FFX; the sample's R32 copy pass is a DX12
    habit we skip); `velocity` = frame velocity (R16G16, **pixels**) with
    `motionVectorScale = (1/renderWidth, 1/renderHeight)` — the shadow
    tile-classification shader computes `previous_uv = uv + velocity·Scale`
    in UV space; `normal` = new world-normal image (unpack mul 1 / add 0);
    `eye` = camera position; `projectionInverse` / `reprojectionMatrix`
    (= `projection × prevView × invViewProjection`, cauldron formula) /
    `viewProjectionInverse` from `cameraUbo` (no-jitter variants — see
    jitter open item); `depthSimilaritySigma = 1.0`; `frameIndex`
    (monotonic, wraps 32-bit safely).
- **Settings / env** (DOF pattern):
  - `ENGINE_HYBRID_SHADOWS=0/1` master toggle, **default off** until Phase 4
    validation; off ⇒ pass dormant, `shadowMaskImageIndex = 0`, fragment
    shaders fall back to the PCF path.
  - `ENGINE_HYBRID_SHADOWS_SUN` (solid angle, degrees; start ~0.5–1° —
    today's hard look is 0°), `ENGINE_HYBRID_SHADOWS_BLOCKER`
    (start from our receiver bias 0.00015 in light-space units).
  - `ENGINE_HYBRID_SHADOWS_DUMP=<frame>` → `vulkanSaveImage` of the mask to
    `/tmp/hybrid_shadow_mask.jpg` (`ENGINE_DUMP_BLOOM` precedent).
  - Settings GUI (Graphics group): on/off + sun-angle slider.

### Depth pre-pass: world-normal attachment

The classifier's backfacing test (`dot(normal, -lightDir) > 0`) and the
denoiser's reprojection (`z_alignment = 1 - dot(viewDir, worldNormal)`)
need a **3-component world-space normal**; our pre-pass today writes only
view-space XY (R16G16 SNORM). Add a full-res R16G16B16_SFLOAT
`worldNormal` frame resource + a third color attachment:

- VS already has the transform buffer (used for the previous-frame clip
  velocity); world normal = `transpose(inverse(mat3(model))) × normal`
  (uniform-scale assumption — the same one the sample's raster module
  documents).
- The foliage override in `scene_depth.frag` (alpha-masked materials write
  view-up as their normal) must write world +Y for the same materials.
- **Scope check in Phase 1**: every pre-pass pipe rendering into the frame
  depth writes the attachment (depth / depth-doublesided / water pipes,
  `heightmap_terrain_depth`, `azgaar_props_depth`, `triangle_depth`, …) —
  a 1-line `out vec3` + store each; unrendered regions read back
  garbage-safe only if the image is cleared to 0 (clear the attachment —
  normal (0,0,0) reads as "facing light" = lit, which is the safe
  default for sky).

### Fragment-shader rewire (Phase 3)

- `ShadowUbo` / `ShadowData`: repurpose `pad0` →
  `shadowMaskImageIndex` (bindless sampled index of the mask; 0 = off) in
  both `LightComponent.h` and `globalset.shader` (GLSL struct mirrors the
  C one, pads included).
- `shadow.shader`: add `sampleShadowMask()` — `gl_FragCoord.xy / viewport`
  fetched from `textures[nonuniformEXT(sd.shadowMaskImageIndex)]` with
  `SAMPLER_CLAMP_LINEAR` (exactly the `sampleContactShadow` pattern).
- `sampleShadowFull()`: early-return `vec4(vec3(mask), mask)` when
  `shadowMaskImageIndex != 0u`. All five consumers (`scene.frag`,
  `heightmap_terrain.frag`, `azgaar_props.frag`, `oit_accumulate.frag`,
  `triangle.frag`) call `sampleShadowFull` already — no consumer-side
  changes. Contact shadows (`sampleContactShadow`) are multiplied separately
  and keep working.
- The map-sampling path (PCF, cascade blend, `SHADOW_DEBUG` stages) stays
  compiled — it is the fallback when the feature is off and the reference
  for debug comparison. Add a mask debug view (tile mask expanded to a
  per-pixel heatmap, a-la the sample's `debugtiles.hlsl`) for
  `SHADOW_DEBUG` parity.

## Phases

- [x] **Phase 0 — SDK build**: classifier registry block; re-enable
      `denoiser`; patch round (context size, qualifiers); rebuild both
      archives; `./scripts/build.sh` + `play log 5000` smoke (no engine
      consumer yet — visually a no-op).
- [x] **Phase 1 — world normal**: `worldNormal` frame resource + pre-pass
      attachment across all depth variants; dump-verify against
      `frameResources.normals` (oct-decoded) for sanity.
- [x] **Phase 2 — `VulkanShadowDenoisePass`**: FFX contexts + scratch
      resources, classifier + denoiser dispatches, env toggle +
      `ENGINE_HS_DUMP` (per-frame `hs_rayhit_*.jpg` + `hs_mask_*.jpg`);
      verified the dumped mask shows coherent soft sun-shadow patterns.
- [x] **Phase 3 — consume the mask**: `shadowMaskImageIndex` in `ShadowUbo`
      (repurposed `pad0`, mirrored in `globalset.shader`) +
      `sampleShadowMask()` + `sampleShadowFull()` early-return in
      `shadow.shader` + `vulkanResourceSetShadowMaskImageIndex` published by
      the pass; settings `hybridShadowsEnabled` / `hybridShadowsSun` +
      Graphics GUI toggle + sun-size slider; A/B screenshots at the parked
      camera confirm PCF vs denoised-mask look.
- [ ] **Phase 4 — tuning + validation**: sun-size / blocker / sigma
      sweeps; FSR on/off (jitter interaction); fast camera moves (ghosting
      check at cascade splits + disocclusion); perf profile before/after
      (the pass profile already exists: `vulkanCreateProfile`); optional
      1024-cascade experiment; full regression (contact shadow, OIT,
      terrain, weather).
- [ ] **Phase 5 — docs + release**: `docs/fsr3.1.md` section for
      classifier/denoiser (registry block, patches, shader lists);
      update `plans/fidelityfx-sdk-expansion.md` out-of-scope bullets;
      win + linux release builds.

## Implementation notes (as built)

- **Depth-permutation bug (found in debug session, fixed)**: the classifier
  was created with `FFX_CLASSIFIER_ENABLE_DEPTH_INVERTED`. That flag couples
  two conventions into one permutation: the scene-depth empty test *and* the
  shadow-map depth-compare branch. Our engine mixes conventions — the scene
  depth is reversed-Z (near=1/far=0, **cleared to 0.0**) while the CSM shadow
  maps are zero-to-one (near=0/far=1, **cleared to 1.0**) — so neither pure
  permutation is exact, but the non-inverted one is the correct one because
  the shadow-map compare is the core of the verdict: with INVERTED on, the
  verdicts invert ("definitely lit" requires max(tap depths) <= z+bias, which
  a single empty tap (1.0) breaks → *no lane is ever lit* → all-shadow mask,
  all-black denoiser output — the "not working" symptom). Fixed: classifier
  runs `INVERTED_DEPTH=0` (its `depth < 1.0` empty test misclassifies our
  empty 0.0 pixels as active, but the cleared zero world normal fails the
  backfacing test and the denoiser's prepare pass excludes depth-0 pixels,
  so it is harmless); denoiser keeps `FFX_DENOISER_ENABLE_DEPTH_INVERTED`
  (scene-depth only: its "closest velocity = max depth" pick matches
  reversed-Z). Verified: tile rayHit + denoised mask dumps are geometrically
  coherent, and a non-inverted GLSL replica of the classifier verdicts
  (TEMP `hs_debug_classify`, `ENGINE_HS_DEBUG=1`) matches the scene. The plan's
  original "INVERTED_DEPTH=0" choice was the right outcome for the wrong
  reason (it assumed zero-to-one scene depth; the scene depth is actually
  reversed-Z).
- **Sun-disc radius bug (found when the tree shadow read all-dark, fixed)**:
  the FFX classifier computes the sun-disc PCF radius as
  `radius = sunSizeLightSpace * lightViewSpacePos.z` — i.e. the disc at the
  receiver's distance from the *light origin* (z=0 of the light view). The
  FFX sample's light view carries a translation that places the light at a
  finite distance, so that z is a meaningful sun-receiver distance. Our CSM
  light view (`cascadeLightView`) is a **pure rotation (zero translation)**,
  so `lightViewSpacePos.z = dot(lightDir, worldPos)` is the receiver's
  absolute offset from the **world origin** along the sun axis (≈ −2025 m in
  this world). That made the disc radius ≈ 18 000 texels — ~9× the 2048 map —
  so every Poisson tap fell out of bounds, `maxD`/`minD` stayed at their
  initial values, and *no lane was ever "definitely lit"* → all-shadow tile
  mask → dark scene (the "tree shadow not working" symptom). Fixed by giving
  the light view a z-translation so the reference receiver (the camera) sits
  at `z = HS_SUN_REF_DISTANCE` (1.0), and subtracting `tz * cascadeScale.z`
  from each cascade's `offset.z` so the receiver depth (and the XY UVs, which
  are unaffected because the translation has no x/y component) is exactly what
  the CSM writes. Verified: the expanded rayHit + denoised mask now show the
  correct dappled tree shadow, matching the hybrid-off PCF.
- **Matching the hybrid-off (PCF) look** (Phase 4 goal): the raster-only path
  has two differences from the hardware-bilinear PCF — (1) no texel-scaled
  normal bias, so foliage canopies (tree leaves) self-shadow and read darker;
  (2) the denoiser's 16×16 normal/depth-guided filter blurs the fine
  dapples. Tuned to compensate: `sunAngleDeg` default **0.1°** (a ~1-texel
  sun disc, matching the PCF's sharpness; the slider's 0% end) and
  `blockerOffset` default **0.001** (a few × the CSM receiver bias 0.00015,
  compensating for the missing normal bias so the canopy doesn't read as
  solid shadow). The residual coarseness/darkness of the penumbra is the
  documented raster-only boundary bias, not a defect.
- **Env knobs** (final names, per `VulkanShadowDenoisePass.h`):
  `ENGINE_HYBRID_SHADOWS=1` master toggle (default off); `ENGINE_HS_SUN_ANGLE`
  (deg, default 0.1 — a ~1-texel disc matching the hybrid-off PCF sharpness;
  raise for softer penumbras); `ENGINE_HS_BLOCKER_OFFSET` (default 0.001,
  a few × the CSM receiver bias 0.00015 to compensate for the classifier's
  missing normal bias); `ENGINE_HS_DEPTH_SIGMA`; `ENGINE_HS_DUMP=<dir>`
  (per-frame `hs_lit_*.jpg` + `hs_mask_*.jpg` in the dir). The plan's
  `ENGINE_HYBRID_SHADOWS_SUN` / `_BLOCKER` / `_DUMP` names were superseded
  by these.
- **Dump readback ordering bug (found during A/B validation, fixed)**: `hsReadbackPixels`
  memcpy'd the mapped readback buffer *before* `vulkanTransientEnd` (submit +
  fence-wait), i.e. before the GPU `vkCmdCopyImageToBuffer` had even been
  submitted — every `hs_lit_*.jpg` / `hs_mask_*.jpg` dump was uninitialised
  host memory (all-zero → the tile view read all-white "all lit" and the mask
  read all-black). The memcpy now runs after the fence wait; dumps verified
  to show real tile/mask patterns.
- **Gap-filling validation (parked tree, A/B screenshots + frame-500 dumps)**:
  the raster-only path fills the dappled gaps between tree-branch shadows even
  at the 0.1° floor (slider 0%), and dramatically more at the 0.997° the GUI
  had persisted (`hybridShadowsSun`). Root causes, in order of impact:
  (1) the classifier's "definitely lit" verdict needs *all 24* Poisson taps
  clear inside the sun disc — dappled gaps narrower than the disc (≈3–4
  texel radius at 0.1°, ≈10+ texel at 0.997° in the near cascade, growing
  with receiver depth) classify indeterminate; the hybrid-off 3×3 tent PCF
  gives partial light to any 1-texel gap, so it stays dappled;
  (2) raster-only boundary bias: indeterminate tiles are never RT-resolved,
  so the denoiser starts them shadow and the temporal accumulator locks them
  there;
  (3) the denoiser's three EAW passes (steps 1/2/4) + contrast remap smear the
  residual lit specks and widen the penumbra ("puffed up");
  (4) no texel-scaled normal bias in the classifier (fixed 0.001 blocker
  offset only) so canopies self-shadow. Raising `ENGINE_HS_BLOCKER_OFFSET`
  to 0.01 does NOT reopen the gaps at 0.997° (the disc is simply too wide);
  the sun angle is the dominant lever. The 0.1° look is still closer to PCF
  than 0.997°, but the dapple filling is structural to the raster-only
  design (all-taps-lit test + indeterminate shadow bias + blur), not just the
  sun size.
- **Sun-size floor**: the classifier's Poisson-disk PCF degenerates when the
  sun disc (in shadow-map texels) exceeds the cascade extent — most taps
  fall out of range, `maxD` stays 0 and every pixel classifies
  "definitely lit" (all-red mask). The settings slider maps 0–100% to
  0.1°–4.0° and `vulkanShadowDenoisePassSetSunAngle` clamps to
  [0.1°, 10°]; an initially large default (45°) produced the degenerate
  mask and was corrected to 1°.
- **FFX VK backend quirks worked around in the pass**:
  (a) single-layer cascade D32 images are copied from the CSM 2D-array
  layers per frame (`vkCmdCopyImage` + manual per-layer
  `VkImageMemoryBarrier2`, since `vulkanTransition` ignores `baseLayer`);
  (b) the denoiser's internal history copy rejects D32→R32, so a small
  compute pass (`hs_depth_copy.comp`) copies the frame D32 into an R32
  `depthCopyImage` fed to the denoiser; (c) a 1×1 dummy D32 image fills
  the two inactive `shadowMaps[]` slots (FFX requires non-null resources).
- **Raster-only fallback look** (the documented risk, confirmed by A/B):
  indeterminate tiles (sun-disc PCF found mixed depths) are never
  ray-traced (work queue is allocated but never dispatched), so the
  denoiser starts them shadow-biased; object canopies read dark, soft
  penumbras read wider than the 3×3 hardware-bilinear PCF. Tunable via
  sun size / blocker offset.

## Risks / open items

- **Raster-only boundary bias**: the denoiser's prepare pass treats only
  "definitely lit" lanes as lit — indeterminate (soft-edge) pixels start
  shadow-biased and the soft-shadow filter blurs them. Shadow edges will
  read softer/wider than today's hardware-bilinear PCF. This is a tunable
  look change (sun size, blocker offset), not a defect — but expect an A/B
  review gate in Phase 4.
- **Jittered matrices vs denoiser reprojection**: the denoiser reprojects
  NDC + depth through `reprojectionMatrix`; our depth/velocity are
  jittered under FSR. Sub-pixel mismatch at worst; verify with FSR on/off
  A/B (open item, Phase 4). If it ghosts, feed the denoiser
  jitter-corrected matrices (we already track both jittered/no-jitter
  camera UBOs).
- **Fallback escape hatch**: keep the PCF path (it stays compiled); if the
  denoised mask is too soft for the near cascade, a per-cascade split is
  cheap to add inside `sampleShadowFull` (mask for far, PCF for near).
- **Classifier cascade limit**: 4 cascades max (we use 2 active; layers
  2–3 of the 4-layer D32 array stay unused/stale — `cascadeCount = 2`).
- **Linux `wchar_t` context-size bump** for the classifier — the expected
  first build failure; the static assert gives the exact number.

## Validation recipe (every phase)

`./scripts/build.sh` → `./scripts/run.sh play log 5000` (≥5000 ms — asset
loading) → parked-camera screenshots (`ENGINE_HIDE_GUI=1` clean variants,
per `AGENTS.md` — never move the parked player/camera) → RenderDoc capture
(`docs/renderdoc-capture.md`) when formats/barriers are in question →
linux **and** win archive builds. Mask-specific: `ENGINE_HYBRID_SHADOWS_DUMP`
frame dumps; classifier cascade/UV sanity via the dump + `SHADOW_DEBUG`
stages of the still-compiled PCF path.
