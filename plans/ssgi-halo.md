# SSGI Character Halo — Fix Plan (round 2)

## Goal

With GI enabled, a soft ring ("halo") is still visible on the wall around
the character at the parked vantage. `plans/ssgi-artifacts.md` removed the
old dark smudge (−18/255) via the hit-reliability gates in
`gi_estimate.comp`; this plan removes the **residual** halo. Per this
session's measurement (below) the residual is **two distinct artifacts that
composite into one ring**, and both must go:

1. **GI dark band** — the GI estimate value *and* confidence dip in a band
   ~100–300 px from the character's silhouette (estimate 20 % below the
   far-wall value, confidence 0.6–0.9 vs 0.98). Source: `gi_estimate.comp`
   hit gating on the thin character silhouette.
2. **AO bright band** — the CACAO strength override (halved while GI is on,
   `ENGINE_GI_AO_SCALE` default 0.5) lifts the shadowed wall by +8…+15/255
   in the same corner region, because CACAO multiplies the whole composite
   (`composite.comp:120`) and the corner is a high-AO region. Source:
   `VulkanGiPass::aoAttenuationUpdate` + `composite.comp`.

The old plan's ON−OFF diff metric is **confounded** by artifact 2 (the AO
bump inflates ON−OFF by up to +15), so the pass criteria below use
three-way metrics (see Validation protocol).

## Baseline (measured 2026-09-01, this session)

Parked vantage: character standing at the house wall corner, screen
silhouette ≈ x 1350–1560, y 900–1250 (2880×1627). All runs
`ENGINE_HIDE_GUI=1 ENGINE_SCREENSHOT_DELAY_MS=6000 ./scripts/run.sh play
screenshot`, 6 s settle. Reference files: `/tmp/halo_on.jpg` (default),
`/tmp/halo_off.jpg` (`ENGINE_GI_DISABLED=1`), `/tmp/halo_notemp.jpg`
(`ENGINE_GI_TEMPORAL=0`), `/tmp/halo_r16.jpg` (`ENGINE_GI_RAYS=16`),
`/tmp/halo_ds03.jpg` / `halo_ds01.jpg` (`ENGINE_GI_DIST_SCALE=0.3/0.1`),
`/tmp/halo_aofull.jpg` (`ENGINE_GI_AO_SCALE=1.0`), raw GI fields
`/tmp/halo_raw_gi.bin`, `/tmp/halo_raw_giEstimate.bin`
(`ENGINE_DEBUG_DUMP_IMAGES=giRaw,giEstimateRaw`, R16G16B16A16 float16,
full/half internal res; decode with `np.float16`).

Signed ON−OFF box means (R/G/B), boxes on the wall:

| region (wall px box) | default | AO_SCALE=1.0 |
|---|---|---|
| left face, 0–100 px from silhouette | +8.4 / +11.9 / +8.7 | +0.1 / +1.3 / −0.9 |
| left face, 100–200 px | +11.2 / +15.1 / +10.5 | +1.5 / +2.7 / −0.5 |
| left face, 200–300 px | +9.3 / +12.0 / +8.8 | +0.7 / +1.5 / −0.3 |
| right face, 0–140 px | +5.5 / +9.3 / +7.6 | −2.1 / −0.0 / −0.4 |
| right face, 140–280 px | +4.5 / +6.9 / +5.7 | −1.2 / −0.1 / −0.3 |
| right face, 280–440 px | +2.4 / +3.0 / +2.4 | −0.2 / −0.0 / −0.1 |
| right face, > 500 px | ~0 | ~0 |

Raw GI history (full-res `giRaw`), value / confidence:

| region | rgb | conf |
|---|---|---|
| far right face | (0.67, 0.88, 1.11) | 0.98 |
| far left face | (0.64, 0.87, 1.11) | 0.98 |
| ring 100–200 px | (0.50, 0.71, 0.82) — ~20 % below far | 0.87–0.90 |
| at silhouette (2 px) | (0.53, 0.65, 0.70) | 0.67 |
| above head | (0.60, 0.66, 0.67) | 0.84 |

Facts that constrain the mechanism:

- **Temporal is a passthrough.** Default vs `ENGINE_GI_TEMPORAL=0`: all box
  deltas ≤ 0.2/255. The halo is entirely in the per-frame estimate.
- **Not MC noise.** `ENGINE_GI_RAYS=16` is box-identical to 6 (Δ ≤ 0.1).
  Systematic bias in the estimator's mean.
- **AO confound dominates ON−OFF.** Flipping only `ENGINE_GI_AO_SCALE`
  0.5→1.0 removes +8…+15 of the left-face bump (→ ≤ +2.7) and turns the
  right face slightly **negative** (−2.1 R at 0–140 px): the residual GI
  dark band. The visible halo ≈ bright AO band (artifact 2) compositing
  with a small dark GI band (artifact 1); the two have opposite signs on
  the two faces, which is what makes the ring read as an odd halo rather
  than uniform lighting.
- `composite.comp:120` applies CACAO as `composite *= aoFactor` to the
  whole image; `scene.frag` contains **no** AO term. So halving CACAO
  strength while GI is on lifts every high-AO region (the house corner is
  one) by ~ (aoFactor·0.5) of composite value — geometry-driven, not
  GI-driven, and independent of where the character stands.

## Files

- `c-engine/data/pak_0_engine/shaders/pass/gi/gi_estimate.comp` — hemisphere
  ray march + hit gates (frontness `smoothstep(0.05, 0.40, front)`,
  sandwich `1 − sandwich·gapS` with gap ramp `mix(0.45, 1.30)` on
  `hitT/maxDist`, thin-silhouette neighborhood gate, range fade
  `1/(1+(hitT/maxDist)²)`), 2×2 origin rule (closest full-res texel of
  the block), `originEdgeFade`.
- `c-engine/renderer/vulkan/pass/gi/VulkanGiPass.cpp` — `aoAttenuationUpdate`
  (global CACAO strength override while GI enabled), env-knob doc in
  `VulkanGiPass.h`.
- `c-engine/data/pak_0_engine/shaders/pass/composite/composite.comp` —
  `aoFactor` application site (would need the GI image + a per-pixel gate
  for the Phase-2 fix).
- `c-engine/data/pak_0_engine/shaders/pass/scene/scene.frag` — GI mix into
  `ambientDiffuse` (unchanged, reference for units).

## Validation protocol (every step)

```bash
# ON (default) + raw GI fields
TERM=xterm ENGINE_HIDE_GUI=1 ENGINE_SCREENSHOT_DELAY_MS=6000 \
  ENGINE_DEBUG_DUMP_IMAGES=giRaw,giEstimateRaw \
  ./scripts/run.sh play screenshot /tmp/h_on.jpg
# OFF
TERM=xterm ENGINE_GI_DISABLED=1 ENGINE_HIDE_GUI=1 ENGINE_SCREENSHOT_DELAY_MS=6000 \
  ./scripts/run.sh play screenshot /tmp/h_off.jpg
```

Metrics (Python/PIL recipe from this session — grid + signed box means;
raw `giRaw`/`giEstimateRaw` decoded as `np.float16`, shape 1627×2880 /
814×1440 ×4):

- **M1 — GI-field ring profile** (primary for artifact 1): on the raw GI
  history, box means along the falloff lanes (right face 0–140/140–280/
  280–440/>500 px from silhouette; left face 0–100/100–200/200–300) plus
  `conf`. Pass: ring-band rgb within **10 %** of the far-wall rgb, `conf`
  ≥ 0.95 outside a 4-px silhouette band.
- **M2 — final-image ON state** (primary for artifact 2): in the ON shot
  alone, |ring-box − far-wall-box| per channel ≤ 3/255 (the user-visible
  halo; immune to the OFF-side AO confound).
- **M3 — ON−OFF with default AO setting**: ring boxes ≤ 3/255 signed
  (catches anything the ON-state metric can't, e.g. a hue shift only
  visible in reference).
- **M4 — feature retained**: `ENGINE_GI_INTENSITY=0` A/B — the shadowed
  walls must stay brighter with GI on (bounce light survives), and the
  crevice/corner darkening CACAO provides must not regress past the
  pre-GI look (AO-attenuation change is A/B'd against `aoScale 1.0`
  default runs).
- Debug aids: `ENGINE_GI_TEMPORAL=0` (should stay identical to default —
  any divergence after changes is a regression), `ENGINE_GI_RAYS`,
  `giEstimateRaw` for per-frame ray-level views.

## Phase 1 — GI dark band (`gi_estimate.comp`)

Hypothesis (prior order):

1. **Partial-gate dark leak.** Gated rays only shed *confidence*; the
   *value* still mixes in the hit radiance at `w = rangeFade·hitW`. For
   near-field hits on the character (dark leather, backface-to-wall
   sandwich, thin silhouette) the gates leave a 5–20 % residual weight of
   a much-darker-than-sky radiance, and the sandwich `gapS` ramp
   (0.45–1.30 of `hitT/maxDist`) spreads that penalty over a band ~1.3 m
   wide — exactly the measured 300-px ring. Fix: when a gate fires, the
   ray's *value* must revert to `skyIrradiance(N)` (already the fully-
   gated path's behaviour) — i.e. weight the hit term by the **product**
   of gate factors squared, or clamp hit contribution to
   `min(hitIrradiance, skyIrradiance(N))` for thin/backface hits (a thin
   dark occluder must not darken the receiver below sky).
2. **Band width is path-length, not gap.** The sandwich near-field ramp
   uses `hitT` (ray path length), not the origin–hit *depth gap*. The
   wall–character gap at this vantage is 0.1–0.5 m; a 1.3 m ramp is the
   ring's width. Fix: derive the near-field band from the world gap
   (`|worldPos − hitPos|` / `abs(originDepth − hitDepth)` in meters) and
   cap it at ~0.5–1 m.
3. **2×2 origin rule at the silhouette.** Half-res texels straddling the
   character take the character (closer, reversed-Z) as origin → wall
   texels get the character's normal/position/albedo estimate (the 0.67-
   conf 2-px band in M1). Fix: if the 2×2 block spans a depth gap beyond
   the origin-edge threshold, treat the block as an edge (origin fade,
   confidence only) instead of picking the closest texel.

Experiments before committing a fix (one-line shader changes, measure M1):
- E1: sandwich → hard reject (`hitW *= 1.0 - step(-0.55, dot(N, hitN));`)
  with `gapS` removed.
- E2: `gapS` ramp narrowed to 0.2–0.5.
- E3: value weight `w = rangeFade·hitW·hitW` (soft) vs current `hitW`.
Expect: the M1 ring dip (≈20 %) collapses toward the far-wall value; the
above-head dip (10 %) should follow from the same fix — if it doesn't, it
is a separate origin-normal issue (fold into E3/Phase 3).

Acceptance: M1 pass; M2/M3 ≤ 3/255 with `ENGINE_GI_AO_SCALE=1.0`
(Phase-2-independent check of the pure GI residual); `TEMPORAL=0`
identical to default.

## Phase 2 — AO bright band (CACAO override)

The global `vulkanAOPassSetStrength(0.5)` while GI is on is the old plan's
documented compromise, but it is measured to be the dominant visible
component of the halo (M2/M3 table above). Replace it with a per-pixel
interaction:

1. Keep CACAO at full strength (remove the `aoAttenuationUpdate` override
   path; `ENGINE_GI_AO_SCALE` retained as a debug A/B only).
2. In `composite.comp`, scale the AO factor by the GI estimate's
   confidence: `aoFactor = mix(aoFactor, 1.0, giConf)` (the GI estimate
   already embeds diffuse occlusion, so where GI is trusted, CACAO's
   occlusion is redundant — exactly the old rationale, but spatially
   correct). The composite pass needs the GI image index + its 0xFFFF
   …FFFF absent-sentinel (same contract as `sceneBuffer.gi`); sample at
   the composite texel (bilinear, one-frame latency as everywhere).
3. Tune the gate shape (linear in `gi.a` vs a smoothstep) with M2/M4:
   crevices (high CACAO, high GI confidence) must not get brighter than
   the pre-GI look more than the old 0.5 override did, and flat walls
   near the character must not get the +12 bump back.

Acceptance: M2 pass (no bright band, no new dark band); M4 crevice A/B
vs pre-GI within a few /255; the old eave/cyan artifacts from
`ssgi-artifacts.md` unchanged.

## Phase 3 — Minor residuals

- Above-head dip (M1: 10 % below far wall, conf 0.84): likely the same
  partial-gate leak (her head occludes a few rays) — verify it fell out
  of Phase 1; if not, check the origin normal above the silhouette
  (facing the sky vs the wall).
- 2-px silhouette band (conf 0.67): from E3 / the 2×2 origin rule; must
  not exceed a 4-px low-conf band.
- Re-run the old plan's remaining boxes (eave strip, mid-wall hue) —
  both plans share this vantage; the Phase-1 ramp changes must not revive
  the eave band.

## Phase 4 — Regression & sign-off

- Full M1–M4 protocol at defaults; also `ENGINE_GI_RAYS=4/16`,
  `ENGINE_GI_TEMPORAL=0`, `ENGINE_GI_INTENSITY=0.5/2` — ring profile flat
  in all.
- `./scripts/run.sh log` clean (no validation errors); RenderDoc frame if
  the composite pass gains a new image binding (`docs/renderdoc-capture.md`).
- User-driven: orbit the parked camera at the house — no halo trails
  behind the character, no flicker as the silhouette angle changes
  (ask the user to park/re-park if a second vantage is wanted; do not
  move the parked transform).

## Notes / constraints

- Do **not** move the parked player/camera (`build/c-game/data/db/db.db`).
- Keep the determinism contract (screen-space only).
- Keep new env-knob usage in the `VulkanGiPass.h` debug-knob doc block;
  `ENGINE_GI_AO_SCALE` semantics change in Phase 2 (update that block).
- The GI estimate is the *only* place the halo can be born (temporal is
  a measured passthrough) — do not tune `gi_temporal.comp` for this
  artifact.
- ON−OFF final-image diffs at the corner are confounded by CACAO until
  Phase 2 lands; always read M1 (raw field) and M2 (ON-state) alongside
  M3, never M3 alone.
