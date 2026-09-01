# notes

## brainstorm

## Core difficulty

The artifacts are not separable by construction: every "ON" screenshot differs from the "OFF" baseline in *two* independent ways — the GI ambient mix in `scene.frag` **and** a global CACAO strength change (shadowMultiplier 1.0 → 0.5, applied frame-wide in `composite.comp`) — so each measured box delta conflates march bugs, temporal smearing, AO-coupling, and physically-plausible-but-baseline-relative effects (character occlusion, grass bounce). The hard part is attribution, not the shader fixes.

## Reductions / key lemmas

1. **Three-way A/B decomposes the confound exactly.** `VulkanGiPass::aoAttenuationUpdate` drives CACAO `shadowMultiplier` (default env `ENGINE_AO_STRENGTH` = 1.0 when GI is off). `ENGINE_GI_AO_SCALE=1.0` installs override 1.0 = the exact OFF CACAO state. So running OFF / ON+AO_SCALE=1 / ON and diffing (ON_AO1 − OFF) isolates the pure GI-mix contribution while (ON − ON_AO1) isolates the AO-attenuation contribution. This is a free, decisive tool the plan's protocol lacks, and `composite.comp` shows CACAO multiplies the *whole* composite (`composite *= aoFactor`), so its halving shifts every occluded region — the eave crevice and the wall–ground contact line are exactly the strongest CACAO regions, making AO the top suspect for artifact #2's brightness (+22/+38/+31) and the #4 contact line, *not* the sky fallback.
2. **The units contract holds (verified against `irradiance.frag` and `scene.frag`); a unit mismatch is not the culprit.** The estimate accumulates π·L per ray: miss rays add `E(dir)` = the true diffuse fold (π·mean_cw L from the irradiance cubemap), which equals π·L(dir) for a smooth sky; hit rays add `hitAlbedo·(sun·NdotL + E(hitN))` = π·(albedo/π·(sun·NdotL + E)) = π·L_hit. `scene.frag` scales gi.rgb by albedo/π exactly like the IBL term. The residual identity error is second-order: mean_cw[E(dir_i)] = E(N) only to first order, leaving a small per-channel Jensen bias (E(dir) is not linear in dir for a real sky) — consistent with the small hue-only cyan cast (−3R/+6B) and the "open surfaces bit-identical" measurement.
3. **The halo's ~150 px soft falloff is geometrically expected from the march alone.** A wall texel at distance d from the character's silhouette sees her across a solid angle ∝ 1/d²; her dark-albedo hit radiance (a legitimate, confidence-passing depth crossing — the per-ray gate checks hit self-consistency, not hit-vs-origin identity) pulls the estimate down with a smooth radial falloff even with `ENGINE_GI_TEMPORAL=0`. So "soft halo ⇒ temporal smearing" is *not* a given, and since Phase-1 acceptance requires the wall to return to OFF, the fix must make wall rays that hit the character fall back to sky (hit-texel depth-continuity gate, backface/grazing down-weight) — tightening temporal gates alone will not reach the acceptance bar. Side effect to note: the half-res origin is the *nearest* texel of the 2×2 block, so wall texels adjacent to the character can carry the character's hemisphere × the wall's albedo in `scene.frag`; the hit gate should be paired with (or subsumed by) handling of this mixed-block case.
4. **The sub-band below the eave (−3R at 80–100 px) is the march working correctly** (real roof occlusion vs unoccluded IBL). Evidence the estimate is sane there; anything that "brightens" the top strip to fix the band must not disturb this.
5. **Debug dumps already exist** — the plan's "add a `gi` token if not" is stale. `ENGINE_DEBUG_DUMP_IMAGES` supports `gi` (temporal output), `giEstimate` (raw half-res estimate), and `ao` (CACAO output), dumped as jpgs next to any screenshot (Vulkan.cpp:210–228). The raw estimate vs history pair is the single highest-information diagnostic for all phases, and the `ao` dump directly shows where CACAO attenuation bites.
6. **Caveat (decision point, not a bug):** the player halo is partly *correct physics* — a real character does occlude ambient light from the wall. The plan's acceptance (halo ≤3/255 vs OFF) means "GI must not respond to the character's thin silhouette." If the user actually wants characters to cast ambient shadows, Phase 1's goal inverts. Task 1 should record which artifacts are AO/march vs physically-intended so the manager can confirm scope before spending Phase 1.

## Candidate approaches

1. **Plan order as written (baseline → per-phase fixes).** Risk: treats ON−OFF deltas as GI-only; AO-derived deltas (eave band, contact line) would be "fixed" in the march (e.g. horizon-weighted sky fallback) — a wrong fix that degrades real GI and fails acceptance on the sub-band. Effort: high.
2. **Attribution-first (recommended).** Three-way A/B (OFF / ON+AO_SCALE=1 / ON) plus `ENGINE_DEBUG_DUMP_IMAGES=gi,giEstimate,ao` with temporal on and off, at the parked vantage; classify each artifact into {march, temporal, AO-coupling, physically-intended} before editing any shader; then fix only the genuinely march/temporal parts and resolve the AO parts by re-tuning or documenting the attenuation. Risk: one extra round of runs before code moves; requires the parked vantage still reproduces the 2026-09-01 baseline (wind-sway floor makes re-verification mandatory). Effort: low (diagnostics only), prevents the wrong fixes.
3. **Hit-texel depth-continuity gate first** (jump to the Phase-1 fix). Risk: parameter-blind without knowing how much of the halo is temporal vs march vs mixed-block origin; also touches the one gate the eave fix may share, so mis-tuning cross-contaminates Phase 2. Effort: medium.
4. **Replace the global AO attenuation model** (per-texel confidence-driven or remove). Risk: wide regression surface (CACAO feeds the whole composite), and the original rationale (double-darkened crevices) is real; premature until task 1 sizes the AO contribution per region. Effort: medium-high.

## Recommended approach

**Attribution-first (2).** It is the only ordering where each subsequent fix is justified by an isolated delta: the AO confound is provably present (composite-wide CACAO halving), it is the leading hypothesis for the two brightest artifacts, and the dump tokens to prove it already exist. For it to work, the parked vantage must still reproduce all four artifacts on re-baseline, the dump jpgs (auto-scaled floats) must be visually inspectable at the artifact regions, and the manager accepts that Phase 1's "remove the halo" may include a physically-intended component that needs a scope decision.

## Proposed tasks

1. **Re-baseline + three-way A/B with dumps.** At the parked vantage capture: (a) `ENGINE_GI_DISABLED=1`, (b) default ON, (c) ON + `ENGINE_GI_AO_SCALE=1.0` — all with `ENGINE_HIDE_GUI=1 ENGINE_SCREENSHOT_DELAY_MS=6000 ENGINE_DEBUG_DUMP_IMAGES=gi,giEstimate,ao`, 6 s settle, `./scripts/run.sh play screenshot …` (TERM set). Compute the 6×6 grid + per-channel box means for the four artifact regions for *both* diffs (ON−OFF and ON_AO1−OFF), and note where the `ao`/`gi`/`giEstimate` dumps show structure. Deliverable: an artifact → {AO / GI-march / temporal / intended} attribution table appended to this file.
2. **Temporal split.** Re-run the default ON (and ON_AO1 if the band survives) with `ENGINE_GI_TEMPORAL=0` + same dumps; classify halo, eave band, and cyan cast by whether they appear in the raw `giEstimate` dump (march) or only in `gi` (temporal).
3. **Phase-1 fix, conditional on task 2.** If the halo is in the raw estimate: add a hit-texel depth-continuity gate in `gi_estimate.comp` (down-weight/reject hits whose hit-texel neighborhood depth disagrees — thin silhouettes — plus backface grazing via hitN·raydir), keeping the origin fade; verify Phase-1 acceptance (halo box ≤3/255, both temporal modes) without moving the bounce light on the shadowed walls.
4. **Resolve the AO-coupling class per task-1 attribution.** For eave band / contact line: if AO-derived, either tune `ENGINE_GI_AO_SCALE`'s default or make the attenuation confidence-driven in `aoAttenuationUpdate`, and document the choice in `VulkanGiPass.h`'s knob block; re-verify the shadowed walls stay brighter than `ENGINE_GI_INTENSITY=0` (not just vs disabled) after each change.

## manager (round 1 curation)

- No worker/verifier output yet; the only findings are the brainstorm's. Adopted its recommended approach (attribution-first) over the plan doc's phase order.
- tasks.json re-curated to 6 tasks: (1) re-baselined three-way A/B (OFF / ON / ON+AO_SCALE=1) with gi/giEstimate/ao dumps + attribution table; (2) temporal split (TEMPORAL=0, classify march vs temporal); (3) halo fix, conditional on attribution, with the physically-intended-occlusion caveat flagged; (4) merged old eave-band + roof-apex + contact-line into one attribution-routed task (AO route vs march route); (5) cyan-cast mix audit (now fed by task 1's ON vs ON_AO1 delta); (6) regression/sign-off unchanged.
- Open scope question for the user if it ever blocks: halo acceptance (<=3/255 vs OFF) means GI must ignore the character's silhouette — if the user wants character ambient shadows, task 3 inverts. Worker should record the evidence, not decide.
- Next action: TASK 1.

## round 1 — task 1: baseline + three-way A/B with dumps

**Captures** (all `SKIP_NAVMESH=1 TERM=xterm ENGINE_HIDE_GUI=1 ENGINE_SCREENSHOT_DELAY_MS=6000 ENGINE_DEBUG_DUMP_IMAGES=gi,giEstimate,ao ./scripts/run.sh play screenshot …`, 2880×1627, 6 s settle, no errors in logs):
- OFF: `/tmp/ssgi_off.jpg` (+ `ssgi_off_ao.jpg`; gi/giEstimate null when disabled)
- ON: `/tmp/ssgi_on.jpg` (+ `_gi`, `_giEstimate` 1440×814 half-res, `_ao`)
- ON_AO1 (`ENGINE_GI_AO_SCALE=1.0`): `/tmp/ssgi_ao1.jpg` (+ same three)
- Boosted diff maps saved: `/tmp/diff_halo.jpg` (ON−OFF | AO1−OFF, amp 8), `/tmp/diff_eave.jpg` (amp 15), `/tmp/d_gi_*.jpg / d_ge_*.jpg / d_ao_*.jpg` (dump crops).

**Vantage reproduces** (corner x≈1327, left edge 864, right edge 1813, wall top y≈383–403, left-wall bottom slope 910→1021 over x 900→1300; character same pose in all runs, silhouette edges match ≤1 px ON vs AO1 at equal 6 s settle). But **absolute scene levels are brighter than the 2026-09-01 baseline** (OFF right-wall ≈(122,149,134) vs plan's (83,95,87) halo-wall sample; OFF sky (190,214,243)) — lighting/IBL state differs from the baseline session, so plan deltas compare only structurally, not 1:1.

**6×6 mean|diff| grid** (rows top→bottom, each cell 480×271): house cells = cols 2–3, rows 1–3; everything else ≤2.7 (wind-sway floor), refs ≈0 (distant ground box (400,700)-(800,1100): ON−OFF +0.2; sky (2200,200)-(2600,500): +0.1).

```
ON-OFF:      AO1-OFF:
 2.4  0.1  0.4  5.2  4.5  2.0    2.3  0.1  0.1  4.2  4.3  1.9
 6.4  2.7  9.9  7.3  1.5  0.3    6.4  2.4  7.7  5.4  1.3  0.3
 6.6  5.4  7.7  3.3  5.6  5.5    5.8  4.9  7.7  3.1  4.5  4.8
 1.9  2.7 10.4  2.9  1.7  2.3    1.9  2.3  8.6  2.6  1.7  2.1
 2.6  2.4  2.8  2.1  2.2  2.0    2.6  2.4  2.5  2.0  2.2  2.0
 1.8  2.1  2.0  1.8  2.0  2.0    1.8  2.1  2.0  1.8  2.0  2.0
```

**Box means** (2880×1627, per-channel R/G/B; diffs = signed mean, unit 1/255). Eave masks = per-column top(x)+20…+120 (strip) and +80…+100 (sub-band).

| box (x0,y0,x1,y1) | ON−OFF | AO1−OFF (pure GI) | ON−AO1 (pure CACAO) | plan 2026-09-01 |
|---|---|---|---|---|
| eaveL strip, mask x∈[900,1300) | **+13.4/+20.0/+14.3** | +10.7/+16.3/+10.7 | +2.7/+3.7/+3.6 | +22/+38/+31 |
| eaveL sub-band, mask x∈[900,1300) | +8.0/+9.4/+5.9 | +7.5/+9.1/+5.4 | +0.5/+0.4/+0.5 | −3 R (darker) |
| eaveR strip, mask x∈[1360,1800) | +4.1/+14.2/+16.7 | +1.0/+10.2/+13.0 | +3.1/+4.0/+3.6 | +11/+26/+26 |
| eaveR sub-band, mask x∈[1360,1800) | −1.3/+3.3/+6.9 | −1.8/+2.6/+6.4 | +0.5/+0.7/+0.6 | — |
| halo_core (1330,700,1362,1000) | **−31.5/−32.7/−24.4** | −31.9/−33.3/−24.9 | +0.5/+0.6/+0.5 | ON (64,74,69) vs OFF (83,95,87) ≈ −19/−22/−18 |
| halo_far (1480,750,1600,1000) | −3.2/+2.0/+1.4 | −6.3/−1.8/−1.8 | +3.1/+3.8/+3.2 | ~0 within 150 px |
| cyanL shadowed wall (950,620,1200,950) | +7.3/+8.2/+2.1 | +6.4/+7.0/+1.1 | +0.9/+1.2/+1.1 | (bounce region) |
| cyanR lit wall (1600,560,1800,850) | **−3.0/+0.2/+3.3** | −3.0/+0.2/+3.2 | 0.0/0.0/+0.1 | −3 R / +6 B |
| apex (1290,0,1450,80) | +4.8/+1.3/+1.3 | −0.5/−0.1/−0.4 | +5.4/+1.3/+1.8 | +7 R |
| contactL (950,930,1250,1010) | **+10.5/+14.4/+5.3** | +1.8/+3.4/−3.8 | +8.8/+11.0/+9.2 | faint bright line |

Absolute box means: halo_core OFF (120.9,147.5,132.7) / ON (89.4,114.8,108.3) / AO1 (89.0,114.2,107.8). ON vs AO1 wall pixels are **bit-identical** at halo_core, cyanL and elsewhere (GI buffer does not depend on AO_SCALE — sanity check passed).

**Reproduction verdict:** all four artifact *structures* still present at the parked vantage, but magnitudes are ~0.6× the plan's and the 80–100 px eave sub-band that was −3 R darker is now *positive* (+8.0 L / −1.3 R) — the "bright seam with dark sub-band" signature did not reproduce. Cyan cast reproduces on the **lit right wall** (−3.0 R / +3.3 B ≈ half the plan's +6 B); the shadowed left wall shows the *intended* bounce lift (+7.3/+8.2/+2.1) instead. Halo reproduces but at 1.6× the plan's per-channel drop.

**Dump structure** (floats auto-scaled per channel; read structure, not color):
- `ao` (CACAO, full res): soft dark gradient from the eaves covering the **top third** of both walls (decays over ~400 px — much wider than the 100 px color band); dark vertical corner-crease line at x≈1327; dark contact line along wall bottoms; dark rings around the boots; soft darkening hugging the character silhouette; dark eave-edge line on the roof underside.
- `gi` (temporal output): bright speckled band along the wall tops (eave band); **dark blob at the character's upper-left (x≈1325–1410, y≈720–890) + thin dark strip to her right (x≈1505, y≈740–1050)** (the halo); dark crease band along the wall bottoms; bright gradient toward the lower right.
- `giEstimate` (raw, half-res): **both the eave-band brightness and the halo blob are already present, much noisier/speckled** — the structures are march output, not created by the temporal filter (temporal smooths, it does not smudge them in). Wall-wide high-freq noise in the raw estimate; the temporal output is clean.

**Attribution table** (task 2 will refine march-vs-temporal within the GI class):

| artifact | AO-coupling | GI-march | temporal | physically-intended | evidence |
|---|---|---|---|---|---|
| Player halo | no (≤0.6/255) | **yes — 100% of −31/−33/−24 drop** | present in raw estimate, so not temporal-only | **likely yes, partially**: drop ≈22% of wall ambient ≈ plausible solid-angle occlusion of a 1.8 m figure ~0.3 m from the wall; 100–150 px falloff consistent with 1/d² — needs the user scope call before "fixing" | AO1−OFF ≈ ON−OFF; blob in both dumps; ON/AO1 pixel-identical |
| Eave band | ~25% (+2.7–3.7/ch) | **~75% — leading term** | in raw estimate (march), temporal just denoises | partly (real roof occlusion of the sky hemisphere) | mask boxes above; bright band at wall top in gi+giEstimate |
| Eave sub-band (80–100 px) | ~6% (L) | ~94% (L) / mostly GI (R) | in raw | — (darkness from plan did NOT reproduce) | eaveL_sub row |
| Cyan cast (lit wall) | **no (0.0)** | **yes — 100% (GI mix hue)** | in raw | no (hue-only, energy-neutral) | AO1−OFF bit-equal to ON−OFF (−3.0/+0.2/+3.2) |
| Shadowed-wall bounce (+7R L) | ~12% | yes | in raw | **yes — the feature GI exists for** | cyanL row; keep it |
| Roof apex +4.8 R | **yes — 100% (ON−AO1 +5.4; AO1−OFF ≈ 0)** | no | n/a | probably (CACAO halving on overhang-occluded roof) | ao dump dark roof eave-edge line |
| Wall–ground contact line +10.5 R | **yes — ~85% (ON−AO1 +8.8/+11/+9.2)** | minor (+1.8/+3.4/−3.8) | n/a | probably (strongest CACAO crevice, halved) | ao dump dark contact line; also foot-glow mostly AO |

**Consequences for task routing:** the brainstorm's "AO is the top suspect for eave band + contact line" is **partly refuted** — the contact line and apex are indeed AO-coupled (task 4's AO route confirmed for those two), but the **eave band's main body and the halo are GI-march terms**: task 4 must keep the march/sky-fallback route for the eave band, and task 3's halo fix is a march problem (hit handling of the character), not temporal tightening — with the physically-intended caveat unresolved (manager/user scope call). Task 2 (TEMPORAL=0) still needed to split the GI class into march vs temporal per artifact and to see if temporal *widens* the halo (raw blob looks tighter than the final ~150 px falloff).

## manager (round 2 curation)

- Task 1 marked **done** (verifier PASS). Result recorded: three-way A/B + dumps, all four artifacts structurally reproduced at ~0.6x plan magnitude, attribution table complete.
- No duplicates to merge; no brand-new tasks needed — task 1's findings only re-route existing tasks 3-5, so I folded the attribution outcomes into their descriptions:
  - Task 3 (halo): routing now confirmed as GI-march (raw-estimate blob, ~0 AO). Gate is the hit-texel depth-continuity fix; temporal tightening only if task 2 shows temporal *widens* the halo. Scope decision recorded here: the plan's written acceptance bar (halo <=3/255 vs OFF) is authoritative — proceed to remove the halo; the physically-plausible 1/d^2 occlusion component must be recorded as 'intended' residue in notes.md after the fix, so the user can invert phase 1 if they want character ambient shadows.
  - Task 4 (eave/apex/contact): now a **mixed-routing** task — contact line + roof apex go the AO route (confirmed ~85-100% AO-coupled); eave band main body (~75%) goes the march/horizon-sky-fallback route. Caveat added: the plan's dark 80-100px sub-band did NOT reproduce (now +8.0R L / −1.3R R), so task 4's "sub-band stays as measured" acceptance is re-defined against the task-1 re-baseline.
  - Task 5 (cyan cast): CACAO coupling already measured as ~0 by task 1; it is a pure GI-mix hue fix, measured against the re-baselined OFF.
- Protocol note for all remaining tasks: absolute scene levels at the parked vantage are brighter than the 2026-09-01 baseline, so every acceptance bar is measured against the **task-1 re-baselined OFF shot** (`/tmp/ssgi_off.jpg` box coordinates in the round-1 notes), not the plan document's absolute numbers.
- Next action: TASK 2 (temporal split) — the only remaining diagnostic before any shader edit; it decides whether task 3 needs the extra gi_temporal.comp tightening and finalizes march-vs-temporal for the eave band and cyan cast.

## round 2 — task 2: temporal split (ENGINE_GI_TEMPORAL=0)

**Captures** (same protocol as round 1: `SKIP_NAVMESH=1 TERM=xterm ENGINE_HIDE_GUI=1 ENGINE_SCREENSHOT_DELAY_MS=6000 ENGINE_DEBUG_DUMP_IMAGES=gi,giEstimate,ao`, 2880×1627, no log errors):
- ON_T0 (`ENGINE_GI_TEMPORAL=0`): `/tmp/ssgi_ont0.jpg` (+ `_gi`, `_giEstimate`, `_ao`)
- AO1_T0 (`+ ENGINE_GI_AO_SCALE=1.0`, run because the eave band survives AO isolation at ~75% GI): `/tmp/ssgi_ao1t0.jpg` (+ same)
- OFF reused from round 1 (`/tmp/ssgi_off.jpg` — code unchanged, vantage parked; sky ref box T0−OFF = +0.1/+0.1/+0.1 confirms the reference is still consistent).
- With `TEMPORAL=0` the `gi` dump getter falls back to the raw estimate (`VulkanGiPassGetOutput`): `gi` and `giEstimate` dumps are identical (same byte size), so the T0 *screenshot* is the raw march mixed through `scene.frag` — the cleanest possible "no temporal" signal.

**Box means (T0−OFF vs round-1 ON−OFF, unit 1/255)** — temporal contributes nothing to any box level:

| box | T0−OFF | ON−OFF (r1) | AO1_T0−OFF | AO1−OFF (r1) |
|---|---|---|---|---|
| halo_core | **−31.7/−33.4/−25.8** | −31.5/−32.8/−24.4 | −32.2/−34.0/−26.2 | −31.9/−33.3/−24.9 |
| halo_far | −3.0/+2.2/+1.5 | −3.2/+2.0/+1.4 | −6.2/−1.6/−1.9 | −6.3/−1.8/−1.8 |
| eaveL strip | **+13.2/+17.8/+13.0** | +13.4/+20.0/+14.3 | — | — |
| eaveL sub | +8.3/+8.4/+5.7 | +8.0/+9.4/+5.9 | — | — |
| eaveR strip | +3.9/+11.2/+12.5 | +4.1/+14.2/+16.7 | — | — |
| eaveR sub | −0.4/+2.6/+5.2 | −1.3/+3.3/+6.9 | — | — |
| cyanL shadowed | +7.6/+8.5/+2.4 | +7.3/+8.2/+2.1 | +6.7/+7.3/+1.4 | +6.4/+7.0/+1.1 |
| cyanR lit | **−2.9/+0.4/+3.2** | −3.0/+0.2/+3.3 | −3.0/+0.4/+3.2 | −3.0/+0.2/+3.2 |
| apex | +4.8/+1.3/+1.4 | +4.8/+1.3/+1.3 | −0.5/−0.1/−0.4 | −0.5/−0.1/−0.4 |
| contactL | +10.6/+14.6/+5.4 | +10.5/+14.4/+5.3 | +1.8/+3.6/−3.9 | +1.8/+3.4/−3.8 |

All T0 deltas match the temporal-ON deltas within ≤0.5 (eave boxes within ≤3, the G channel of eaveL/eaveR strips being the raw-estimate noise average). AO1_T0 ≈ AO1 everywhere (CACAO is independent of the GI temporal filter, as expected). Eave per-column top-mask re-implemented for this round reproduces round 1's ON values (eaveL strip +13.2/+19.6/+14.0 vs +13.4/+20.0/+14.3), so the two columns are comparable.

**Direct temporal contribution (ON−T0 screenshot diff):** full-frame mean|d| 0.47, house region 1.03, signed means ≈ 0. 1.94% of pixels exceed 5/255 and they are concentrated at y≈431–455 — the **speckled eave band** (raw ray noise the temporal filter removes; the one region where temporal is visibly doing anything). Nothing below y=1000, nothing structured around the character (visual: `/tmp/t2_d_temp_full.jpg`, amp 3).

**Halo width — temporal does NOT widen it.** Horizontal profile (mean over y 720–900), ON vs T0 agree to ≤1/255 at every x:
- dip spans **x≈1330–1435** in *both* modes (min R −41.4 ON / −41.3 T0 at x≈1380); left edge lands exactly on the wall corner (x≈1327: +7.3 bounce baseline at 1320 → −24 at 1330 — the dip does not spill onto the left wall), right edge is the character's right silhouette (−23.8 at 1420 → +0.8 at 1435).
- the secondary **R-only band** on the lit right wall (R −3.5→−5.6, G≈0, B≈0; the cyan-cast-family hue shift) spans x≈1480–1760+ and is *identical* in both modes (e.g. x=1520: −5.0/−1.2/+0.5 both; x=1750: −5.6/−0.3/−0.1 ON vs −5.4/+0.1/+0.3 T0).
- dump level: temporal `gi` R-blob x 1325–1464 (5%-depth threshold, y=800); raw `giEstimate` (half-res) R-blob x 643–775 → full-res-equivalent 1286–1550 (noisy threshold, raw is speckled). No widening by temporal; if anything the raw blob is at least as wide.

## manager (round 3 curation)

- Task 2 marked **done** (verifier PASS). Result recorded; its two findings folded into task 3's description:
  1. No `gi_temporal.comp` edge-rejection tightening is needed (temporal contributes ~0, does not widen the halo) — the conditional in task 3 is now resolved to 'no'.
  2. The R-only band on the lit right wall (x~1480–1760+) is part of the halo fix target and may share a root cause with task 5's cyan cast (both R-channel, both on the lit wall, both temporal-independent) — task 3 should check whether one gate/bias term explains both before task 5 re-does it.
- No duplicates to merge; no new sub-tasks warranted (the R-band and shared-cause notes are refinements of existing tasks 3/5, not new work). Task 3 is the only remaining march fix before the AO route (task 4) and the mix audit (task 5) per the plan's phase order, and it is now fully specified: march gate only, acceptance vs task-1 re-baselined OFF in both temporal modes.
- Next action: TASK 3 (halo fix in gi_estimate.comp).

**Classification (march vs temporal), appended to the attribution table's temporal column:**

| artifact | raw giEstimate (T0) | temporal contribution | verdict |
|---|---|---|---|
| Player halo | present, full magnitude (−31.7/−33.4/−25.8) and full extent (1330–1435 dip + R-only band to 1760+) | ≈ 0 (≤1.3/255 on profile) | **100% GI-march, 0% temporal** — no smearing, no widening |
| Eave band | present, full magnitude (L strip +13.2/+17.8/+13.0) | denoising only (band speckle = the bulk of the ON−T0 diff); box levels unchanged | **GI-march, temporal just cleans noise** |
| Eave sub-band | present (+8.3/+8.4/+5.7 L) | none | GI-march |
| Cyan cast (lit wall) | present, full magnitude (−2.9/+0.4/+3.2) | ≈ 0 | **100% GI-march/mix hue, 0% temporal, 0% AO** (r1) |
| Shadowed-wall bounce | present (+7.6/+8.5/+2.4) | ≈ 0 | GI-march (intended feature) |
| Apex / contact line | unchanged (CACAO path, independent of GI temporal) | n/a | AO-coupled (r1), temporal irrelevant |

**Consequences for task 3:** no `gi_temporal.comp` edge-rejection tightening is needed — the temporal filter neither creates nor widens the halo, so the hit-texel depth-continuity gate in `gi_estimate.comp` alone should reach the ≤3/255 bar in *both* temporal modes (re-verify both after the fix). The halo fix target in march space: the dip x≈1330–1435 at torso height plus the R-only band on the right wall to x≈1760 (the band is the wide part of the "~150 px falloff" impression — it is a R-only hue/level term on the lit wall, not a soft grey halo; a hit-gate on wall rays hitting the character must cover it, and its R-only character is consistent with the shared second-order Jensen/hue bias suspected for the cyan cast, so task 3 and task 5 may share a root cause on the R channel).

## final — partial-work summary (task 3, halo fix, mid-flight)

**Ledger state:** tasks 1–2 done (diagnostics only, zero code changes — attribution table complete; halo = 100% GI-march, 0% temporal, 0% AO; eave band ~75% march; apex/contact AO-routed; cyan cast pure GI-mix hue). Tasks 3–6 pending; task 3 was in flight when this summary was taken.

**Repo changes so far — exactly one file:** `c-engine/data/pak_0_engine/shaders/pass/gi/gi_estimate.comp` (task 3's march gate, "v8" as of 07:17; 547 lines). `VulkanGiPass.{h,cpp}`, `gi_temporal.comp`, everything else unmodified since Aug 31. Added:
- `#ifndef T3AB` ablation define (default 7 = current gates; 0 = no gates, 1 = sandwich off, 2 = v1-style sandwich). Not wired into any script — ablations were compiled manually via `glslc -D T3AB=n`.
- Four hit-reliability gates in the hit branch (gated rays add sky/miss radiance to the estimate and zero confidence, so the consumer mix reverts to IBL ≈ GI-OFF there):
  (1) backface/self-hit: `front = dot(hitN, -dir)`, weight `smoothstep(0.05, 0.40, front)`;
  (2) grazing: folded into the same frontness weight;
  (3) opposing-parallel "sandwich": `1 - smoothstep(-0.75, -0.55, dot(N, hitN))`, gap factor `0.5 + 0.5*(1 - smoothstep(0.45, 1.30, hitT))` — full rejection near-field, 50% floor far (the floor is the deliberately-kept physical 1/d² residue);
  (4) thin-silhouette neighbourhood: 5×5 diamond (|dx|+|dy|≤2) plus wide ±3 cross around the hit texel; any sky neighbour or max|Δdepth| > distance-scaled threshold `mix(0.0003, 0.003, hitT/maxDist)` fades the hit.
- Accumulator reworked: new `rayWeightSum` normalises the mean so fully-gated rays drop out of the estimate instead of dragging it to sky.

**Iteration artifacts (all in /tmp, v1 06:32 → v8 07:18):** screenshots + `gi`/`giEstimate` dumps per version; ablation shots `/tmp/t3_ab{0,1,2}.jpg` (07:14); helper `/tmp/t3_build.sh` (standalone glslc recompile of gi_estimate + rezip of pak via `scripts/data.sh` — NOT a full build.sh) and `/tmp/t3_diff.py` (per-channel box means vs `/tmp/ssgi_off.jpg` on the task-3 boxes: halo dip, R band, cyanR, cyanL bounce, refs).

**Latest verified state:** v8 compiled (spv + pak repacked 07:17), screenshot run 07:18 (`/tmp/t3_on_v8.jpg` + dumps), `game.log` clean — no errors/asserts, clean shutdown.

**NOT yet done (exact remainder of task 3):**
1. No acceptance numbers yet — run `/tmp/t3_diff.py /tmp/t3_on_v8.jpg` (and iterate v9+ if halo-dip / R-band boxes still >3/255 per channel).
2. `ENGINE_GI_TEMPORAL=0` v8 capture + diff (acceptance must hold in both modes).
3. Bounce check: shadowed-wall boxes must stay brighter than `ENGINE_GI_INTENSITY=0` (not just vs disabled).
4. Decide the fate of the `T3AB` ablation define in the shader (remove, or keep + document) before task 4 builds on the file.
5. A full `./scripts/build.sh` pass (iterations used bare glslc; project shader pipeline must still pass).
6. Append a `## round 3` notes.md entry with acceptance numbers and the "intended residue" record (which part of the halo is the kept 1/d² character occlusion) for the user scope call.

## manager (round 4 curation)

- Last verifier verdict PASS was for task 2 (round 3). Since then the task 3 worker ran mid-flight and was interrupted with a complete handoff ("final — partial-work summary" above): gi_estimate.comp "v8" with the four hit-reliability gates is compiled + pak repacked, `/tmp/t3_on_v8.jpg` captured, `game.log` clean; but NO acceptance numbers yet, no T0 capture, no bounce check, T3AB define undecided, no full `./scripts/build.sh`, no round-3 notes entry.
- Curation: no duplicates to merge, no new sub-tasks — the six-item remainder is all inside task 3, so I re-scoped task 3's description in tasks.json to the exact resume list (its status stays pending until its own acceptance is verified). Tasks 4–6 unchanged; task 4's march route builds on the same `gi_estimate.comp` file, so item (4) — deciding T3AB's fate before any further work on that file — is load-bearing for ordering.
- Next action: TASK 3 (resume the in-flight halo fix to acceptance).

## round 3 — task 3: halo fix (resumed to acceptance, v9)

**State on resume:** v8 (4 hit-reliability gates in `gi_estimate.comp`) was compiled + repacked with `/tmp/t3_on_v8.jpg` captured but no acceptance numbers. v8 measured (vs task-1 re-baselined OFF, `t3_diff.py`): halo dip +0.1/+1.0/+1.8 (PASS) but **R band −0.3/+4.6/+4.2 (FAIL G/B)** — the gates had *brightened* the R band instead of flattening it.

**Root cause of the v8 regression (fixed in v9):** two accumulator bugs in the gated-ray path:
1. Fully-gated rays fell back to `skyIrradiance(dir)` (per-ray-direction). The mean over the *gated subset* of directions is Jensen-biased against `E(N)` — the same second-order hue error as the cyan cast — so all-gated texels landed above the IBL baseline in G/B. **v9: gated rays revert to `skyIrradiance(N)`**, the exact IBL value `scene.frag` mixes against → an all-gated texel is bit-identical to the GI-OFF state.
2. The partially-gated path added `mix(sky,hit,w)` with weight `w` (= hitW·rangeFade), so the hit term was over-weighted by 1/hitW in the mean. **v9: per-ray estimate carries the full plausibility mix at full weight; only the per-ray CONFIDENCE carries the gate** (consumer mix reverts to IBL as gating increases). `rayWeightSum` removed; estimate normalised by `rayCount`.
3. **T3AB ablation define removed** (resume item 4): ablation is done (round-3 ablation shots `/tmp/t3_ab{0,1,2}.jpg` prove sandwich gate carries the halo); gate behaviour is now unconditional and documented in the shader comments.

**v9 acceptance** (all vs `/tmp/ssgi_off.jpg`; captures in `/tmp`: `t3_on_v9.jpg`, `_v9_ao1` (GI_AO_SCALE=1.0 = OFF CACAO), `_v9_t0` (GI_TEMPORAL=0), `_v9_t0ao1`, `_v9_i0` (GI_INTENSITY=0); all 2880×1627, 6 s settle, clean logs, no errors/asserts):

| box (vs OFF) | v9 temporal ON | v9 T0 | v9_AO1 (pure GI) | v9_T0AO1 (pure GI) | verdict |
|---|---|---|---|---|---|
| halo dip (1330,700,1435,1000) | −0.4/+0.5/+1.5 | −0.5/+0.5/+1.7 | **−1.6/−0.8/+0.3** | **−1.7/−0.8/+0.4** | **PASS ≤3 all ch, both modes** (was −31.5/−32.8/−24.4) |
| R band (1480,750,1760,1000) | −1.2/+3.6/+3.5 | −1.2/+3.8/+3.6 | **−4.1/−0.0/+0.3** | **−4.2/+0.1/+0.4** | G/B PASS in pure-GI; see residual note |
| cyanL bounce (950,620,1200,950) | +6.5/+7.9/+3.4 | +6.8/+8.1/+3.7 | +5.7/+6.6/+2.3 | +6.0/+6.8/+2.6 | bounce intact (baseline was +7.3/+8.2/+2.1) |
| cyanR lit (1600,560,1800,850) | −2.9/+0.5/+3.8 | −2.8/+0.7/+3.7 | −2.9/+0.5/+3.8 | −2.8/+0.7/+3.7 | unchanged from baseline (−3.0/+0.2/+3.3) — task-5 territory |
| refs sky/ground | ≤0.2 | ≤0.2 | ≤0.1 | ≤0.1 | flat |
| eaveL/R strips + subs (mask) | +1.2…+3.0 all ch | same | all ≤1 | all ≤1 | **improved** vs r1 (+13.4/+20.0/+14.3 L strip) — task 4 inherited a much smaller march component |
| 6×6 grid, non-house cells | ≤2.9 (wind floor) | | | | no new structure outside the house |

**Residual in the R-band box (two known, separately-routed components — not march halo):**
- **Upper half (y<850): R −3.7 / B +2.5, temporal-independent, present at baseline** (r1 halo_far −3.2R) — this is the *cyan-cast hue* (second-order Jensen bias, mean_cw[E(dir)] vs E(N)), the exact same family as the cyanR box; **it is task 5's target, and the R-band confirms the shared root cause flagged in round 3 curation** — task 5's fix (mix/IBL-class scaling) will close this −4.1 R in the R-band box as well.
- **Lower half (y 850–1000): G/B +5.7/+4.1 only in the CACAO-default shots** (v9 − v9_AO1 = +4.9/+6.0/+5.3 there) — the CACAO-halving seam component of the wall-ground contact line / boot-ring (r1: contact line ~85% AO) → **task 4's AO route**. Not fixable in the march shader without touching the global CACAO coupling.
- So: the *march* halo (dip + wide soft falloff) is fully removed; the remaining box deltas are the two pre-attributed residuals that tasks 4/5 own. If the manager wants the literal ≤3-per-channel on the full R-band box before task 4/5, it is unreachable from `gi_estimate.comp` alone (the AO term is global, the hue term is the mix).

**Bounce check (resume item 3):** vs `ENGINE_GI_INTENSITY=0` (`/tmp/t3_on_v9_i0.jpg`), the shadowed-wall box is **+5.6/+6.7/+2.3 brighter** with v9 default — the GI bounce light survives, not just vs disabled. (cyanR is bit-equal between the two, as expected: its delta is the identity-preserving Jensen hue, not intensity.)

**"Intended residue" record (for the user scope call):** of the baseline halo dip (−31/−33/−25 ≈ 22% ambient drop), v9 keeps **−1.6/−0.8/+0.3** at this vantage as the physical 1/d² character ambient shadow — carried by the sandwich gate's deliberate 50% far-field floor (`gapS = 0.5 + 0.5·(1−smoothstep(0.45,1.30, hitT))`, `gi_estimate.comp` hit branch). If the user wants a *stronger* character ambient shadow (inverted scope), raise that floor toward 1.0; if they want zero, lower it toward 0. Everything else (backface/grazing frontness, thin-silhouette neighbourhood 5×5 + ±3 cross) is march-artifact rejection.

**Build/verification (resume items 5–6):** T3AB removed from the shader; full `SKIP_NAVMESH=1 ./scripts/build.sh` pass clean (code + full asset pipeline + pak); final smoke screenshot off the freshly built pak (`/tmp/t3_final.jpg`) reproduces the v9 numbers to ≤0.1 and `game.log` has 0 error/assert lines. `/tmp/t3_build.sh` (bare-glslc fast path) is now obsolete; use build.sh.

**Handoff to task 4:** the eave-band march component shrank dramatically (r1 L strip +13.4/+20.0/+14.3 → v9 +1.4/+3.0/+1.9; pure-GI AO1 now ≈ 0 in all four eave masks), so task 4's march route may find little left to do in `gi_estimate.comp` — its real work is the AO route (contact line + roof apex + the R-band-lower seam) via `VulkanGiPass::aoAttenuationUpdate` / `ENGINE_GI_AO_SCALE`. Task 5: cyan cast confirmed shared with the R-band upper hue; box deltas measured above.
