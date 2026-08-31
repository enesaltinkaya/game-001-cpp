# notes

**Status:** all 4 tasks done. Deliverable `plans/ssgi.md` (SSGI++ plan doc) written (task 3) and fully cross-checked against the tree (task 4, 14 fixes applied). Fact-sweep approach (brainstorm "approach 2") was executed; candidate approaches 1/3 rejected and are no longer relevant.

## Invariants (verified tree facts — ground truth for the plan)

- **Pass order** `vulkanInit()` (`c-engine/renderer/vulkan/Vulkan.cpp:293-323`); GI inserts after `vulkanSsrPass` (line 310) before `vulkanAOPass` (line 311). 0-based indices: scene = 10, ssr = 17, **GI = 18**, ao = 19.
- **AO temporal precedent is live:** `ao_temporal.comp` exists (rewritten for CACAO output). `VulkanAOPass` (CACAO dispatch → temporal pipe → `vulkanAOPassGetOutput()` NULL-sentinel) is the structural template for `VulkanGiPass`.
- **Ambient blocks:** `scene.frag` 195-227 (`ambientDiffuse = kD_ibl * irradiance * baseColor.rgb / PI * iblIntensity`; final `color = (ambientDiffuse + ambientSpecular) * shadowDarkFactor + Lo`); `azgaar_props.frag` 171-180 (same IBL chain, own cascade-only `shadowDarkFactor`).
- **AO strength knob exists** (survey's "no knob" claim stale): `settings.shadowMultiplier = aoEnvFloat("ENGINE_AO_STRENGTH", 1.0f)` (`VulkanAOPass.cpp:258`). It is **reassigned from env every frame in cacaoUpdate** — a runtime setter needs a precedence override, not a one-time set.
- **Orphan spv inventory (exact):** `ssgi/{ssgi,ssgi_temporal}` + `gi/{gi_estimate,gi_initial,gi_gather,gi_blur,gi_temporal}.comp.spv.debug` (survey §5 omits `gi_estimate`).
- **G-buffer formats** `VulkanFrameResources.cpp`: albedo R16G16B16A16_SFLOAT, normals R16G16_SFLOAT (157), material R8G8B8A8_UNORM (165), depth D32, velocity R16G16_SFLOAT (257); half-res precedent `renderWidth/2` line 210 (ReflectionDepth).
- **GetOutput contract** `vulkanAOPassGetOutput()` (`VulkanAOPass.h:34-37`, `.cpp:533-541`): temporal ping-pong output when temporal enabled, raw CACAO buffer when not, **NULL** before context exists / while disabled. Composite maps NULL → `0xFFFFFFFFu` push-const index (`VulkanCompositePass.cpp:99-103`, `composite.comp:21,115-120`); `composite.comp:116-120` applies `composite *= aoFactor`.
- **`AoTemporalPushConstants`** (`VulkanAOPass.cpp:328-342`), 13 fields: u32 `aoIndex/velocityIndex/depthIndex/prevIndex` (sampled), `outIndex` (storage), `width/height`; float `blendWeight` (ENGINE_AO_TWEIGHT 0.92), `depthThreshold` (TDEPTH 0.05), `clampSlack` (TCLAMP 0.35), `clampFloor` (TFLOOR 0.15), `devStart` (TDEV0 0.12), `devEnd` (TDEV1 0.50). Accumulators `temporalA/B` are **R16G16B16A16_SFLOAT** (.r AO, .g inverse view depth, 0 = no history, cleared black), dispatched at CACAO internal `cacaoWidth/cacaoHeight`, 8×8 workgroups, output transitioned to SHADER_READ_ONLY at end of dispatch (line 444); re-enable reset at 465-470. GI temporal copies all of this.
- **FSR reactive mask:** `docs/fsr3.1.md` has NO reactive-mask section (only SDK `ffx_fsr3upscaler_autogen_reactive_pass` / `_luma_instability_pass` names, lines 77/79; `rw_luma_history` rgba8→rgba16f patch at lines 181-184). The real per-pixel reactive signal is the engine's own `c-engine/data/pak_0_engine/shaders/pass/fsr/reactive.comp` (pipe `fsr_reactive`, `VulkanFsrPass.cpp:108-110`) → render-res R32F mask via `vulkanFsrPassGetReactiveMaskImage()` (DOF max-blends CoC into it). Terms: planar reflection, specular, **composite-difference fallback** (roughness>=0.25, rel. luma diff opaque-vs-composite, smoothstep 0.10-0.30 × 0.25), terrain grazing; alpha-cutout intentionally disabled. No `ENGINE_FSR` env var exists anywhere; `vulkanFsrPassSetReactiveMask` (`VulkanFsrPass.cpp:657`) is defined but called by nothing (usable only as a temporary diagnostic hook).
- **Build system needs no edits for the GI pass:** `build.sh` sources `scripts/shaders.sh` per module (c-engine then c-game); `shaders.sh` does `find ./data/pak_*/shaders -name "*.comp"` → compiles each to `<dir>/spv/<name>.comp.spv.debug` (`.spv.release` in release) with `glslc -I <pak>/shaders/includes`; `data.sh` zips the pak into `build/c-game`; missing spv → always compiled (mtime fast path only skips existing sources). CMake: `file(GLOB_RECURSE cSrc CONFIGURE_DEPENDS "*.cpp")` in both c-engine and c-game.
- **Scene-pass texture binding has no ceiling** (survey/round-1 "texture slot budget" premise was misframed): scene pass samples via `textures[MAX_IMAGES=4096]` / `samplers[MAX_SAMPLERS=11]` (`globalset.shader:3-4,374-375`). Real work for the GI texture = new `giIndex` uint in the `SceneBuffer` buffer-ref struct (`globalset.shader:247-264`) + host mirror `VulkanSceneBuffer` (`VulkanResourceManager.cpp:107`) + pool registration.
- **Blue noise:** `sceneBuffer.blueNoiseIndex` slot exists (`globalset.shader:227`) but is unpopulated (`VulkanIbl.cpp:107` "unused") — GI supplies its own LUT.
- **`ao.comp` does NOT exist** (removed in CACAO migration, `plans/cacao-ao.md:42-44`; `plans/ambient-occlusion.md` marked SUPERSEDED). HiZ chain + `vulkanHiZGetMipSampledIndex` still live (`VulkanHiZPass.cpp:408-412`); **no current shader does HiZ early-out** — `ssr.comp` has `hizIndex` but linear-marches (`depthEdgeFade` line 65, `local_size 8`, unjittered-UV + `invViewProjectionNoJitter` convention). `taa.comp` has `depthToInv` (50) and `prevUv = uv - mv / res` (117). AO "village-tuned scale" constants (startDist, clamp(0.15×dist,1,20)) belong to the removed pass, not CACAO.
- **Historical numbers:** 0.98 ms / 16.6 ms / "32 rays × ≤16 HiZ steps" / "≈0.5/255" all come from the SUPERSEDED XeGTAO plan (NVIDIA validation) — labeled historical in the doc; P1 re-baselines the current CACAO `ao` GPU cost (`plans/cacao-ao.md` records none). The `aoFrame` dump token was removed with XeGTAO (historical lesson only).
- **`azgaar_props` does not write velocity** → GI history over canopies relies on depth rejection (logged as a plan risk).
- **Debug-dump token table** (`Vulkan.cpp:165-271`): velocity/depth/normals/albedo/color/taa/ao/scene/oitReveal/oitAccum/lensIn/lensOut/dof/bloom + `<name>Raw` raw-byte variant. Plan adds `gi`, `giEstimate` (and `reactive` in P4).
- Survey section refs §1/§2.2/§2.4/§3/§4.1/§4.2/§5 all exist in `docs/global-illumination.md`.
- `Renderer.cpp:85` aoDisabled pattern; AO toggle in `c-game/game/settingsGui/graphics/SettingsGraphicsGui.cpp`.

## Design decisions (pinned in plans/ssgi.md; deviations from survey noted in the doc)

1. **Pass:** `VulkanGiPass` in `c-engine/renderer/vulkan/pass/gi/`, registered after `vulkanSsrPass` before `vulkanAOPass`.
2. **Resolution (was open in survey §4.1):** estimate at **half**-internal-res (single pass-owned slot, 1280×720×8 B ≈ 7.3 MB); temporal history at **full** internal-res ping-pong (each slot 2560×1440×8 B ≈ 29.5 MB; total ≈ 66 MB).
3. **One-frame latency:** `scene.frag`/`azgaar_props.frag` sample *last* frame's history, handed in as a `sceneBuffer` texture with NULL/absent-sentinel before the first GI frame (mirrors the AO output contract), plus a layout barrier after the GI temporal write so next-frame `scene` sampling is legal. `vulkanGiPassGetOutput()` adopts the identical NULL-sentinel contract.
4. **AO attenuation:** new `vulkanAOPassSetStrength(float)` runtime override of `settings.shadowMultiplier` (needs precedence over the per-frame env reassignment); survey's composite `aoStrength` uniform kept as documented fallback.
5. **FSR:** P4 targets `reactive.comp` (dump mask GI on/off); mitigation via `ENGINE_GI_TLUMA`, **not** by editing reactive.comp; P4 also delivers a new "reactive mask" section in `docs/fsr3.1.md`.
6. **Env knobs** modeled on the `ENGINE_AO_*` family (fact-check ref is #4, not #6).
7. **Phases P1–P4** each independently shippable/verifiable with own verification commands (`ENGINE_HIDE_GUI=1 ./scripts/run.sh play screenshot`, `ENGINE_LOG_PASS_GPU=1`, parked-player do-not-move rule): P1 estimate-only debug pass, P2 temporal filter (ping-pong + reprojection), P3 ambient injection + AO attenuation, P4 validation (reactive mask, GPU budget vs AO+SSR, OIT/vegetation artifact documentation).

## Open questions

- None outstanding in the notes. The former "sceneBuffer texture-slot budget" question is resolved (see binding invariant above). The plan's own "Open questions" section is the authority for implementation-time unknowns.

## Round log

- **round 1:** wrote `plans/ssgi.md` (457 lines): status checklist, background, survey corrections & fact checks, 7 design decisions, P1–P4, cost budget, risks. Residual fact checks (a) FSR reactive, (b) shader glob, (c) AO temporal push constants, (d) GetOutput contract — all resolved as recorded above.
- **round 2:** cross-checked every path/name/slot in `plans/ssgi.md` against the tree; fixed 14 stale/invented references (ao.comp attribution → ssr.comp + superseded plan; blue-noise unpopulated; D4 fact-check numbering; D5 per-frame env reassignment; GI pass index 11→18; aoFrame token historical; line-range corrections 533-541 / 181-184; XeGTAO numbers labeled historical; VRAM math 7.3/29.5/66 MB; no ENGINE_FSR env var; sceneBuffer slot premise misframed). All fixes are reflected in the invariants/decisions above.
- **round 3:** curator round — task 4 (cross-check) marked done per the round-2 log; no new sub-tasks from notes; verifier verdict PASS, deliverable `plans/ssgi.md` present (492 lines). Task complete.

## final

Compaction: merged brainstorm fact list + round 1 + round 2 into the Invariants/Decisions sections; dropped the rejected candidate approaches and per-round duplication. Invariants, decisions, and open questions preserved.
