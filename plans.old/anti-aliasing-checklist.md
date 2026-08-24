# Anti-Aliasing Checklist

## Milestone 1 — CAS + policy

- [x] Make CAS default consistent between settings and `VulkanFinalPass.c`
- [x] Clamp CAS to `0..1` everywhere
- [x] Finalize AA sanitize rules in `c-engine/renderer/Renderer.c`
- [x] Decide initial combo policy:
  - [x] keep `MSAA` and `SMAA` fully independent
  - [x] allow all combinations
  - [x] keep sample shading selection persistent even when `MSAA Off`
  - [x] show sample shading as inactive/ignored in UI/debug when `MSAA Off`
- [x] Verify UI/debug show effective values correctly
- [x] Build: `./scripts/build.sh`
- [x] Screenshot check: `./scripts/run.sh screenshot`

## Milestone 2 — Vulkan sample-count plumbing

- [x] Add helper to map renderer MSAA setting to Vulkan sample count
- [x] Store active sample count in renderer/Vulkan state
- [x] Extend image/pipeline helpers for multisampled attachments
- [x] Extend pipeline creation for sample count selection
- [x] Build: `./scripts/build.sh`

## Milestone 3 — MSAA backend

- [x] Add multisampled scene color target
- [x] Add multisampled depth target if needed
- [x] Update world render passes to use multisampled targets:
  - [x] `VulkanMeshletRenderPass.c`
  - [x] `VulkanTriangleRenderPass.c`
  - [x] `VulkanGridPass.c`
- [x] Add resolve path back to single-sample post input
- [x] Keep `ResolvedColor` contract stable
- [x] Verify composite/bloom/final still work
- [x] Screenshot compare:
  - [x] `MSAA Off`
  - [x] `MSAA 2x`
  - [x] `MSAA 4x`
- [x] Build: `./scripts/build.sh`
- [x] **Bug fixed**: frame resources read desired sample count from renderer instead of own stale cache
- [x] **Bug fixed**: MSAA storeOp changed from DONT_CARE to STORE so multi-pass rendering works

## Milestone 4 — Depth/post compatibility audit

- [x] Verify depth prepass still works with MSAA mode
- [x] Verify Hi-Z input assumptions (single-sample depth resolved from MSAA works)
- [x] Verify AO depth sampling assumptions (uses single-sample resolved depth)
- [x] Verify reflection/composite expectations (reads single-sample SceneColor)
- [x] Decide whether normals/material need multisampled variants (yes, already implemented)
- [x] Build: `./scripts/build.sh`
- [x] Screenshot check: `./scripts/run.sh screenshot`

## Milestone 5 — Sample rate shading

- [x] Add pipeline flags for sample shading enable/min rate
- [x] Map settings:
  - [x] Off -> disabled
  - [x] Half -> `0.5`
  - [x] Full -> `1.0`
- [x] Apply only to multisampled graphics pipelines
- [x] Verify runtime switching does not break pipelines
- [x] Screenshot check under MSAA:
  - [x] Sample Shading Off (gpu: 2.29ms)
  - [x] Half (gpu: 2.58ms)
  - [x] Full (gpu: 3.16ms)
- [x] Build: `./scripts/build.sh`

## Milestone 6 — SMAA 1x

- [x] Add SMAA resources:
  - [x] edges texture (`SmaaEdges`, R8G8)
  - [x] blend weights texture (`SmaaBlendWeights`, R8G8B8A8)
  - [x] area texture (generated at init, 160×560 R8G8)
  - [x] search texture (generated at init, 64×16 R8)
- [x] Add SMAA shaders/pass implementation
- [x] Implement passes:
  - [x] edge detection (`smaa_edge_detect.comp` — luma-based with local contrast adaptation)
  - [x] blend weight calculation (`smaa_blend_weight.comp` — area LUT lookup)
  - [x] neighborhood blending (`smaa_neighborhood.comp` — directional blending)
- [x] Integrate into pipeline after composite and before bloom/final
- [x] Keep output writing to `ResolvedColor`
- [x] Decide initial interaction with MSAA (fully independent)
- [x] Screenshot compare:
  - [x] SMAA Off
  - [x] SMAA 1x
  - [x] MSAA 4x + SMAA 1x combined
- [x] Build: `./scripts/build.sh`

## Milestone 7 — UI / UX follow-up

- [x] Show which options are active vs ignored
  - [x] Sample shading shows "(inactive)" in debug GUI when MSAA is off
  - [x] Video settings grays out sample shading when MSAA is off
- [x] Reflect sanitize policy in UI labels if needed
  - [x] Policy label in both debug and video settings explains independence
- [x] Add optional descriptions/tooltips for:
  - [x] MSAA — "Geometric AA"
  - [x] SMAA — "Post-Process AA"
  - [x] Sample Rate Shading — "MSAA Quality"
  - [x] CAS — "Sharpening"
- [x] Decide whether any mode changes require restart/pipeline rebuild messaging
  - [x] No restart needed — pipelines rebuild live on setting change

## Milestone 8 — Cleanup

- [x] Removed SMAA T2x enum, labels, and fallback code (deferred to future if needed)

## Runtime validation checklist

- [x] No TAA path exists
- [x] No always-on camera jitter exists
- [x] `ResolvedColor` remains valid in all AA modes
- [x] Bloom still samples the correct final pre-tonemap image
- [x] Final pass still behaves correctly
- [x] Swapchain resize still recreates AA resources correctly
- [x] Settings persist correctly across restarts
- [x] Debug GUI and video settings stay in sync

## Nice-to-have follow-ups

- [ ] Add alpha-to-coverage for foliage/material cutouts when MSAA is enabled
- [ ] Add per-feature performance notes to debug/stats UI
- [ ] Add screenshot comparison presets for AA testing
