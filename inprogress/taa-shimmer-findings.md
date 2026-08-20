# Original chat with ai

me: "forza 6 taa looks great.
i cant replicate that in my own game engine.
if i fix ghosting i get heavy shimmering.
if i fix shimmering i get heavy ghosting.
are there any details on how forza 6 achieved such great quality with taa?"

ai: "I couldn't find a public deep-dive specifically from Turn 10 on Forza Motorsport 6's TAA (unlike, say, id Software or Naughty Dog talks, Turn 10 hasn't published much on the exact algorithm). So take this as informed general knowledge about what separates good TAA from bad TAA, since the ghosting-vs-shimmering tradeoff you're describing is basically _the_ central problem in TAA design and Forza is often cited as one of the better implementations of its era.

The core issue: ghosting and shimmering are two failure modes of the same root cause — how aggressively you trust the history buffer. Crank up history weight/rejection tolerance → ghosting. Crank down history trust → shimmering (because you're falling back to raw jittered samples that alias). Good TAA doesn't pick a point on that slider — it uses several independent techniques so neither failure mode has to carry all the weight.

Things that actually move the needle, roughly in order of impact:

**1. Neighborhood clamping/clipping, not just rejection**
Naive TAA rejects history when it differs too much from the current pixel — this is where the ghosting/shimmer tradeoff comes from directly. Instead, clamp the history sample into an AABB (or better, a variance-based ellipsoid) built from the current frame's neighboring pixels (3x3 or similar), often in YCoCg space. This lets you keep blending history for anti-aliasing but constrains it so stale data can't persist as a visible ghost. This one change alone eliminates most "either/or" behavior.

**2. Better sample rejection than simple color distance**
Use depth + motion vectors to detect disocclusion (things that were hidden last frame) and reset history there instead of blending. A lot of ghosting comes from disoccluded regions blending in stale background pixels behind moving objects (classic car-edge ghosting).

**3. Per-pixel motion vectors for everything that moves, including things shaders fake**
If your motion vectors only cover rigid mesh transforms and miss things like wheel rotation blur, vertex-animated foliage, or skinned parts, those regions get residual ghosting or shimmer because reprojection is wrong there. Forza's cars have lots of fast rotating/reflective geometry (wheels, disc brakes) — getting motion vectors right on high-frequency rotating parts is disproportionately important for a racing game specifically.

**4. Sub-pixel jitter pattern and count**
A well-distributed low-discrepancy jitter sequence (Halton 2,3 is standard) over enough frames (8-16) before falling back to spatial AA gives you more effective supersampling per unit of temporal weight, which reduces how much history "insurance" you need against shimmer — meaning you can afford tighter clamping (less ghosting) without shimmer creeping back in.

**5. Variance clipping over min/max clamping**
Min/max AABB clamping (what a lot of hobby TAA implementations use) is crude and either too loose (ghosting) or too tight (loses temporal stability, shimmer returns). Variance clipping (Salvi's SIGGRAPH 2016 technique, used widely since — including presumably by Turn 10 given the era) computes mean/variance of the neighborhood and clips based on that, giving a much better ghosting/sharpness compromise.

**6. Sharpening the resolve, not the input**
A lot of "TAA feels bad" comes from softness, which people compensate for by increasing current-frame contribution — which reintroduces shimmer. Instead, apply a mild sharpening filter (CAS-style) _after_ the temporal resolve so you can keep history weight high (stable, low shimmer) without the mush.

**7. Velocity-weighted history blend factor**
Instead of a fixed alpha (e.g. always 90% history / 10% current), scale the current-frame weight by how much a pixel moved (from motion vector magnitude) and by confidence in the reprojection. Fast-moving thin objects get more current-frame weight (less ghosting there specifically) while static/slow areas stay heavily temporally accumulated (rock solid, no shimmer)."

# TAA shimmer — findings (in progress)

Goal: kill the frame-to-frame shimmer in tree canopies (esp. distant trees)
under TAA, static camera + wind-sway animation, without reintroducing ghosting.
Reference quality: Forza 6 TAA.

Measurement harness (already exists):

- `./scripts/run.sh play screenshot <prefix> 8` → 8 consecutive frames
  (each capture blocks ~100ms; exactly one frame is rendered between shots,
  so the shots ARE consecutive frames).
- `python3 scripts/shimmer_check.py <prefix> 8` → per-pair `mean_abs`
  (0–255) and `shimmer_pct` (fraction of pixels with max-channel diff > 8).
  Baseline canopy shimmer ≈ 17% shimmer_pct, mean_abs ≈ 4.

---

## TL;DR — the depth rejection is the shimmer source, NOT the color rejection

Disabling the **depth** rejection alone drops shimmer from ~17% to ~0.2%.
Disabling the **color** rejection alone does almost nothing.

The depth rejection false-fires across the whole canopy because the
reprojected previous-depth tap is misaligned by ~0.5px (sub-pixel jitter) and
the canopy is a thick 3D volume with steep depth gradients — so even a
sub-pixel offset samples a very different depth (a different leaf, or sky
between leaves). The fixed relative threshold (smoothstep 0.005–0.03) then
kills the temporal history everywhere in the foliage → shimmer.

The color rejection is fine: it only fires on strong edges (diff > 50% of
luminance), i.e. real leaf-vs-sky disocclusions, not the subtle leaf-pattern
changes inside the canopy.

---

## Experiment matrix (shimmer_pct, 8 consecutive frames, threshold 8)

| #   | Variant (taa.comp)                         | Weather | shimmer_pct | mean_abs |
| --- | ------------------------------------------ | ------- | ----------- | -------- |
| 1   | Baseline (depth + color rejection)         | ON      | 14.7–19.2   | ~4.0     |
| 2   | Baseline (depth + color rejection)         | OFF     | 7.03        | 2.03     |
| 3   | Depth rejection DISABLED, color kept       | ON      | 0.12–0.26   | ~0.1     |
| 4   | Color rejection DISABLED, depth kept       | ON      | 4.4–5.2     | ~1.6     |
| 5   | Both disabled, forced weight=0.99          | ON      | 1.3–2.0     | ~1.0     |
| 6   | Gradient-aware depth rejection (see below) | OFF     | 6.4–7.0     | ~1.9     |

Takeaways:

1. Depth rejection is the dominant shimmer source (row 1 vs row 3).
2. Color rejection is not the problem (row 1 vs row 4: little change).
3. Weather adds ~10% shimmer ONLY when the depth rejection is active
   (row 1 vs row 2). With the depth rejection off, weather contribution is
   negligible (row 3 ≈ 0.2%). The depth rejection is reacting to the moving
   weather particles (disocclusions) as well as the canopy.
4. A gradient-aware depth rejection (normalising by the 3×3 neighbourhood
   depth range) is NOT enough — row 6 (6.5%) ≈ row 2 (7.0%) baseline. The
   current-vs-previous depth comparison is inherently noisy in a swaying
   thick canopy: the previous-frame depth at the reprojected tap doesn't
   match the current neighbourhood range because the leaves moved between
   frames. No threshold tuning fixes this.

---

## Root cause (detail)

TAA reprojection in `taa.comp`:

```
vec2 mv     = texture(velocity, uv).xy;     // sampled at jittered pixel uv
vec2 prevUv = uv - mv / res;                // reprojected previous tap
```

- The current frame COLOR is sampled at `uv - jitter` (the UNJITTERED content
  at pixel uv).
- But the VELOCITY and DEPTH are sampled at `uv` (the JITTERED pixel).
- Net effect: the reprojected previous tap is off by up to ~0.5px from where
  it should be. In flat regions that's harmless; in the canopy (steep depth
  gradient) it samples a different leaf's depth → `|currDepth - prevDepth|`
  is large → depth rejection fires → history killed → shimmer.

The canopy is also a thick 3D volume: depth varies by metres over a few
pixels, so even a correct reprojection sees large current-vs-previous depth
differences as leaves sway. The depth test cannot distinguish "leaf moved"
from "real disocclusion" here.

Why the color rejection is safe: `smoothstep(0.5, 1.0, maxChannelDiff/lum)`
only fires when the colour change is > 50% of luminance — strong
leaf-vs-sky edges, not intra-canopy leaf-pattern variation.

---

## Tools created this session (fast iteration loop)

1. `scripts/dev_taa.sh` — recompile ONE shader with glslc + refresh the
   engine pak entry via `zip`, no full `build.sh` needed.
   Targets: `taa`, `scene_depth`, `props_depth`, `props`.
   - BUG FIXED: spv output path was computed with `${src%.*}` (wrong — gave
     `pass/taa/taa`); must be `${src%/*}` (gives `pass/taa`). The broken
     path made the `zip` re-pack the OLD spv, silently invalidating the
     first several experiments (they all reported identical numbers).
     A pure-red TAA output test confirmed the loop now works.
2. `scripts/dev_run.sh` — launch the already-built `c-game` with the same env
   as `run.sh` but skip `build.sh`.
   - BUG FIXED: must run from the REPO ROOT, not `build/c-game`. The scene
     parser writes a debug JSON to `scripts/gltf-json-debug/` relative to
     CWD; running from `build/c-game` segfaults in `fileWriteBinary`.
3. `ENGINE_AZGAAR_WEATHER=0` disables weather (state→0, pass skips the draw)
   for cleaner shimmer tests.

---

## Current code state

- `c-engine/data/pak_0_engine/shaders/pass/taa/taa.comp` currently contains
  the **gradient-aware depth rejection** (row 6) — it compiles and runs but
  is NOT an effective fix (6.5% vs 7.0% baseline). It should be reverted or
  replaced.
- Original shader backed up at `/tmp/taa.comp.orig`.
- No C sources were changed. Only the TAA shader (and the two new scripts).

---

## Next steps (pick up here)

Ranked by expected impact / risk:

1. **Remove (or drastically loosen) the depth rejection** and rely on the
   color rejection + neighbourhood clamp for ghosting control. This is the
   validated 0.2% result (row 3). MUST verify no ghosting with a MOVING
   camera / moving objects (the static-camera test can't show ghosting).
2. **Fix the jitter misalignment** so the depth comparison is accurate:
   sample velocity (and ideally depth) at the unjittered position
   (`uv - jitter`) to match where the current colour is sampled. Then a
   _loose_ depth rejection can be kept as a real-disocclusion safety net.
   Note: `texelFetch` needs integer coords; the unjittered tap is fractional,
   so depth may need a nearest-integer round or a careful formulation.
3. **Very loose depth safety net**: only reject on large, unambiguous
   disocclusions (depth far outside the neighbourhood range by a wide
   margin), so it catches camera-move ghosting but ignores canopy noise.
4. (Optional) Re-enable stochastic alpha dither for cutout foliage:
   `stochasticAlphaDiscard(..., frameIndex)` already supports a temporal
   dither path (`frameIndex > 0` in `includes/utils.shader`) but the scene
   passes `0u` (plain alpha test). A world/UV-anchored per-frame dither would
   let TAA resolve cutout edges temporally (the standard foliage solution).
5. Re-run the full matrix with weather OFF for the final numbers, and add a
   moving-camera ghosting check before declaring done.

Open question to resolve: does removing the depth rejection cause visible
ghosting when the camera moves? That is the only thing blocking the simple
fix (option 1).

---

# Update 2026-08-20 — tree gap ghosting under TAA (root cause + fix)

Symptom: with TAA on, trees looked see-through (gaps between branches)
and those gap areas showed ghosting / a smudge-brush look, esp. at the
bottom of the canopy where the ground shows through.

## Root cause (two independent bugs)

1. **Velocity buffer mismatch at cutout gaps (props trees).**
   The azgaar_props depth/velocity pre-pass (`azgaar_props_depth.frag`)
   deliberately skipped the alpha test so FSR/TAA would get motion vectors
   for the whole animated canopy. Consequence: at gap pixels (background
   visible between branches) the velocity buffer held the LEAF's motion
   vector even though the rendered content there is the BACKGROUND, which
   moves at a different parallax speed. TAA reprojected the background's
   history with the leaf's velocity → smearing/ghosting through the gaps.

2. **Spatial depth test in taa.comp was measuring the wrong thing.**
   It compared the CURRENT frame's depth at the pixel vs the CURRENT
   frame's depth at the reprojected position — i.e. the canopy's
   within-frame depth gradient, not disocclusion. Noisy in 3D volumes
   (the shimmer of the previous session); the `motionGate` that muted it
   at low camera speeds also disabled real disocclusion rejection →
   ghosting.
   Bonus bug found: taa.comp's `linearizeDepth` used
   `2NF/((F-N)d + N+F)` while every other pass uses
   `NF/(N + (F-N)d)` (the correct inverse for this engine's reversed
   depth buffer: 0 = far/empty, 1 = near, GE test, clear 0 — see
   VulkanPipe.c). The TAA variant compressed all distances into
   [N, ~2N], making relative depth comparison useless for far content.

## Fix (implemented)

1. `azgaar_props_depth.vert` now outputs UV / texId / species; the
   fragment applies the SAME discards as `azgaar_props.frag` (flower
   radial test + `stochasticAlphaDiscard(tex.a, 0.5, gl_FragCoord.xy,
   frameIndex)`). Both passes rasterize with the identical jittered
   transform, so decisions match pixel-for-pixel. Velocity/viewNormal are
   now written only where a leaf is actually drawn; gap pixels keep the
   background's motion vector (written by the terrain/scene pre-passes
   that run first in the depth pass). This also fixes FSR's motion
   vectors at those pixels, and GTAO sees background normals through
   gaps.
2. `taa.comp` now does a TRUE temporal depth test: each frame's inverse
   view depth S = 1/D = (N + (F-N)d)/(N*F) (affine in the raw buffer
   depth, A16F-safe, ~0.05% relative precision) is stored in the
   accumulator's alpha (previously unused, cleared 0 = "no data"). The
   test compares current S vs the reprojected HISTORY's S and rejects on
   >`taaDepth` relative difference (default 0.06, smoothstep from 0.25x).
   Empty/sky maps to a 0 sentinel → 100% difference vs any surface,
   so leaf<->gap transitions always reject (kills the gap ghosting),
   while two nearby leaves differ by <<1% (no canopy shimmer). The
   `motionGate` is gone; the spatial depth fetch is gone. Color rejection,
   3x3 neighborhood clamp, weather mask and motionConf weight are kept.
   Swaying canopies are tracked correctly because the props depth vertex
   shader already emits the previous-frame sway into the motion vector.
3. New setting `taaDepth` (0.01–0.5, default 0.06; env override
   `ENGINE_TAA_DEPTH`). Settings fix: `settingsInit` now seeds missing
   template keys in place instead of rewriting the whole file (a new
   template key used to wipe user settings).

Files changed: taa.comp, azgaar_props_depth.vert/.frag,
VulkanTaaPass.c, Renderer.h/.c, Settings.c.

## Manual test checklist (user)

- Gaps between branches: background seen through them stays sharp while
  panning/walking (no ghost trails / smudge).
- Bottom of canopy: smudge-brush look gone.
- Static camera: leaves don't shimmer (regression check from the
  previous session).
- Walk PAST a tree: no ghost trails behind the canopy, and no new
  shimmer on the canopy itself. If shimmer returns, raise
  ENGINE_TAA_DEPTH (e.g. 0.10); if ghosts remain, lower it (e.g. 0.02).
- TAA off / FSR upscaler paths still work (props pre-pass change also
  affects FSR motion vectors — should be strictly an improvement).
