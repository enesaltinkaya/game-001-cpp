# notes

## brainstorm

I have enough grounding now. Here is the ideation pass.

## Core difficulty

The Brixelizer GI outputs are raw *incoming radiance* (FFX contract: the consumer must apply material response, exactly like IBL split-sum), but the engine's composite pass adds them to the scene **without any albedo/F0 weighting** — and because the GI radiance cache is fed from the previous frame's lit output (`prevLitOutput`), the missing albedo attenuation also makes the temporal feedback loop gain ~1, so the un-attenuated bounce energy inflates until the frame looks flat and washed out. The second difficulty is that the GBuffer has **no per-pixel albedo** (`outMaterial = vec4(roughness, metallic, alphaMask, 0)`), so the correct weighting can't be expressed with the buffers the composite currently sees.

## Reductions / key lemmas

1. **FFX consumer contract (verified in the reference sample):** `fsr3.1/git/samples/brixelizergi/shaders/lightinggicommon.h:161-162` — `diffuse = diffuseGI * baseColor * factor` (with `baseColor *= (1-metallic)`) and `specular = specularGI * (F0*brdf.x + F0_90*brdf.y) * factor`. The FFX docs (`docs/techniques/brixelizer-gi.md:172`) say to composite GI "the same way you would do with Image based lighting… split-sum approximation." Our `composite.comp:139-147` does `composite += giD * factor` / `composite += giS * factor` — raw radiance added to every surface regardless of albedo. Dark house walls receive the same bounce light as white walls, tinted by the sky/env color → desaturation + contrast loss = "washed out."
2. **Feedback-loop lemma:** the radiance cache is populated from `prevLitOutput` (previous composite, `VulkanBrixelizerPass.cpp` ~line 970). Steady-state GI ≈ Σ_k (product of surface responses along k bounces) · direct. With albedo (≤1) per bounce this converges like real multiple scattering; without it, per-bounce gain ≈ 1, so GI converges to an *unattenuated* irradiance of the scene — systematically too bright, and it grows over the first seconds of a static (T-pose, parked camera) shot. This matches the user's static-scene reproduction exactly.
3. **IBL-off is an isolator, not a cause:** `VulkanIbl.cpp:246-252` — `ENGINE_IBL_DISABLED` only zeroes the scene-side ambient; the env image "stays valid so GI passes can still sample the environment map." So with IBL off, GI is the *only* ambient term and its (wrong, unweighted) magnitude dominates the image. Env scale itself is modest (`BRIX_GI_ENV_INTENSITY = 0.1`), so the excess energy comes mainly from the un-attenuated cache feedback, not the env tap.
4. **No albedo exists anywhere in the GBuffer chain** (checked `VulkanFrameResources.h`, `scene.frag` outputs, `VulkanPipe.h`): any correct fix must *create* a per-pixel albedo (or precomputed GI-weight) buffer. `VulkanPipe` already has an unused `colorFormat4` slot, so a 4th GBuffer attachment is infrastructure-supported.
5. **Bounded blast radius:** the washed-out region is player + house + ground, i.e. surfaces written by `scene.frag` (player, house if scene mesh), `heightmap_terrain.frag`, and `azgaar_props.frag` — those are the only shaders that must emit the new albedo output; sky/empty pixels are skipped by the composite (`isSky`), so no clear-correctness concern beyond the terrain pass's existing clear-first pattern.

## Candidate approaches

**A. Albedo GBuffer + correct split-sum weighting in composite (full fix).**
Add a 4th GBuffer attachment (RGBA8 or RGBA16F albedo) written by scene/terrain/props shaders; composite multiplies `giD` by `albedo*(1-metallic)` and `giS` by `F0*brdf.x + brdf.y` (BRDF LUT via `sceneBuffer.ibl.brdfLutIndex`, F0-only fallback when IBL LUT absent).
Risk: touching every GBuffer-writing pass (attachment declaration, load/clear ops, fragment outputs) — mechanical but wide; a missed writer leaves stale albedo in a region. Effort: medium-high (the bulk of the work).

**B. Tune the existing `ENGINE_BRIX_GI_DIFFUSE_FACTOR` / `SPECULAR_FACTOR` env knobs down.**
One-line "fix" via env var; also serves as the diagnostic A/B that confirms over-addition (if lowering the factor removes the wash-out, the magnitude story is proven).
Risk: band-aid — dark surfaces still get relatively too much light, metals get unweighted specular GI; not a root-cause fix. Effort: trivial.

**C. Premultiply GI in a post-dispatch compute inside the brixelizer pass.**
Small full-res compute after `ffxBrixelizerGIContextDispatch` that reads the new albedo buffer and writes weighted GI into a second texture pair.
Risk: needs the same new albedo buffer as A plus an extra texture pair, extra pass, and extra layout juggling — strictly more moving parts than doing it in the composite, which already samples the GBuffer. Effort: medium-high, worse value than A.

**D. Approximate albedo from existing buffers (e.g. derive from `sceneColor`/direct light).**
No new buffer.
Risk: inverting lighting is unreliable (shadows, emissive, specular, fog already mixed in) — would produce patchy, wrong-weighted GI. Effort: low, but rejected on correctness.

## Recommended approach

**A**, with **B as the diagnostic step 0**. It is the only approach that matches the FFX consumer contract (the same split-sum the reference sample and the engine's own IBL path use), and it fixes the feedback-loop gain at the same time — with albedo < 1 per bounce, the radiance-cache feedback becomes a convergent geometric series. It must be true that: (1) every GBuffer-writing pass that covers visible geometry emits the albedo attachment (scene, heightmap_terrain, azgaar_props; water/decal if they write the GBuffer), (2) the new attachment is cleared by the terrain pass's clear-first pattern so load-on-top passes never read stale data, and (3) the composite's absent-sentinel pattern is preserved so the no-GI frame stays bit-identical.

## Proposed tasks

1. **Reproduce + confirm the mechanism (no code change).** Build, then with the parked camera: `ENGINE_IBL_DISABLED=1 ENGINE_AZGAAR_WEATHER=0 ENGINE_TPOSE=1 ENGINE_HIDE_GUI=1 ./scripts/run.sh play screenshot /tmp/gi_before.jpg`; also dump the raw GI buffers (`ENGINE_BRIX_GI_SAVE=1`, a few frames) and an A/B with `ENGINE_BRIX_GI_DIFFUSE_FACTOR=0.3 ENGINE_BRIX_GI_SPECULAR_FACTOR=0.3`. Verify: washed-out player/house in the before shot, raw GI dumps show bright env-colored irradiance on dark surfaces, and the factor A/B reduces the wash-out (proving over-addition).
2. **Add the albedo GBuffer attachment.** New frame-resource image + getter (`vulkanFrameResourcesGetAlbedo`), `colorFormat4` on the scene/terrain/props pipes (plus clear in the terrain pass's clear-first path), and `out location 3` albedo writes in `scene.frag`, `heightmap_terrain.frag`, `azgaar_props.frag`. Independently verifiable: build warning-free + a debug dump of the new buffer (or a temporary composite override) shows correct per-surface albedo.
3. **Weight the GI in the composite.** In `composite.comp`: `composite += giD * albedo.rgb * (1.0 - metallic) * giDiffuseFactor` and `composite += giS * (F0 * brdf.x + brdf.y) * giSpecularFactor` using `sceneBuffer.ibl.brdfLutIndex` with an F0-only fallback when the LUT index is 0; keep the `0xFFFFFFFF` sentinel path untouched. Verifiable: build + no-GI frames remain bit-identical (sentinel path), with-GI frames finite and non-saturated.
4. **End-to-end verification.** Rebuild via `./scripts/build.sh`, re-run the exact user recipe (IBL off, weather off, T-pose, parked camera) to `/tmp/gi_after.jpg`, and compare against task 1's baseline: player + house colors should show restored contrast/saturation (dark surfaces dark, env bounce visible but albedo-tinted), no new artifacts (horizon, sky, transparent OIT objects).

## round 1

Worker id 1 (reproduce + confirm mechanism). No code changes.

Findings:
- Washed-out look confirmed (/tmp/gi_before.jpg): player suit, house walls, shadowed ground all flat pale grays, very low contrast.
- Over-addition confirmed by A/B (/tmp/gi_factor03.jpg at DIFFUSE/SPECULAR_FACTOR=0.3): contrast clearly restored; wall lum 108->70, shadow-ground 112->74; sunlit ground unchanged. Wash-out scales linearly with GI factor.
- Raw GI dumps (pre-composite): diffuse GI is strong env-colored irradiance at EQUAL strength on dark and light surfaces (raw radiance, no albedo weighting). Specular GI is pure black in this scene -> entire wash-out comes from the diffuse GI term.
- Temporal ramp: frame 0 GI black, full strength by frame ~20 (radiance-cache feedback fill-in).
- Env knobs: factors read in VulkanCompositePass.cpp:30-31; ENGINE_BRIX_GI_SAVE dumps /tmp/brix_gi_{diffuse,specular}_N.jpg from VulkanBrixelizerPass.cpp:1123-1134.

Remaining steps: none. (Note: specular GI zero in this scene, so specular branch of the fix won't be visible in this exact shot.)

## round 2

Worker id 5 (albedo GBuffer attachment). Code changed.

Findings:
- New `albedo` frame image (R16G16B16A16_SFLOAT, COLOR_ATTACHMENT|SAMPLED|TRANSFER_SRC|DST) + getter `vulkanFrameResourcesGetAlbedo()` in VulkanFrameResources.{h,cpp}.
- `colorFormat4` on scene pipes (load-on-top, clear4 off), heightmap_terrain scene pipe WITH clearColor4Enabled=1 (clear-first) + wireframe pipe (clear off), azgaar_props pipe (load-on-top). `.color4 = albedo` in beginRender.
- Shaders: scene.frag `outAlbedo = vec4(baseColor.rgb, 0)`; heightmap_terrain.frag `outAlbedo = vec4(baseColor, 0)` (after full tint chain); azgaar_props.frag `outAlbedo = vec4(albedo, 0)` (post tint).
- Vulkan.cpp: `albedo` token added to ENGINE_DEBUG_DUMP_IMAGES tables.
- VulkanPipe already had full colorFormat4/clear plumbing — no pipeline-creation changes.
- Verified via ENGINE_DEBUG_DUMP_IMAGES=albedo: /tmp/albedo_check_albedo.jpg shows correct per-surface albedo (red house, green grass, dark suit + yellow trim, black cleared regions). Composite still washed out (task 6 pending) — expected.
- Known limitation: decal/water/river passes write only color1; decal pixels keep underlying albedo (fine for GI weighting).

Verifier id 5: PASS.
- SKIP_NAVMESH=1 ./scripts/build.sh exit 0; clang++ -fsyntax-only of all 5 changed TUs with strict flags: zero warnings; .spv newer than .frag sources; ./scripts/run.sh play log 8000 clean (0 errors); screenshot run produced /tmp/verify_after.jpg, plausible frame, parked db untouched.
- Verifier note: no /tmp/baseline.jpg existed for pixel comparison (baseline is /tmp/gi_before.jpg from round 1).

Remaining steps: task 6 can consume vulkanFrameResourcesGetAlbedo() as sampled image.

## round 3

Worker id 6 (weight GI in composite). Code changed.

Findings:
- composite.comp: added albedoIndex push constant; GI block samples albedo GBuffer; diffuse = giD * albedo * (1-metallic) * factor; specular = giS * (F0*brdf.x + brdf.y) * factor via sceneBuffer.ibl.brdfLutIndex (same formula as scene.frag IBL path); sentinel fallback reproduces pre-fix unweighted expressions bit-for-bit.
- VulkanCompositePass.cpp: albedoIndex in CompositePushConstants; fetches vulkanFrameResourcesGetAlbedo(); transitions to SHADER_READ_ONLY_OPTIMAL only when a GI output is consumed; transitions back to COLOR_ATTACHMENT_OPTIMAL after dispatch (scene pass relies on init-time layout — first run crashed on this layout invariant).
- Verified quantitatively (exact user recipe, /tmp/gi_task6.jpg): wall (109,108,103)->(90,49,43) pale gray -> deep red; shadowed ground (90,112,133)->(51,66,59); sunlit ground ~unchanged (direct light untouched). Contrast/saturation restored, no new artifacts.
- Build warning-free, 0 CRIT/ERROR in log, clean exit. Specular branch code-reviewed only (specular GI zero in this scene).

Verifier id 6: PASS.
- build.sh exit 0; composite.comp.spv.debug regenerated after .comp edit; isolated strict recompile of VulkanCompositePass.cpp: zero warnings; exact user-recipe screenshot run clean; new code path active in log.
- Visual fix confirmed: pre-change baseline washed out (near-white ground, faded outfit), post-change capture has proper light/shadow split, deep-black outfit with detail, saturated red wall. Parked player/camera untouched.

Remaining steps: none — task 4 (end-to-end verification) satisfied by this verifier run.
