# notes

## brainstorm

Core difficulty: the task bundles (1) a design question — "should diffuse color-bleed between player/house/terrain be visible at all?" — and (2) a debugging question — "is the GI pipeline actually producing non-zero output the composite consumes?" The composite consumer (`VulkanCompositePass.cpp` + `composite.comp`) looks correct on inspection; the real constraint is upstream, in *what geometry the SDF clipmap is fed*.

Key lemmas:
- **L1:** SDF atlas is `VK_FORMAT_R8_UNORM` — signed distance only, no albedo. GI tracer samples environment (sky) on miss, per-brick **radiance cache** on hit (`FfxBrixelizerGISampleRadianceCacheSH`, `ffx_brixelizergi_main.h:977`).
- **L2:** Radiance cache is seeded from previous frame's lit composite (`FfxBrixelizerGIEmitPrimaryRayRadiance`, `ffx_brixelizergi_radiance_cache_update.h`); color injected only where a valid brick exists. Non-voxelized surfaces contribute no color.
- **L3:** The SDF clipmap is fed ONLY by per-tile vegetation props: voxelizer subscribes to `vulkanAzgaarPropsSetMeshCallback`/`SetTileCallback`; `tileCallback` fires only from `vulkanAzgaarPropsSetTile`/`ClearTile` (VulkanAzgaarPropsPass.cpp ~468/485). `SetGlobal` (settlement buildings = houses) and `SetLandmarks` go through `enqueueGlobal` which does NOT fire the callback. Player + terrain are not props. **=> player, house, terrain likely NOT in the SDF** (confirm the tested house is a settlement building, not a per-tile prop).
- **L4:** Composite does consume GI weighted by albedo: `composite.comp` step 7b `composite += giD * albedo * (1-metallic) * giDiffuseFactor` + specular; sky pre-scaled by `BRIX_GI_ENV_INTENSITY = 0.1` inside tracer. General ambient lift (sky + vegetation bounce) SHOULD land on player/house/terrain even though they don't participate in interreflection.

Net reduction: no color bleeding between player/house/terrain is *expected by construction* for interreflection (not in SDF). What still needs checking: whether the general GI lift (sky×0.1 + vegetation bounce) is even visible, i.e. pipeline alive + non-zero + actually consumed.

Candidate approaches:
- **A.** Read-only "alive + what's in the SDF" diagnosis: logs (`GI context created`, `brixelizer: ... bricksInUse=... instances=... tiles=...`), DebugGui panel, raw GI dump via `ENGINE_BRIX_GI_SAVE=1 ENGINE_BRIX_GI_SAVE_EVERY=1` -> `/tmp/brix_gi_diffuse_*.jpg`.
- **B.** Composite A/B: screenshot default vs `ENGINE_BRIX_GI_DIFFUSE_FACTOR=0 ENGINE_BRIX_GI_SPECULAR_FACTOR=0`, diff. Identical => composite not consuming GI; different => consumption works, issue is scale/contents.
- **C.** Feed more geometry into voxelizer (SetGlobal/SetLandmarks/terrain). Large, invasive; instance cap `FFX_BRIXELIZER_MAX_INSTANCES`, brick budget `1<<14` would be strained. Only after A/B prove pipeline healthy; alters visible output — needs explicit go-ahead.
- **D.** Hunt data-path bug in GI dispatch (layout handoff, `motionVectorScale` reprojection sign/scale, normals/depth conventions) — only if A/B show zero/noisy GI.

Recommended: A then B (cheap, decisive), then branch: consumed + non-zero => answer is "by design (SDF-only-vegetation), lift subtle at envIntensity 0.1" + decide raise factor or pursue C; not consumed or zero/noisy => escalate to D.

## round 1

Task 1 (worker, read-only): SDF contents confirmed.
- One-line: SDF clipmap contains only per-tile vegetation/prop instances; buildings (houses), landmarks, terrain, player are NOT voxelized.
- Code: voxelizer subscribes only to props tile callback (VulkanBrixelizerPass.cpp:173-174); tileCallback fires only from SetTile/ClearTile (VulkanAzgaarPropsPass.cpp:464-484); SetGlobal/SetLandmarks via enqueueGlobal never touches tileCallback. House species (HUT/HOUSE/TOWER/WALL/TEMPLE/DOCK/GATE, ids 13-19) have biome scatter density 0 and are registered via azgaarPropsRegisterGlobal -> SetGlobal (AzgaarProps.cpp:~1879, 2825). => house under test is in SetGlobal slot, NOT in SDF.
- Log evidence (build/c-game/data/game.log):
  - azgaarSettlements: uploaded 21211 building instances in 7 species ranges
  - zoneGui: spawned in settlement 'Kalcos' - Troyralia
  - vulkanBrixelizerPass: GI context created (2880x1627, 50% internal, DEPTH_INVERTED)
  - registered tile (-1,-2) stamp=1 — 22471 instances (1 tile)
  - brixelizer f=30: bricksInUse=9276 freeBricks=252868 (total=262144) instances=22471 tiles=1 (stable at f=150)
- Pipeline alive. Caveat: GI lift on player/house/terrain comes only from sky env (x0.1) + vegetation bounce; interreflection between those three surfaces impossible by construction.
- Remaining steps: none.

## round 2

Task 2 (worker, read-only): raw GI dump via ENGINE_BRIX_GI_SAVE=1.
- Run clean; 10 saved frames (/tmp/brix_gi_diffuse_0..9.jpg, _specular_0..9.jpg).
- Diffuse: frame 0 all-black (warmup); frames 1-9 min=0 max=255, 98.4-99.4% non-zero; per-channel mean ~63-127/255; colorful (42% px channel spread >30; B>G>R = sky env tint x0.1 + green veg bounce).
- Specular: identically zero in every frame — plausible (SDF only holds diffuse vegetation, no metallic/low-roughness geometry). Don't misread as "not consumed".
- Temporal: warmup churn frames 2-5 (expected seeding); settled frames 6-8 meanDiff 3.48/255, 1.2% px >30/255 => stable accumulated field, NOT reprojection/disocclusion noise.
- Bottom line: GI pipeline alive with full-frame stable colorful diffuse content at scale that *should* be visible after *albedo*(1-metallic) in composite. Task 3 A/B is the decisive probe.
- Remaining steps: none.

## round 3

Task 3 (worker, read-only): composite A/B at parked vantage.
- Artifacts: /tmp/gi_on.jpg, /tmp/gi_off.jpg (ENGINE_BRIX_GI_DIFFUSE_FACTOR=0 SPECULAR=0), gi_diff_x6.jpg; captured ENGINE_HIDE_GUI=1, 6s delay (past warmup).
- Whole frame: mean abs diff 10.6/255; signed mean (on-off) +9.0/255 (+8.4%); 92.6% px brighter with GI on (additive lift, as composite.comp predicts).
- Regions (on-off): house wall +10.9/255 (+9.7%), player +9.0 (+11.6%), grass ground +10.5 (+14.8%).
- Lift is near-uniform hue (sky-env x0.1 + green veg bounce), NOT directional color-bleed between surfaces — consistent with SDF-holds-only-vegetation.
- Caveat: ~30s apart runs => some temporal variance in diff (sky band, grass speckle) — excluded from static-surface stats.
- Implication: pipeline end-to-end healthy and consumed; complaint best explained as expected-by-design (ambient lift exists ~+10/255, subtle; true interreflection between player/house/terrain impossible since only vegetation voxelized).
- Remaining steps: none.

## round 4

Task 4 (worker, read-only): branch decision.
- VERDICT: **not a defect — expected by design.** SDF holds only per-tile vegetation; GI ambient lift working and consumed but subtle.
- Answer to user: diffuse AMBIENT GI should and does reflect on player/house/terrain (~+10/255 = +4-15%, washed out at BRIX_GI_ENV_INTENSITY=0.1). True color BLEEDING between those surfaces should NOT work — none of them exist in the SDF, tracer can't see their color. That is exactly the complaint's signature; a content/feeding gap, not a pipeline bug.
- All defect candidates ruled out: SDF non-empty (9,276 bricks), GI non-zero + stable (no reprojection noise), composite provably consumes (A/B).
- Cross-checked composite.comp:156 `composite += giD * albedo*(1-metallic) * giDiffuseFactor`, layout transitions and albedo gating in VulkanCompositePass.cpp correct; env x0.1 at VulkanBrixelizerPass.cpp:69,1017.
- Paths to visible inter-surface bleed: (b) cosmetic knob ENGINE_BRIX_GI_DIFFUSE_FACTOR>1 / raise BRIX_GI_ENV_INTENSITY; (a) Approach C voxelize buildings/terrain — blocked pending user go-ahead (budget + max instances strain).
- Task 5 => skipped (no defect). Task 6 satisfiable with existing A/B artifacts or one confirmatory screenshot.
- Remaining steps: manager records verdict; user decides (accept / knob bump / Approach C workstream).

## round 5

Verifier (independent, for worker id 4 verdict): **PASS**.
- Log: GI context created 2880x1627; bricksInUse=9286 stable f=30..300; 21211 settlement building uploads.
- Code: voxelizer only on props tile callback; SetGlobal/SetLandmarks via enqueueGlobal never fire it; HOUSE/HUT biome scatter density 0.0f (AzgaarProps.cpp:297-298, 1879-1880); composite.comp:156 consumption line + factor env knobs + BRIX_GI_ENV_INTENSITY=0.1 confirmed.
- Independent A/B re-diff: mean abs 10.58/255, signed mean (on-off) +8.95/255, 94.5% px brighter with GI on => composite provably consumes GI. Raw GI dumps: frames 1-8 98.4-99.4% non-zero, B>G>R; specular all-black (structurally expected).
- Both halves of verdict hold: ambient lift exists + consumed; inter-surface color bleed impossible by construction. No defect.
- Notes: A/B ~60s apart => some temporal variance; conservative bias, no threat to conclusion.

## sign-off

Task complete. Final verdict = "not a defect; expected by design". Verifier PASS is the ground-truth confirmation. No code was changed anywhere in this run.
