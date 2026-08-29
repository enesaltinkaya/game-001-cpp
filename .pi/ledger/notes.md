# notes

## brainstorm

## Core difficulty

"Color bleeding between a red wall and green ground" is a *symptom* with several independent candidate mechanisms in this pipeline (TAA temporal accumulation, FSR 3.1 upscaling, MSAA resolve, G-buffer load ordering, screen-space samplers), so the task is hard because we must identify which stage the user's expected bleed would come from — and pinpoint the exact code that prevents it — rather than looking for one obvious "bleed" function.

## Reductions / key lemmas

1. **Bidirectional boundary bleed = neighboring-pixel albedo mixing.** The house (mesh, red material) and the ground (implicit-lattice heightmap terrain, green) are both drawn into the G-buffer with a depth test. At any single pixel, the winner is decided purely by depth — so *same-pixel* cross-contamination is impossible without a broken depth test. Any visible bleed must originate in a stage that samples *neighboring* or *previous-frame* pixels.
2. **G-buffer load ordering is not a bleed source here.** `VulkanScenePass.cpp:66-73` shows the scene pass *loads* (not clears) the terrain pass's G-buffer attachments (color1–4). That's correct same-pixel overdraw compositing — terrain albedo only persists where the scene doesn't draw, and depth ordering keeps it clean. It cannot mix wall color into ground pixels.
3. **TAA is the prime suspect for the *expected* bleed — and it has three anti-bleed mechanisms.** `VulkanTaaPass.cpp:124-146` documents: (a) relative color-rejection (`ghostThreshold`), (b) relative depth-rejection (previous frame's depth stored in the accumulator's alpha, compared per-pixel), and (c) YCoCg neighborhood clamping in the shader ("prevents ghosting independently, so the rejection only needs to catch truly [bad pixels]"). A red wall adjacent to green ground is exactly the high-contrast edge these mechanisms suppress. If the user expected TAA ghosting/bleed at that boundary, this is almost certainly why they don't see it.
4. **FSR 3.1 upscaling is the second suspect** (spatial reconstruction at edges), but it operates after TAA and typically produces mild blur, not saturated cross-color bleed.
5. **The pipeline is experimentally accessible.** TAA has env knobs (`ENGINE_TAA_WEIGHT`, `ENGINE_TAA_GHOST`, `ENGINE_TAA_DEPTH` — `VulkanTaaPass.cpp:267-284`) and `ENGINE_DUMP_TAA`, plus the parked player already frames the object under test. So the hypothesis "TAA rejection/clamping suppresses the bleed" is cheaply falsifiable via A/B screenshots.

## Candidate approaches

**A. Pure code trace.** Read the TAA pass + its shader (rejection/clamping logic), the FSR pass, MSAA resolve, and G-buffer load ops; write up which stages *could* bleed and the exact lines that prevent it. *Risk:* may miss a stage (contact shadow, SSR, AO all sample the G-buffer) and can't confirm what the frame actually shows. *Effort:* medium.

**B. Empirical A/B only.** Screenshot with TAA env knobs set to extremes (e.g. `ENGINE_TAA_WEIGHT=0` / permissive thresholds) and compare the wall/ground boundary. *Risk:* shows *that* something changes but not *why*; without the code trace the explanation stays hand-wavy. *Effort:* low.

**C. RenderDoc capture.** Headless capture + Python replay, inspect G-buffer, TAA history, and FSR output textures at the boundary pixel-by-pixel. *Risk:* heavy (≈1 GB capture, replay scripting per `docs/renderdoc-capture.md`); overkill if a screenshot + code trace already answers the question. *Effort:* high.

**D. Hybrid: screenshot → targeted trace → A/B, RenderDoc only as fallback.** Cheap ground truth first, then confirm the mechanism in code, then falsify with the env knobs. *Risk:* the boundary may be too small in the screenshot to judge visually (mitigated by `ENGINE_DUMP_TAA` / RenderDoc if needed). *Effort:* low-medium.

## Recommended approach

**D.** The task is an explanation, and the explanation has a clear testable prediction: with TAA's rejection/clamping defeated via `ENGINE_TAA_*` env vars, the expected red↔green bleed should appear at the wall/ground boundary; with defaults it should not. A clean screenshot at the parked position plus a line-referenced read of the TAA pass/shader is enough to state the root cause with confidence; the A/B run converts that from plausible to demonstrated. RenderDoc is reserved for the case where the screenshot is inconclusive.

Must be true for this to work: (1) the parked player actually frames the house/ground boundary; (2) `ENGINE_TAA_WEIGHT`/`ENGINE_TAA_GHOST`/`ENGINE_TAA_DEPTH` accept values that fully disable the suppression (defaults and parsing semantics must be read first); (3) no other stage (FSR) re-introduces the bleed after TAA is disabled, or we account for it.

## Proposed tasks

1. **Capture ground truth.** Run `ENGINE_HIDE_GUI=1 ./scripts/run.sh play screenshot /tmp/bleed_default.jpg` and inspect the red-wall/green-ground boundary for any visible bleed. (Do not touch the parked player/camera db rows.)
2. **Trace the TAA suppression mechanism.** Read `c-engine/renderer/vulkan/pass/taa/VulkanTaaPass.cpp` (lines ~124-146, 267-284) and the TAA compute shader (locate it under the shader sources): document exactly how color-rejection, depth-rejection (accumulator-alpha depth), and YCoCg neighborhood clamping each prevent cross-edge bleed, with file:line references. Also note the default values of `ENGINE_TAA_WEIGHT` / `ENGINE_TAA_GHOST` / `ENGINE_TAA_DEPTH` and what values fully disable each mechanism.
3. **A/B falsification.** Re-run the screenshot with TAA suppression disabled (e.g. `ENGINE_TAA_WEIGHT=1` with permissive `ENGINE_TAA_GHOST`/`ENGINE_TAA_DEPTH`, or weight=0 to confirm the no-history baseline — exact values per task 2's findings) and diff the boundary region against task 1's frame; record whether the expected bleed appears.
4. **Fallback (only if 1–3 are inconclusive):** RenderDoc capture per `docs/renderdoc-capture.md` and headless replay to inspect the TAA history and FSR input/output textures at the wall/ground boundary, to rule in/out FSR as a secondary bleed source.

## round 1 (task 3)

Findings:
- Clean default frame captured at parked position (/tmp/bleed_default.jpg, 2880x1627). No visible color bleed at the wall/ground boundary under default settings - edge is sharp, consistent with TAA rejection/clamping suppressing expected ghosting.
- At both wall corner and right wall face, wall->ground transition is a sharp 1-2px step; only a thin dark contact-shadow line at wall base. No red halo on grass, no green tint on wall.
- Only non-boundary artifacts: DoF blur on foreground vegetation + expected soft shadow cast by wall onto grass.
- Naive r>g / g>r pixel classifier gives false positives (shadowed wall color misread as ground; vegetation misread). Any A/B diff must use a smarter boundary detector (strong r-g gradient, or vegetation-free columns x~1350-2050).
- Parked player/camera framing intact.

Remaining steps:
- Task 4 (A/B): re-run screenshot with TAA suppression disabled per task 2 env-var findings; diff boundary band (vegetation-free, x~1350-2050) vs /tmp/bleed_default.jpg.

## round 2 (task 2)

Findings (TAA suppression trace, no code changes):
- Pass: c-engine/renderer/vulkan/pass/taa/VulkanTaaPass.cpp; shader: c-engine/data/pak_0_engine/shaders/pass/taa/taa.comp. History = two ping-pong accumulators taaA/taaB R16G16B16A16_SFLOAT; ALPHA stores previous frame inverse-view-space depth (taa.comp:280-283). Cleared to black on creation -> alpha=0 = "no data" -> rejected (taa.comp:145-150).
- Mechanism 1 temporal depth-rejection: depthToInv (taa.comp:49-59); 3x3 neighborhood depth span (196-213); depthExcess = max(|S_curr-S_prev|/max(S) - nDepthSpanRel,0) (230-232); depthReject=smoothstep(threshold*0.25,threshold,excess) (233-234). At wall/ground boundary a reprojected history of the OTHER surface has large relative S diff -> reject=1 -> weight=0 -> output=current only (259). No cross-surface mixing.
- Mechanism 2 temporal color-rejection: 3x3 color AABB of current frame (86-110); prevDev = excursion outside box (250); colorReject=smoothstep(ghost*0.5,ghost,colorOutFrac) (254-255). History only rejected when outside current neighborhood color range -> red history on green pixel (or vice versa) far outside local AABB -> rejected.
- Mechanism 3 neighborhood clamping: NOTE clamps in RGB not YCoCg: prevClamped=clamp(prev,nMin,nMax) (274). Even if rejections bypassed, blended history cannot exceed current neighborhood color range -> red history cannot bleed green-edge pixels. Final: result=mix(curr,prevClamped,weight); weight=blendWeight*motionConf*(1-reject), zeroed when reprojected UV OOB or weather particles (259-277).
- Env vars: defaults Renderer.cpp:34-39 (taaWeight=0.9, taaGhost=1.0, taaDepth=0.06). Read per-frame via atof (VulkanTaaPass.cpp:267-284). rendererSetAASettings clamps (weight[0.5,0.95], ghost[0.3,1.0], depth[0.01,0.5]) BUT env-var path is NOT clamped (reads rendererGetAASettings then overwrites with raw env).
- Fully disable: ENGINE_TAA_WEIGHT=0 -> pure current frame (strongest no-TAA baseline, other mechanisms moot). ENGINE_TAA_GHOST=100 -> color rejection never fires. ENGINE_TAA_DEPTH=100 -> depth rejection never fires. KEY: with GHOST/DEPTH huge + default weight, ONLY remaining guard is the hardcoded RGB neighborhood clamp (mechanism 3), NOT env-disableable. So a true "expected bleed" A/B will show at most clamped-history bleed; a saturated red->green ghost only if the clamp itself were removed. Expectation for task 4: disabling rejections may still NOT reproduce strong bleed.
- G-buffer load ordering ruled out: terrain pass clears all 4 G-buffer attachments + depth (VulkanHeightmapTerrainPass.cpp:165-168); scene pass LOADS not clears (clearColor*Enabled=0, VulkanScenePass.cpp:66-73; VulkanPipe.cpp:529,545,561,577). Correct same-pixel overdraw; depth test decides every pixel; load-vs-clear cannot mix colors across surfaces.
- FSR ruled out: FSR3 here is spatial upscaler only (ffxFsr3UpscalerContextDispatch, VulkanFsrPass.cpp:570); inputs = current-frame color/depth/MV/masks only (501-527); no temporal accumulation -> no cross-frame ghost. Spatially only mild edge blur. Also FSR & TAA mutually exclusive: upscaler forces aaMode=AA_OFF (Renderer.cpp:178-181); final pass prefers TAA output when present (VulkanFinalPass.cpp:56-60). FSR default OFF (Renderer.cpp:33).

Remaining steps:
- Task 4: use ENGINE_TAA_WEIGHT=0 (no-history baseline) and/or ENGINE_TAA_GHOST=100 ENGINE_TAA_DEPTH=100 (unclamped by sanitizer) to disable both rejections; expect at most clamped-history bleed, not strong red<->green ghosting (clamp is hardcoded).

## round 3 (task 4)

Findings (A/B falsification, no code changes; artifacts /tmp/bleed_taa_noreject.jpg, /tmp/bleed_taa_off.jpg, crops + amplified diffs):
- Boundary located per-column as r-g sign crossing in vegetation-free band x in [1350,2050]; boundary rows ~950-1131 (wall base oblique). Diff band = +/-20px around per-column boundary; measured Delta(r-g) and DeltaRGB-magnitude.
- A/B-1 (rejections off GHOST=100/DEPTH=100, history on) vs default: mean Delta(r-g) ~0 both sides (ground -0.009, wall +0.071); max |Delta(r-g)| 15/22; only 5.6%/12.0% of band pixels differ >2 -> jitter/JPEG noise, not a color shift.
- A/B-2 (no history WEIGHT=0) vs default: larger scattered per-pixel noise (mean |DeltaRGB| ~5-6) as expected when temporal smoothing removed, but mean Delta(r-g) NEGATIVE on both sides (ground -0.16, wall -0.65); no systematic red-tinted band on grass nor green-tinted band on wall.
- Visual: three boundary crops near-identical; wall->ground stays sharp 1-2px step with same thin contact-shadow line. No red halo on grass, no green tint on wall, in any variant.
- Interpretation (confirms task 2): disabling both temporal rejections does NOT reproduce bleed. Reasons: (1) hardcoded RGB neighborhood clamp (taa.comp:274) is env-un-disableable and alone bounds any history to the current 3x3 neighborhood color range -> history can never push a ground pixel to saturated red or a wall pixel to saturated green; (2) with the parked STATIC camera, reprojected history at the boundary is almost always the SAME surface -> the cross-surface ghost scenario requires camera motion. So "no bleed" is guaranteed by the clamp regardless of rejection settings; rejections matter only under motion.
- Practical: to see ANY measurable history bleed here, only levers are removing the clamp (code change) or a moving-camera A/B; env vars alone cannot falsify it. RenderDoc (task 5) would only confirm the clamp's effect on the history buffer, not find a hidden bleed.

Remaining steps:
- Task 5 (RenderDoc) NOT needed - 1-4 conclusive. Task 6 can state: missing bleed is correct behavior.

## final analysis

**Question:** why is there NO red-wall→green-ground / green-ground→red-wall color
bleed at the house/ground boundary?

**Verdict: the absence of bleed is correct behavior, not a missing artifact.**
Two independent guarantees make cross-surface albedo bleed impossible in this
pipeline under the parked (static) camera:

1. **Same-pixel correctness (depth).** The red wall (scene pass, mesh) and the
   green ground (implicit-lattice heightmap terrain pass) both write the same
   G-buffer. Every pixel's color is decided by the depth test; the scene pass
   *loads* (not clears) the terrain's G-buffer, so terrain albedo only survives
   where the scene draws nothing. Load-vs-clear ordering can never mix one
   surface's color into the other's pixel.

2. **TAA's hardcoded neighborhood clamp.** TAA accumulates history in two
   ping-pong buffers; the reprojected previous-frame value is clamped to the
   current 3x3 neighborhood's color AABB before blending
   (`prevClamped = clamp(prev, nMin, nMax)`, taa.comp:274). This means a
   history sample can never push a ground pixel toward saturated red, or a wall
   pixel toward saturated green, regardless of the rejection thresholds. Two
   additional temporal rejections (depth-reject via the accumulator's alpha
   depth, and color-reject vs the local AABB) further suppress stale cross-surface
   history, but the clamp alone is sufficient.

**Empirical confirmation (A/B):** disabling both temporal rejections
(`ENGINE_TAA_GHOST=100 ENGINE_TAA_DEPTH=100`) and removing history entirely
(`ENGINE_TAA_WEIGHT=0`) did NOT reproduce any red/green bleed at the boundary —
the crops stayed near-identical. This is expected: (a) the clamp is hardcoded and
not env-disableable, and (b) with a static camera the reprojected history at the
boundary is almost always the *same* surface, so the cross-surface ghost scenario
(which requires camera motion to bring the other surface's history onto the
boundary pixel) never arises.

**Where bleed *would* come from, if it existed:** the only stages that sample
neighboring/previous pixels are TAA (temporal) and FSR (spatial). FSR is ruled
out — it is a spatial-only upscaler with no temporal accumulation, and it is
mutually exclusive with TAA (enabling the upscaler forces AA off; the final pass
prefers TAA output when present). So TAA is the only candidate, and its clamp
defeats it.

**Bottom line:** no bug. If you want to *observe* cross-surface bleed, the only
levers are (a) a moving-camera A/B (so reprojected history can carry the other
surface onto the boundary), or (b) removing the neighborhood clamp in taa.comp —
both are deliberate code changes, not something the current pipeline does.
