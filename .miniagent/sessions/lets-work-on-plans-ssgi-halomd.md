# Scaffold session

- Task: lets work on plans/ssgi-halo.md
- Date: Tue Sep  1 08:40:38 2026
- Outcome: no clean sign-off
- Last verifier verdict: PASS

## Final report

Scaffold finished without a clean sign-off (last verifier verdict PASS). Final worker report:



## Task result

done — closed the three pending tasks in the best verifiable way: Task 6's delta-mix A/B was completed and **reverted per its decision rule** (the +8.5 R bump did not move — the delta form is algebraically identical to the original mix, which the measurements confirm); Phase 3 (Task 4) and Phase 4 (Task 5) verification ran against a fresh capture battery at the parked vantage, and the `VulkanGiPass.h` doc block now carries the corrected knob semantics.

## Files changed

- `c-engine/data/pak_0_engine/shaders/pass/scene/scene.frag` — restored byte-identical to `/tmp/scene.frag.pre_task6` (removed the previous round's un-decided delta-mix; rebuilt pak 08:36). Repo is back to the accepted SSGI-look state (E1+E3 `gi_estimate.comp` + original IBL mix).
- `c-engine/renderer/vulkan/pass/gi/VulkanGiPass.h` — env-knob doc block updated: `ENGINE_GI_AO_SCALE` documented as debug A/B only with the dead "0.5× CACAO override" premise recorded (AO field ≈1.0 in ON and OFF; no-op at the measured vantage; unmeasured at other vantages); `ENGINE_GI_INTENSITY` documented as the proven clean A/B knob (0.0 = exact GI-OFF image, ≤0.12/255).
- `.miniagent/ledger/notes.md` — appended `## final` with full metrics and file pointers.

## Findings

- **Task 6 verdict: delta-mix is a mathematical no-op.** Since `ambientDiffuse = kD·irradiance·baseColor/π·ibl`, `mix(ambientDiffuse, kD·gi.rgb, w)` equals `ambientDiffuse + w·kD·(gi.rgb − irradiance)` exactly — the "delta-mix" adds the deviation on top of the same baseline the mix was already blending from. Measured confirmation: `/tmp/out_t6.txt` M3 boxes identical to baseline (L1 +8.52 vs +8.5), M2/M1 unchanged, INTENSITY=0 still = GI-OFF (≤0.12/255). Decision rule (bump ≤3/255) failed → reverted; the ON−OFF whole-face ambient difference (+8.5 R left / −2.1 R right) is **accepted as a design difference, not a defect**. Crevice washout N/A; no crevice/eave corner is visible at the parked vantage (stated explicitly per the task).
- **Phase 3 (decode `/tmp/out_final.txt`):** above-head dip gone (value within 0.3% of far ref, conf 0.935); the old 2-px conf-0.67 silhouette band is now within the 4-px low-conf tolerance (wall strip x1326-1330 = 0.418, exactly 4 px). The two known sub-0.95 conf bands remain (left dip 0.652 at x1300-1330; right band 0.914-0.946 at x1540-1577) — unfixable by the Phase-1 ladder, fix would need the 2×2 origin-rule change. Eave strip and mid-wall hue boxes: no revival; their ON−OFF values track the face ambient bump exactly (eave R = REF_R; no hot seam, no cyan cast — old artifacts +22/+38/+31 and −3R/+6B are gone).
- **Phase 4 (`/tmp/out_sweeps.txt`):** M1 ring ≤2% of far wall; M2 ≤1.8/255; sweeps RAYS=4/16, TEMPORAL=0, INTENSITY=0.5/2 all ring-flat (M2 max 2.01/255); TEMPORAL=0-vs-default (mean 1.57/255) is pre-existing (measured on the baseline shader in round 2) and reported as reference only; `run.sh log 5000` had 0 error/invalid/assert/fail lines and clean exit; full build warning-free.
- Parked player/camera untouched; all run-to-run deltas confirm the vantage is intact (final vs baseline: image mean|d| 0.121/255, giRaw mean|d| 0.00235).
- All numbers are reproducible from `/tmp`: `final_on/off.jpg` + `final_{r4,r16,t0,i05,i2,i0}.jpg`, `out_final.txt`, `out_sweeps.txt`, decode script `/tmp/halo_m123.py`.

## Remaining steps

- (Out of plan scope) The two sub-0.95 conf bands would need the 2×2 origin-rule / thin-silhouette-gate change in `gi_estimate.comp` (plan Phase 3 hypothesis 3) plus its own A/B round — never attempted.
- (User-driven, per AGENTS.md) orbit check at the house to confirm no halo trails and no eave flicker as silhouette angle changes; parked transform must not be moved.
- Note for the manager: `tasks.json` still lists Tasks 4/5/6 as pending; their results are fully recorded in `notes.md` under `## final` — I did not touch `tasks.json` since only the manager owns it.
[usage: ↑1945009 ↓321 ...

Verifier:



## Verdict

PASS

## Commands run

- `./scripts/build.sh` — exit 0 (ninja: no work to do; engine/game shaders, data, Blender exports, and navmesh all up to date)

## First failure (if FAIL)

None.

## Notes

Build is a no-op up-to-date pass, so this confirms the current tree is in a buildable state but did not re-exercise compilation or shader recompilation. If the recent change should force a recompile (e.g. timestamps were preserved by a copy), a clean rebuild or `touch` of the changed files would be a stronger check. No runtime/screenshot verification is part of the fixed Verification line, so the M1–M4 halo measurements themselves remain the outstanding sign-off evidence.
[usage: ↑14985 ↓461 tokens]
