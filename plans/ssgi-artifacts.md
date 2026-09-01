# SSGI Visible Artifacts — Fix Plan

## Goal

Remove the three visible SSGI artifacts measured at the parked vantage
(house + player). The GI estimate itself (SSGI++ ray march, temporal
filter, injection) is Phase-validated per `plans/ssgi.md` — this plan is
about *quality of the estimate near geometry edges and dynamic
characters*, not a redesign.

## Baseline (measured 2026-09-01, this session)

A/B screenshots at the parked player (house, player standing at the wall
corner): `ENGINE_HIDE_GUI=1 ENGINE_SCREENSHOT_DELAY_MS=6000
./scripts/run.sh play screenshot …`, one run default (GI on), one with
`ENGINE_GI_DISABLED=1`. 2880×1627, 6 s settle so the temporal filter
converged. Reference pair: `/tmp/ssgi_on.jpg`, `/tmp/ssgi_off.jpg`,
6× difference map `/tmp/ssgi_diff6x.jpg`.

Artifacts, in order of visual severity:

1. **Player halo** — dark smudge on the wall around the character,
   soft radial fall-off. Wall around her: ON `(64,74,69)` vs OFF
   `(83,95,87)` (−18/−20/−17), fading to ~0 within ~150 px. In the
   OFF shot the wall behind her is perfectly flat.
2. **Eave band** — hot bright strip along the top ~100 px of both walls
   under the roof line, fading vertically. Top strip of left (shadowed)
   wall: +22 R / +38 G / +31 B; right wall: +11 R / +26 G / +26 B.
   The 80–100 px below the strip are actually slightly *darker* (−3 R),
   so it reads as a bright seam, not a uniform lift.
3. **Cyan cast** — uniform mid-wall shift of about −3 R / +6 B vs OFF,
   wall goes greener/cyaner.
4. Minor: +7 R at the roof apex against sky; faint bright line at the
   wall–ground contact; small green boost on lower wall.

Everything else (grass, sky, distant vegetation) is bit-identical or
within the wind-sway noise floor — the problem is localized to house
walls and the character.

## Files

- `c-engine/renderer/vulkan/pass/gi/VulkanGiPass.cpp/.h` — pass driver,
  env knobs (`ENGINE_GI_RAYS`, `_DIST_SCALE`, `_TEMPORAL`, `_TWEIGHT/
  _TDEPTH/_TCLAMP/_TFLOOR/_TDEV0/_TDEV1/_TLUMA`, `_INTENSITY`,
  `_AO_SCALE`), temporal push constants, AO attenuation override.
- `c-engine/data/pak_0_engine/shaders/pass/gi/gi_estimate.comp` —
  hemisphere ray march, hit radiance, sky fallback.
- `c-engine/data/pak_0_engine/shaders/pass/gi/gi_temporal.comp` —
  history reprojection / edge continuity / clamp.
- `c-engine/data/pak_0_engine/shaders/pass/scene/scene.frag` — GI mix
  into IBL diffuse (`sceneBuffer.gi`, `kD_ibl * gi.rgb * baseColor/PI`).
- `docs/global-illumination.md` — method background.

## Validation protocol (use for every step)

```bash
# ON
TERM=xterm ENGINE_HIDE_GUI=1 ENGINE_SCREENSHOT_DELAY_MS=6000 \
  ./scripts/run.sh play screenshot /tmp/ssgi_on.jpg
# OFF (baseline)
TERM=xterm ENGINE_GI_DISABLED=1 ENGINE_HIDE_GUI=1 ENGINE_SCREENSHOT_DELAY_MS=6000 \
  ./scripts/run.sh play screenshot /tmp/ssgi_off.jpg
```

Then the 6×6 mean-|diff| grid + targeted box means (PIL recipe used in
this session; the grid localizes, the boxes quantify sign per channel).
Pass bar: the three artifact regions above return to the wind-sway noise
floor (grid cells ≲2/255, box deltas ≲3/255 per channel) **while the
bounce-light effect that GI exists to provide is retained** — i.e. the
shadowed walls must still be brighter than the pure-shadow (no-ambient-
bounce) case; A/B against `ENGINE_GI_INTENSITY=0` (not just against
disabled) is the right "did we kill the feature" check.

Debug aids: `ENGINE_GI_TEMPORAL=0` (raw estimate vs filtered — separates
march bugs from temporal smearing), `ENGINE_GI_RAYS=…` (noise vs bias
scaling), `ENGINE_DEBUG_DUMP_IMAGES` tokens if a GI image dump token
exists (add `gi` alongside `velocity,depth,color,taa` if not).

## Phase 1 — Player halo (dark smudge on wall around character)

Hypotheses, in prior order:

1. **Hit radiance samples the character unoccluded-by-geometry.** A wall
   texel's hemisphere rays graze the character and treat her dark
   albedo as a legitimate bright-ish bounce surface. Check in
   `gi_estimate.comp` how hit radiance is built (shadowless-direct +
   sky) and whether the per-ray depth agreement gate tolerates the
   character's thin silhouette (a few-px-thick hit against a wall
   100+ px away passes coarse depth checks).
2. **Character normals/albedo in the GI G-buffer inputs.** If the
   character is drawn into the normals/albedo the march reads with a
   degenerate or view-facing normal, reconstructed-geometry rays fan out
   wrong and the dark albedo spreads. Confirm which image the pass
   consumes (`estimateDispatch(cmd, depth, normals, albedo, material)`)
   and whether the character writes sane normals there.
3. **Temporal smearing of the halo.** With `ENGINE_GI_TEMPORAL=0` the
   halo should be a crisp ~silhouette-sized shadow; if it is, the
   march is fine and `gi_temporal.comp` is bleeding it (edge
   continuity using only depth at a 100+ px depth delta *behind* the
   wall should already reject — check the `devStart/devEnd`
   ramp defaults 0.12/0.50 aren't being applied before the depth
   gate, or the luma clamp `_TLUMA` 0.15 is too loose).

Fix directions: depth-continuity gate on the *hit* texel (not just the
origin texel); angular/N·L falloff on hit radiance so near-silhouette
grazing hits don't dominate; exclude or down-weight hits whose depth
delta from origin exceeds a few meters *and* whose normal faces away
from the ray origin (backface); tighten temporal edge rejection for
large depth deltas.

Acceptance: halo box delta ≤3/255 per channel with a player at the
corner; `ENGINE_GI_TEMPORAL=0` and default settings both clean.

## Phase 2 — Eave band (bright seam under the roof line)

Hypotheses:

1. **Wall-top normal discontinuity.** The topmost wall texels sit on
   the silhouette edge between wall and sky/roof; reconstructed normals
   there are garbage or lean toward the surface, so a hemisphere whose
   "up" is tilted opens toward unoccluded sky and the sky-fallback
   radiance (bright sky) floods in. Check how normals are
   reconstructed in the estimate (`dx/ddy` from depth) and whether the
   band tracks exactly the wall silhouette (it appears to).
2. **Sky-fallback radiance too hot.** The miss radiance is the IBL sky
   color — if it reuses the same sky evaluation as direct lighting
   (sun-tinted zenith), an unoccluded hemisphere returns more energy
   than the IBL diffuse it replaces. Compare the miss radiance against
   the `kD_ibl`-scaled ambient it mixes with in `scene.frag`; the
   Phase-3 energy identity was algebraic-only (parked vantage had no
   sky texels), so a constant scale error on sky hits is exactly what
   would surface on eave lines.
3. **Depth-edge ray stepping.** March steps near the roof occluder may
   step *past* the thin roof overhang (roof is thin in screen space
   at this range) and report a miss; the ~100 px fade-down is
   consistent with rays whose hemisphere includes more sky as the ray
   origin approaches the occluder.

Fix directions: horizon-aware sky (hemisphere weight of the miss,
i.e. `Σ cos θ` over unoccluded directions, rather than full-sky
radiance); normal clamping/regularization at depth discontinuities;
sub-pixel roof thickness in the depth test.

Acceptance: top-strip box delta ≤3/255; the shadowed walls remain
noticeably brighter than a no-bounce reference (`ENGINE_GI_INTENSITY=0`).

## Phase 3 — Cyan cast on mid-wall

Hypotheses:

1. **Hit radiance uses raw albedo without the same lighting model the
   OFF path gets.** The OFF wall color is albedo × IBL diffuse (blue
   sky ambient). The ON wall adds albedo × (shadowless-direct + sky) of
   *whatever surface it hits* — at this vantage the dominant hit
   surface is probably grass/sky with a greener spectrum than the IBL
   term being partially replaced, and the mix weight
   `clamp(gi.a * giIntensity)` may not fully discount the replaced IBL,
   producing a net hue shift. Check the mix in `scene.frag` (lines
   ~224–251): verify the IBL term is scaled by `(1 − w)` and the GI
   term is pre-scaled to be the *same* ambient class (÷π, kD).
2. **AO attenuation coupling.** `aoAttenuationUpdate` (default
   `ENGINE_GI_AO_SCALE` 0.5) darkens CACAO in proportion to GI; on a
   mostly-unoccluded flat wall this shifts the AO/ambient balance in a
   hue-dependent way. A/B `ENGINE_GI_AO_SCALE=0` vs `1` on the wall
   box to size the contribution.

Acceptance: mid-wall box delta ≤3/255 per channel, hue of the shadowed
wall within a few degrees of the OFF shot.

## Phase 4 — Minor items (roof apex, contact line)

Same protocol, one box each: roof apex (+7 R vs sky) and wall–ground
contact line. Likely the same root causes as Phase 2 (silhouette-edge
sky fallback). Fold the fix in if it falls out of Phase 2 for free;
otherwise a targeted depth-edge bias at the contact test.

## Phase 5 — Regression & sign-off

- Re-run the full validation protocol; diff grid should be flat
  (≲2/255) outside the intended bounce-light regions (crevices, under
  canopies).
- `ENGINE_GI_TEMPORAL=0`, `ENGINE_GI_RAYS=4/6/16`, `ENGINE_GI_INTENSITY
  =0.5/1/2` all clean (no new banding/halo at extremes).
- Moving-camera check is user-driven (parked camera can't be moved —
  see AGENTS.md): orbit the camera at the house and confirm no halo
  trails behind the player and no eave band flickers as the silhouette
  angle changes.
- One `./scripts/run.sh log` clean run (no validation errors) and a
  RenderDoc frame if a fix touches the dispatch pipeline
  (`docs/renderdoc-capture.md`).

## Notes / constraints

- Do **not** move the parked player/camera (`build/c-game/data/db/db.db`
  transform rows); the whole A/B protocol depends on this vantage.
- Keep the determinism contract (screen-space only, no world state).
- Keep new env-knob usage in the `VulkanGiPass.h` debug-knob doc block.
- The high |diff| noise floor of cross-run A/Bs is wind-sway; signed
  per-channel box means are the discriminator (per the Phase-4 method in
  `plans/ssgi.md`).
