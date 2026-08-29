# notes

## brainstorm

## Core difficulty

The artifact is *temporal* (color changing frame-to-frame on the character), so it cannot be seen in any single frame, and the render pipeline has multiple independent per-frame stochastic sources (FFX GI ray jitter, FSR/TAA, volumetric fog, OIT/DCC on AMD) — the task is really attribution: which stage owns the flicker, not just that it exists.

## Reductions / key lemmas

1. **Static-scene reduction.** T-pose + parked camera + no input makes every temporal-filter input (view/proj, depth, normal, motion vectors) frame-invariant. FFX Brixelizer-GI's per-ray jitter is driven by an *internal* `frameIndex` that the host auto-increments every dispatch (`ffx_brixelizergi.cpp:1775`, fed to `SampleBlueNoise`/PCG hashes in `ffx_brixelizergi_main.h`) — so jitter still changes every frame even in a static scene. Therefore: if color still changes per frame after warm-up, the temporal filter is rejecting/restarting history every frame (or SDF content is changing), never a "needs more samples" transient.
2. **Warm-up lemma.** `ENGINE_SCREENSHOT_DELAY_MS=12000` means the burst starts ~12 s (hundreds of frames) after GI context creation. Any flicker visible in the burst is *persistent* per-frame rejection, not convergence.
3. **Stage-isolation lemma (the big one).** `ENGINE_BRIX_GI_SAVE=1 ENGINE_BRIX_GI_SAVE_EVERY=1` already exists and dumps the raw, *pre-composite* GI diffuse/specular images every frame to `/tmp/brix_gi_diffuse_N.jpg` / `brix_gi_specular_N.jpg` (VulkanBrixelizerPass.cpp:1067). Comparing the raw-GI frame sequence against the swapchain burst directly splits the hypothesis space: raw GI stable + composite flickers ⇒ downstream (FSR optical flow/TAA/volumetric/OIT-DCC); raw GI flickers ⇒ the GI temporal filter itself (reprojection/disocclusion rejection, wrong velocity for the skinned character, or the voxelizer's per-frame round-robin cascade re-bake — `ffxBrixelizerRawGetCascadeToUpdate(frameIndex, ...)` updates one cascade per frame). `ENGINE_BRIX_GI_DEBUG=1/2` adds radiance-cache/irradiance-cache visualizations, which are only written in the same save block — all three env vars compose in a **single run**.
4. **Why wrists/legs/neck specifically.** With IBL ambient off, character diffuse ≈ direct sun + GI only. The SDF holds *props only* (no terrain, no character), so rays from thin limbs mostly miss to the env map at `BRIX_GI_ENV_INTENSITY=0.1` (low radiance, high relative per-ray variance), while grazing hits on nearby canopies add variance. Thin geometry = worst signal-to-noise, so a broken temporal filter is most visible exactly where the user sees it. This is a consistency check: if the flicker were uniform everywhere, the GI-noise hypothesis would be weaker.
5. **Confounds to control.** Weather cannot be fully disabled — `ENGINE_AZGAAR_WEATHER_COUNT` clamps to ≥1 (VulkanAzgaarWeatherPass.cpp:258); one particle is negligible but the attribution is not perfect. Also verify the parked camera truly doesn't drift (a moving camera would invalidate everything): the test is whether *background* regions are temporally stable in the burst while the character flickers. Use `ENGINE_HIDE_GUI=1` so HUD doesn't pollute the character regions.

## Candidate approaches

1. **Combined single capture run** — `./scripts/run.sh play screenshot /tmp/brix 16` with `TERM=xterm ENGINE_IBL_DISABLED=1 ENGINE_AZGAAR_WEATHER_COUNT=1 ENGINE_TPOSE=1 ENGINE_SCREENSHOT_DELAY_MS=12000 ENGINE_HIDE_GUI=1 ENGINE_BRIX_GI_SAVE=1 ENGINE_BRIX_GI_SAVE_EVERY=1 ENGINE_BRIX_GI_DEBUG=1`. Per-frame readbacks (16 screenshots + 16×3 GI saves) may hitch the run; if frame timing perturbs the artifact, drop to 12 frames.
2. **Burst-only, minimal** — same run without the GI save/debug envs; cheaper, but loses the stage split and forces a second run later.
3. **Static code audit first** — read the FFX GI reprojection/disocclusion code + engine history/velocity wiring to find the defect without frames. Fast, but the two leading mechanisms (per-frame disocclusion rejection vs SDF round-robin content change) are not distinguishable by reading alone; likely wasted round.
4. **A/B control runs** — repeat the burst with IBL on (or `ENGINE_TPOSE` off) to bracket the effect. Good evidence, but a second+ run; keep as fallback only.

## Recommended approach

Approach 1, with 3 as the follow-up against whatever region the frame analysis points at. One run yields all three signals (final-frame burst, raw pre-composite GI, FFX cache view), which is exactly the data needed to assign the flicker to a stage; the code audit then has a concrete target (character velocity vs SDF update) instead of two open suspects. Must be true for it to work: the parked player/camera stay put during the run (background must be temporally stable in the burst — verify from frame 1's comparison, and don't touch db.db rows or spawn code), and the ~12 s delay must land in gameplay (asset load complete — per AGENTS, keep ≥5 s, and watch the log for the GI context creation lines before the capture window).

## Proposed tasks

1. **Capture run.** Execute the combined single run from Recommended approach; verify deliverables on disk: `/tmp/brix_1..16.jpg` + paired `/tmp/brix_gi_diffuse_N.jpg` / `brix_gi_specular_N.jpg` / `brix_gi_debug_N.jpg` sequences, clean engine exit, and log lines confirming IBL-disabled, T-pose, and GI context created *before* the capture window.
2. **Burst analysis.** Compute per-pixel temporal delta/stddev across the 16 composite frames (small Python/PIL script); produce a difference map; quantify flicker magnitude (levels) and spatial extent on the character (wrist/leg/neck) and measure background stability (camera-drift check); compare per-frame variance of the same body regions in the raw GI images to stage-attribute the artifact (GI vs downstream).
3. **Cache-view check.** Inspect the `brix_gi_debug_*` (radiance/irradiance cache) sequence: flickering cache on the character ⇒ reprojection/disocclusion failing per frame; stable cache ⇒ jitter is being integrated and the artifact is downstream or in SDF content (then examine per-frame voxelizer stats in the log for the round-robin re-bake signature).
4. **Root-cause note.** Using the attributed stage, inspect the specific code path (FFX GI disocclusion/reprojection inputs from the engine — depth/normal history, `motionVectorScale`, and whether the skinned character writes correct velocity into the GBuffer velocity buffer at all) and write the finding + minimal fix proposal into notes.md.

## round 1 (task 1: capture run)

**Findings**
- All deliverables on disk: /tmp/brix_1..16.jpg (~1.1 MB each), /tmp/brix_gi_{diffuse,specular,debug}_0..43.jpg (SAVE_EVERY=1 → every frame), game.log truncated to this run, /tmp/brix-run3.log kept.
- Log evidence: `vulkanIbl: disabled via ENGINE_IBL_DISABLED` (line 47); GI context created (2880x1627, 50% internal, DEPTH_INVERTED) + `GI debug visualization = radiance cache` at 04:31:46; burst 04:31:58.87 → 04:32:06.24, clean exit. Burst start = +12.9 s ≈ arm delay.
- Weather is *active* (leaves) but clamped to 1 particle (env clamp ≥1); count only printed with ENGINE_AZGAAR_WEATHER_DEBUG.
- T-pose confirmed visually in brix_1.jpg (arms horizontal), no HUD; no missing-clip warning.
- **Frame mapping: brix_N ↔ GI frame 27+N** (brix_1↔28 … brix_16↔43).
- No GI context destroy/re-bake during burst — SDF content stable throughout.
- Burst hitches ~2.2 fps (triple GI readback + screenshot per frame); burst is a slowed-down time slice.
- brix_gi_debug_28.jpg (radiance-cache view) fully black — plausible with IBL off; verify across 0..43; fallback: ENGINE_BRIX_GI_DEBUG=irradiance.
- Raw GI diffuse frame 28 carries real signal: warm GI on skin, bluish env in background, dark zero-GI spots on legs/hips.

**Remaining steps**: none for task 1; tasks 2–4 use the brix_N↔GI(27+N) mapping.

## round 2 (task 2: composite burst analysis)

**Findings**
- Camera drift PASS: phase correlation f1→f8/f1→f16 peak at (0,0), corr 0.992–0.993. Background snow noise floor std 0.93–1.59 levels; strong flicker negligible outside character.
- Flicker is character-specific: strong-flicker pixels 7–30× more prevalent on character. Flagged ROIs (neck, wrists, legs) hold ~86% of strong-flicker pixels — matches user report.
- Signature: ~1000–1400 px 2-state flicker, lit↔near-black with 100–140-level swings, persistent all 16 frames (no decay); near-black state is NOT background color → internal surface color-state flicker.
- Stddev map: bright rim contours along ALL character edges (adds TAA reprojection to suspects) + discrete hotspots at wrists/knees/collar.
- Hotspots flicker largely independently (pairwise |r|<0.3 mostly) → local per-pixel rejection/stochastic, not global history reset.
- Caveat: burst hitches ~2.2 fps; composite is post-FSR output (GI + downstream jointly).
- ROI coords (full-res): neck (1535,755), hand_L (676,789), hand_R (2199,874), legs (1374,1400). Maps/scripts in /tmp/brix_analysis/.

## round 3 (task 3: stage attribution)

**Findings**
- Raw GI diffuse carries the flicker per-pixel: ~3700 char px (0.74%) std>0.02, blue-dominant SDF-hit pixels, localized to flagged limbs. ROI-mean luma stable (0.067–0.073 base) but per-pixel noise, hot-pixel sets different each frame (Jaccard≈0) → stochastic per-frame ray jitter.
- Raw GI specular = exactly 0 all frames.
- Both cache debug views (radiance, irradiance) fully black, all frames, per-pixel std=0 → brick-SH/world-probe and radiance cache data empty (FfxBrixelizerGIInterpolateBrickSH returns zero ⇒ has_world_probe=false everywhere; ffx_brixelizergi_main.h:1835).
- Log: single BRIX_STATS line (stride 30): bricksInUse=4036/4037, attempted=0, static/dynamic tris 0, instances=379, tiles=1 — SDF static, no re-bake during burst.
- Composite cross-check: GI↔composite per-pixel corr r≈0.15 same frame; hot-pixel set overlap Jaccard≈0.20; GI hot px show NO flicker in composite → composite 2-state additionally shaped downstream (tone-map/TAA/FSR/OIT-DCC).
- KEY CAVEAT: saved GI jpgs are per-frame auto-normalized (min..max→0..255); absolute radiance only via log `float range` lines.
- Attribution: GI temporal reprojection/accumulation failing on character pixels (fresh-MC per-frame fallback). Suspect inputs: GBuffer velocity/motion vectors for skinned character, `desc.motionVectorScale = −1/render-res`, `FfxBrixelizerGIGenerateDisocclusionMask` (FFX_DISOCCLUSION_THRESHOLD 0.9, normal+world-pos history compare). Not SDF round-robin.
- 2nd burst with irradiance debug view: /tmp/brix2_*.jpg (run1 debug images overwritten).

## round 4 (task 4: root-cause code mapping)

**Findings**
- Engine input map (VulkanBrixelizerPass::dispatchGI): depth=frame-N D32 rev-Z; historyDepth=frame N-1 copy; normals R16F world raw; motionVectors = engine velocity in PIXEL units (ndcCur−ndcPrev)·viewport·0.5 y-flipped (scene_depth.frag:29-33); motionVectorScale=(−1/renderW,−1/renderH). MV convention verified CORRECT against FFX LoadMotionVector (callbacks_glsl.h:955-967); 50% mode rebinds to downsampled buffers (ffx_brixelizergi.cpp:1330-1340) so self-consistent.
- Velocity buffer NOT cleared per frame (VulkanDepthPass.cpp:120) — sky holds stale MV but ffxIsBackground masks it (harmless). Character/terrain/props all write MV each frame; T-pose joints static → MV≈0.
- SDF round-robin exonerated again: variable-rate cascade updates (ffx_brixelizer_raw.cpp:2342); static cascade no-op; bricksInUse=4037 constant, attempted/cleared/merged=0.
- FFX per-frame stochastic sources (ffx_brixelizergi_main.h): probe seed pixel re-jittered every frame (444-507); checkerboard re-trace ~1/16 probes/frame with fresh jitter (910); per-pixel ReprojectGI(mask-gated) → 4-nearest-probe interpolation (power-8 weights) or world-probe (BRICK SH) fallback, lerp w capped 64.
- ROOT-CAUSE HYPOTHESIS (primary): per-pixel history reset every frame on character-limb pixels. Gates: (a) RED-FLAG path main.h:1597-1602: `if (!has_world_probe && weight_sum < 1e-3) StoreStaticGITarget(tid, 0)` → temporal_weight=1 → fresh MC. Character not voxelized (SDF=props only) ⇒ has_world_probe=false everywhere on character (proven: all-black irradiance view; FfxBrixelizerGIInterpolateBrickSH gate shs[0].w>=16). Thin limbs: probe weight_sum hovers the 1e-3 threshold, per-frame probe jitter + checkerboard re-trace toggles the reset. (b) Disocclusion mask (main.h:1890-1915, threshold 0.9, exp normal+world-pos factors): per-frame GBuffer jitter (skin interp via utils::timer.alpha) swings it.
- ABSOLUTE-radiance re-analysis (de-normalized via log float ranges): round-3 "per-pixel noise" was inflated by per-frame auto-normalization; absolute temporal stddev small (mean 0.012, p99 0.015) but BLUE (ch2) events abs≈0.9-1.9 (env-sky hits) appear/disappear frame-to-frame with NO persistence → temporal filter not integrating. Event pixels land exactly on user ROIs. ~875 px exceed std 0.05.
- SYSTEMATIC ONSET (key): in BOTH runs blue events only at GI frame 1 (post-clear) + frames 29-43 (burst window); 26/26 pre-burst frames clean (p~1e-16 if random). Burst = wall-clock +12.9 s + readback-hitched ~2.2 fps. Top suspect: timer.alpha pose interpolation under changed pacing moving skin a few cm/frame, toggling ray hit/miss on canopy SDF. Env map verified static (sun aligned once at load; vulkanIblCycleNext GUI-only).
- Ruled out: SDF round-robin, MV convention bug, GI-context re-create mid-burst, world-probe contribution, stale-MV on world geometry.
- Decisive verification plan (from worker): (1) fork patch in ffx_brixelizergi.cpp (game-001 already patches this file): read back FFX per-pixel debug target (red = weight_sum<1e-3 reset) + DISOCCLUSION_MASK each frame alongside GI saves; (2) velocity + GBuffer depth/normal diff dump on limb ROIs; (3) A/B control burst without per-frame GI readbacks.

## round 5 (task 6: decisive mask dump) — VERIFIER PASS

**Findings**
- Fork-patched FFX (ffx_brixelizergi.h/.cpp: optional outputDebugTarget/outputDisocclusionMask + size getter) and VulkanBrixelizerPass.cpp (ENGINE_BRIX_GI_MASK_SAVE knob, raw dumps /tmp/brix_gi_mask_{debug,disocc}_%u.raw). Rebuilt libffx_fsr3upscaler_vk.a (Linux).
- DECISIVE: at the exact limb hot pixels, reset flag (weight_sum<1e-3, no world probe) set 6-13/16 frames; disocclusion 0/16. Hot-pixel coverage: reset 87.7% overall, 98.8-100% inside limb boxes; disocclusion 5.5% overall.
- Mechanism confirmed: `!has_world_probe && weight_sum < 1e-3` (main.h:1597-1602) → StoreStaticGITarget(0) → history+sample count cleared → fresh per-frame-jittered MC sample. Per-frame probe jitter + checkerboard re-trace makes the power-8 weight sum hover the threshold on thin non-voxelized limbs → 2-state flicker exactly as observed.
- Disocclusion mask exonerated as primary (covers only edges/silhouette; explains the secondary rim contours in the stddev map).
- Reset is persistent (fires in pre-burst frames too) — a permanent mechanism, not a readback-hitch artifact; the round-4 "burst-window only" applied to the *blue* manifestation (a jitter ray missing to the 0.1-intensity env sky), not the reset state.
- Mask geometry: internal 1440x813, probe 1440x816. brix4_N <-> gi file (37+N).
- Fix candidates: raise/widen the reset gate for non-voxelized (character) pixels, or give the character a world probe / put it in the SDF as a dynamic cascade.
- NOTE: Windows libffx_fsr3upscaler_vk.a NOT rebuilt (Linux test only); a Windows release build regenerates it. FFX fork working tree contains only the two intended diagnostic edits (verified by worker after scoped git restore of shader_output wipe).

**Verifier**: PASS — SKIP_NAVMESH=1 build.sh exit 0, artifacts newer than sources; clean 16-frame run, no errors/asserts; T-pose + IBL-off render confirmed; parked player/camera untouched.

## final: root cause (integrated)

**The glitchy color animation on the character (wrists, legs, neck) is the FFX Brixelizer-GI per-pixel temporal history being reset every frame on the character's thin-limb pixels, because the character is never voxelized (SDF = props only) and the 4-nearest-probe interpolation weight hovers at/below the 1e-3 reset threshold there.**

Chain of evidence:
1. Composite burst (16 frames, IBL off, T-pose, weather=1 particle): 2-state flicker (lit<->near-black, 100-140 levels) in ~1000-1400 px, 86% concentrated in the user's flagged ROIs; background + parked camera stable.
2. Raw pre-composite GI diffuse carries the per-pixel variance; GI specular = 0; brick-SH (irradiance) and radiance-cache views fully black => has_world_probe=false for every character pixel.
3. Code mapping: two reset gates identified; MV convention + SDF round-robin re-bake exonerated.
4. Decisive dump of the FFX reset flag + disocclusion mask: reset flag covers 98.8-100% of hot pixels in limb boxes, disocclusion 0/16 at hot pixels.

Primary fix candidates: (a) relax the `weight_sum < 1e-3` reset gate (or the power-8 probe weighting) for pixels with no world probe; (b) give the character a world probe (voxelize character into the SDF as a dynamic cascade). Secondary: disocclusion-mask edge rejections produce the rim contours; TAA/FSR shape the final 2-state amplitude.

Caveats carried forward: per-frame auto-normalized GI jpgs (use log float ranges for absolute radiance); burst runs hitch ~2.2 fps under readbacks; one weather particle unavoidable (clamp >=1).
