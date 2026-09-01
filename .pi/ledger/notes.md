# notes

## Invariants & decisions (verbatim from rounds — do not lose)

1. **Energy contract (lemma 1)**: estimate output = cosine-weighted irradiance (×π); consumer: `ambientDiffuse = mix(ambientDiffuse_ibl, kD_ibl * gi.rgb * baseColor.rgb / PI * iblIntensity, gi.a * giIntensity)` with **no** shadowDarkFactor inside the mix. All-miss texel ≡ `skyIrradiance(N)` by construction.
2. **No feedback**: estimate reconstructs hit radiance analytically (`hitAlbedo·(1−hitMetallic)·(sunColor·NdotL + skyIrradiance(hitN))`) — never samples lit scene color (temporal feedback → unbounded multi-bounce).
3. **Clamp direction**: temporal 3×3 box built from *current* estimate neighborhood, *reprojected history* clamped into it. History channels: `rgb = filtered irradiance, .a = confidence` + separate R16F inv-depth ping-pong pair (D2's 5-values/4-channels resolved).
4. **One-frame latency**: scene pass (idx 10) binds GI's `lastOutput` (prev completed frame); ping-pong slot swap guarantees no write-while-bound; NULL → `0xFFFFFFFFu` sentinel → IBL fallback; disable→enable resets history; per-frame index writes patch only the current flight-index scene buffer.
5. **AO attenuation**: estimate never samples hit pixel's CACAO AO (double-occlusion); `vulkanAOPassSetStrength()` override member beats per-frame `ENGINE_AO_STRENGTH` read; driven to `ENGINE_GI_AO_SCALE` (0.5) when GI active.
6. **Budget call (P4)**: keep 6 rays — gi pass 1.14 ms (24% under 1.5 ms fail line; 4 rays saves 0.32 ms for ~22% more MC noise); azgaar_props consumer fetch +0.42 ms (18%); frame 7.71→9.92 ms (residual is DVFS variance, not GI).
7. **Linear march over HiZ**: HiZ early-out deferred (unproven in-tree; same fetch count per step); SSR-convention doubling-step march used.

## Measured actuals (RADV/radeon ICD, 2880×1627 internal, 600-frame avgs)

- Baseline re-baseline: ao = 0.56 ms, ssr = 0.04 ms, frame 7.73 ms.
- gi = 1.13–1.14 ms total (estimate 0.94 @6 rays ~0.152 ms/ray + 0.06 fixed; temporal ≈0.19). 4 rays = 0.65 ms.
- Boiling test: inter-frame mean|diff| 0.019 vs 0.13 raw (6.8×); spatial std 0.040 vs 0.128; 0 NaN/inf. FSR final-frame boiling unchanged (1.96 vs 1.91/255, p99 identical).
- Reactive mask: GI-on reduces mean mask 0.110→0.094; 0.5% pixels newly >0.1 (term-3 knee reshuffle) — no mitigation needed.

## Tooling gotchas (verified)

- `vulkanSaveImage` float-JPEG normalizes per-channel [min,max]→[0,255]: uniform buffers dump **black** — verify uniformity via `Raw` token or the `float range` log line.
- Dumping an image requires `TRANSFER_SRC` in its creation flags (Material + FsrReactiveMask lacked it).
- `run.sh play screenshot` **overrides** exported `ENGINE_SCREENSHOT_COUNT` (pass count as 3rd arg).
- Cross-run JPEG diffs have high zero-mean noise floor (wind sway, control 41% pixels >2/255); use **signed mean diff** as discriminator (control −0.03 vs GI +1.88/255).
- `vulkanTransientBegin/End` live in `VulkanResourceManager.h`. Push-constant ranges fixed 256 B — extend structs freely.
- Odd full-res height (1627) vs 8×8 groups: pad depth rows when mapping 2×2 blocks.
- `syncAAUi` (not just `syncEffectLabels`) must sync new GUI labels.
- GI is first in-tree albedo sampler → albedo must return to COLOR_ATTACHMENT_OPTIMAL in depth `preUpdate` (VUID 09592).
- Local settings.json had stale pre-migration `gi*` prototype keys (purged in R5; template default was the shipped behavior).

## Open questions / user-driven follow-ups (parked-camera limits)

- Camera-orbit ghost-trail + orbit-reactive checks need a user-driven moving vantage (parked camera static).
- Open-sky energy identity and sky-path pixels: no sky texels at parked vantage (verified algebraically only).
- OIT-neighbor contrast: no OIT objects in view at vantage.
- GUI toggle click-test (headless rounds only).
- `reactive.comp` `isAlphaCut` reads material.b which terrain overloads ao=1 (all terrain reads alpha-cut; harmless, alphaCutReactive=0).

## Round log

- R1 (task 6, P1a): skeleton + constant-sky estimate + dump tokens + disable path; gi=0.01 ms. PASS.
- R2 (task 1, P1b): full ray-march estimate; 0.94 ms @6 rays; depth-pass albedo restore. PASS.
- R3 (task 2, P2): temporal filter + ping-pong history + ENGINE_GI_T* knobs; pass 1.13 ms; boiling 6.8×. PASS.
- R4 (tasks 3+7, P3): GiData SceneBuffer + injection in scene/props frags + AO attenuation + settings toggle (off-by-default). PASS.
- R5 (task 4, P4): reactive/material dump tokens, validation (reactive mask, RADV flicker, OIT, vegetation A/B signed-mean +1.88/255 across veg+non-veg), budget call keep 6 rays, docs (fsr3.1.md reactive section, global-illumination.md → implemented), stale keys purged, default flipped GI-on. PASS.

Approach comparison (brainstorm): D (plan + energy contract + linear march) chosen over B (sceneColor sampling — feedback risk) and C (probe tier — reserved fallback only).
