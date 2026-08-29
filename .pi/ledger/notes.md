# notes

## brainstorm

## Core difficulty

This is a re-integration of a _removed_ subsystem (`plans/brixelizer-gi.md` is gone, so the only ground truth is the FFX sample, `docs/fsr3.1.md`, and engine buffer conventions), and the hard part isn't the FFX API — it's the engine-side data: the voxelizer needs CPU-visible mesh instances, but the scene is a streamed implicit heightmap (no mesh) plus per-tile streamed props, so the SDF can only ever contain props (terrain is invisible to GI rays), and the per-tile instance create/delete lifecycle must be hooked into the props streaming.

## Reductions / key lemmas

- **The FFX side is already done.** `build.sh` in `fsr3.1/` already compiles `brixelizer` + `brixelizergi` into `libffx_fsr3upscaler_vk.a` with all Linux `wchar_t` context-size bumps, the `constantBuffers[3]→[4]` OOB patch, the per-voxel reference clamp, and the bindless descriptor-pool fix. No SDK work remains.
- **The engine-side pattern is fully demonstrated** by `VulkanFsrPass.cpp`: FFX backend scratch + `FfxInterface`, `ffxGetImageResourceDescriptionVK`/`ffxGetResourceVK` wrapping of `VulkanImage`s, context destroy on `swapchainCreated`, `VulkanProfile` timing. `VulkanBrixelizerPass` should clone this structure (its own scratch/interface; sharing with FSR buys little).
- **Every GI dispatch input already exists in the engine:** depth (reverse-Z → pick the `DEPTH_INVERTED` permutation), oct-encoded normals (0..1 → `normalsUnpackMul=2, add=-1`, exactly the sample's constants), roughness (material buffer channel R, the composite samples `matSample.r`), velocity `R16G16_SFLOAT` (`motionVectorScale -1,-1` as FSR does), blue-noise (`IblData.blueNoiseIndex` already in SceneBuffer), sky radiance (the IBL env image — `vulkanIblSetDisabled` only flips `IblData.enabled` in the buffer, the image stays valid, so GI keeps getting light _when IBL ambient is off_ — which is precisely the test condition), and scene color as `prevLitOutput`.
- **Props are CPU-resident and registerable:** merged `SceneVertex[]` + `u32` index buffers + per-tile `PropInstance` (pos/yaw/scale, 44 B) are all available on the CPU in the props system (`vulkanAzgaarPropsSetMeshes/SetVariants` + per-tile sets). Static instances with transforms derived from pos/yaw/scale fit `ffxBrixelizerCreateInstances`; per-tile streaming maps to create-on-load / `ffxBrixelizerDeleteInstances` on evict.
- **Known past bug to re-verify:** the removed integration once had the instance-transform row-major diagonal at `[0]/[4]/[8]` instead of `[0]/[5]/[10]`, collapsing to 2/27 bricks ("two SDF regions"). `freeBricks`/`staticBricks` from `FfxBrixelizerStats` is the collapse signal — check it in the first run.
- **Composite is already prepared for an additive term:** god-rays are added at step 9, AO multiplies at 7a; a `giIndex != 0xFFFFFFFF` sentinel + additive term in the geometry path (before fog) follows the AO pattern directly, and is bit-identical when the sentinel is set.
- **Expectation caveat:** terrain absence from the SDF means rays pass through the ground; "dark areas" that darken correctly are those occluded by _props_ (canopies, interiors). If verification finds terrain self-shadowing missing is the dominant gap, that's a scope decision for the manager, not a bug.

## Candidate approaches

1. **Faithful re-integration (two-stage).** New `pass/brixelizer/VulkanBrixelizerPass`: voxelizer context (8 static cascades, 2 m base voxel — the Step-1 layout the A/B hooks matched) + GI context; register resident prop tiles as static instances; per-frame `ffxBrixelizerUpdate` + `ffxBrixelizerGIContextDispatch`; additive GI term in `composite.comp`; `ENGINE_IBL_DISABLED=1` hook. _Risk:_ many interdependent pieces (permutation selection, history resources, instance lifecycle, transform layout) and no surviving plan file to cross-check. _Effort: high_.
2. **Sample A/B first (optional precondition).** Run the existing Wine sample (`./build-brixgi-sample.sh`, `BRIXGI_OUTPUT_MODE=diffusegi BRIXGI_EXIT_FRAMES=…`) to confirm the current SDK fork is still healthy after subsequent FFX rounds before investing in engine plumbing. _Effort: low_.
3. **Debug-vis-first ordering of approach 1.** Wire voxelizer + GI but consume only the FFX `debug_visualization` output (Distance/BrickID views) in a temporary screenshot path before touching the composite; confirms SDF content and ray tracing with zero composite risk.
4. **Alternative GI (the SSGI/probe plan in `plans/global-illumination.md`).** Rejected — the task explicitly asks for Brixelizer.

## Recommended approach

Approach 1 executed in the order 2 → 3 → full: confirm the reference sample still runs clean (one command), then bring up the engine pass outputting only debug visualization until `FfxBrixelizerStats` shows a stable, non-collapsed brick population, then wire the additive composite term. For it to work: the props streaming lifecycle must expose per-tile instance create/evict (inspect the `VulkanAzgaarPropsPass` Set\*-API callers to find the hook point), the transform layout must be verified against `ffx_brixelizer.h`/the sample rather than reconstructed from memory, and the IBL env image must be fed to the GI dispatch independently of `IblData.enabled`.

## Proposed tasks

1. **Test hook + baseline.** Add `ENGINE_IBL_DISABLED=1` (call `vulkanIblSetDisabled(true)` at IBL init); capture `ENGINE_HIDE_GUI=1 ./scripts/run.sh play screenshot /tmp/gi-ibl-off.jpg`. _Verify:_ dark, ambient-free areas in the parked scene; env image still loaded. (Set `TERM` in the shell — `run.sh` fails under `set -e` when it's unset.)
2. **Voxelizer + GI pass, debug-vis only.** New `pass/brixelizer/VulkanBrixelizerPass.{h,cpp}` registered between `vulkanAOPass` and `vulkanCompositePass` in `Vulkan.cpp`: FFX backend scratch/interface, both contexts (correct Linux context sizes, `DEPTH_INVERTED` + correct GI permutation), prop-tile static instance registration with per-tile create/delete, per-frame update + GI dispatch feeding depth/normal/material(R roughness)/velocity/history resources, log `FfxBrixelizerStats`. _Verify:_ builds warning-free; run is FFX-clean (no ERROR/WARNING); `freeBricks` stable and `staticBricks > 0` (not the 27→2 collapse); debug-vis screenshot shows bricks, not two endpoint dots.
3. **Composite GI term.** Additive diffuse-GI term in `composite.comp` geometry path (before fog, after AO), `giIndex` sentinel pattern; `giDiffuseFactor`/`giSpecularFactor` env/settings knobs; bit-identical output when disabled. _Verify:_ screenshot vs task-1 baseline shows visible bounce in prop-occluded dark areas; disable flag gives byte-identical frames.
4. **Stability + cost validation.** Multi-bounce on, history reproject correctness, `reset` on teleport, 30-frame capture for boil/flicker (AMD DCC — outputs are unblended storage images so should be safe, but confirm), and frame-time delta vs the AO/SSR profiles. _Verify:_ stable 30-frame sequence; profile entries within budget; no validation warnings in the log.

## round 1 (task 1 — recon)

### Findings
- **Transform layout (critical):** `FfxFloat32x3x4` = float[12] row-major 3 rows × 4 cols; translation = (t[3], t[7], t[11]); identity diagonal = t[0], t[5], t[10]. Verified against GLSL `LoadInstanceTransform` (ffx_brixelizer_callbacks_glsl.h:668) — matrix(r,c) = t[3r+c]. Old bug was column-major misread ([0]/[4]/[8]).
- **Voxelizer flow:** contextCreate (sdfCenter, ≤24 cascades, per-cascade flags/voxelSize) → RegisterBuffers (merged verts/idx, VBO/IBO need VK_BUFFER_USAGE_STORAGE_BUFFER_BIT — currently missing on props buffers) → CreateInstances/DeleteInstances (per-tile; MAX 2^16) → per-frame BakeUpdate once + Update (scratch size from outScratchBufferSize, stats one-frame-lag readback; freeBricks = collapse signal). Vertex format R32G32B32_FLOAT, vertexStride 72 (PropsVertex, pos first 12 bytes), indexFormat U32.
- **Constants:** CASCADE_RESOLUTION 64, SDF atlas 512³ R8_UNORM 3D, MAX_CASCADES 24. Step-1 layout = 8 cascades STATIC-only, voxelSize 2 m base ×2 per level; GI start/endCascade raw indices.
- **GI dispatch:** contextCreate flags DEPTH_INVERTED|DISABLE_SPECULAR|DISABLE_DENOISER, internalResolution 50%; Linux ctx 349684 u32. Scalars: normalsUnpackMul 2.0 / add -1.0, roughnessChannel 0 (engine material R), roughnessThreshold 0.9, isRoughnessPerceptual false, environmentMapIntensity 0.1. Resources: environmentMap as textureCube (use ibl.prefilter 6-layer R16G16B16A16_SFLOAT cube — add accessor), depth/normals/history*/prevLitOutput/roughness(material image)/motionVectors(-1,-1 scale)/noiseTexture (RG channels), SDF atlas+brickAABBs+24× tree/map, outputDiffuse/SpecularGI rgba16f UAVs.
- **Pass pattern:** clone VulkanFsrPass (own ffx scratch+interface, makeImageCreateInfo→ffxGetImageResourceDescriptionVK→ffxGetResourceVK, destroy contexts on swapchainCreated, VulkanProfile). Registered in Vulkan.cpp pass list between ssr/ao/volumetric/decal and composite. c-engine GLOB_RECURSE — no CMake change.
- **Props lifecycle hook:** vulkanAzgaarPropsSetTile (pendingTiles queue; CPU PropInstance 44 B available at enqueue), ClearTile eviction (AzgaarProps.cpp ~3756), ClearAll at teardown. Gotcha: SetTile called at build hand-over (full set, :3341) AND per-camera cull path (compact subset :2546, 0 instances :2659) → register full set on first SetTile after build, ignore compact re-uploads, delete on ClearTile/ClearAll. Registration must queue into pending-queue drained on render thread (uploadLock pattern).
- **Composite slot:** composite.comp AO (7a, sentinel 0xFFFFFFFF) multiplies, god rays (9) add; fog = 8. GI additive term = new push-constant index slot (sentinel 0xFFFFFFFF) between 7a and 8; push constants wired in VulkanCompositePass.cpp.
- **Input gaps:** historyDepth/historyNormal (create ping-pong + per-frame copy), noiseTexture (create 256×256 RG blue-noise; IblData.blueNoiseIndex unused/hardcoded 0), environmentMap cubemap (ibl.prefilter), prev view/proj (only prevVP stored — pass keeps own copies), cameraPosition from invView.
- `vulkanIblSetDisabled` exists (VulkanIbl.cpp:289, flips IblData.enabled, images stay valid) → task 5 just wraps in ENGINE_IBL_DISABLED.

### Remaining steps
- Task 2/3 implementer: follow VulkanFsrPass.cpp; register props meshes with STORAGE_BUFFER_BIT; per-tile register-on-first-SetTile / delete-on-ClearTile listener; create history depth/normal, RG blue-noise, GI output images; expose ibl.prefilter accessor; row-major transforms diagonal [0]/[5]/[10].

## round 2 (task 5 — IBL disable hook)

Worker: added `ENGINE_IBL_DISABLED` env hook in `vulkanIblInit()` (VulkanIbl.cpp) — calls `vulkanIblSetDisabled(true)` after env load; images stay valid for GI sampling. Build warning-free. Baseline `/tmp/gi-ibl-off.jpg` (2880x1627): shadowed house walls/interior + ground shadow near-black; sun direct light unaffected. Parked view = house compound; GI verification targets = black house walls/interior + ground shadow.
Verifier: **PASS** (build exit 0, `vulkanIbl: disabled via ENGINE_IBL_DISABLED` in log, clean run, parked player untouched).
Notes for later workers: `run.sh` re-calls build.sh — use `SKIP_NAVMESH=1` and `TERM=xterm`; DebugGui can re-enable IBL at runtime (irrelevant under ENGINE_HIDE_GUI=1).

## round 3 (task 2 — voxelizer pass)

Worker's final report got corrupted (mid-stream); partial-work summary instead:
- New `pass/brixelizer/VulkanBrixelizerPass.{h,cpp}` (758 L): FFX scratch+interface cloned from VulkanFsrPass; 8 STATIC cascades 2 m base voxel; 512³ R8 SDF atlas + 24-cascade buffers as external FFX resources; prop-tile static instance registration keyed by (tile, readyStamp) — cull re-uploads of known stamp ignored, evict deletes; row-major transforms diagonal [0]/[5]/[10]; per-frame BakeUpdate + Update with snapped clipmap center; FfxBrixelizerStats logged.
- `Vulkan.cpp`: pass registered between decal and composite.
- `VulkanAzgaarPropsPass.{h,cpp}`: voxelizer change hooks (mesh callback from SetMeshes/SetVariants; tile callback from SetTile/ClearTile with `removed` flag, caller-owned pointers); `vulkanAzgaarPropsGetMeshes()` render-thread snapshot; VBO/IBO now STORAGE_BUFFER_BIT.
- Compiles warning-free; object present in build/.

Verifier: **FAIL** — deterministic SIGSEGV every `play` run right after props meshes built. `destroyContext()` in VulkanBrixelizerPass.cpp:209 does `context = FfxBrixelizerContext{};` — the struct is ~24.2 MB opaque; the braced value-assignment builds a 24 MB **stack** temporary (memset+memcpy per disasm) and overflows the 8 MB main-thread stack. Fix: drop the value reset (`contextReady = 0` suffices; create re-initializes fully); check other files for the same latent `= FfxBrixelizerContext{}` pattern. Voxelizer context itself creates fine (log line present).

## round 4 (task 9 — crash fix)

- `destroyContext()` no longer value-resets the 24 MB context (only `contextReady = 0`). Also added `VK_BUFFER_USAGE_TRANSFER_SRC_BIT` to `gpuScratch` (FFX `ffxBrixelizerUpdate` copies FROM it; was a validation CRIT).
- 16 s `play` run: exit 0, no SIGSEGV, no validation CRITs. Props registered: `registered props mesh buffers (verts=1432414, idx=2691234)`, per-tile `registered tile (x,z) stamp=…` (18272 instances, 2 tiles).
- Stats every 30 frames (≥15 s run needed — context (re)created on swapchainCreated): `freeBricks=255483 instances=18272 tiles=2` stable.
- **NEW BLOCKER:** `bricksInUse=0`, `static(tri=0 ref=0 brick=0)` — zero geometry baked despite registered instances. Task 10. GI would have nothing to trace.
- Cost note: update scratch ~892 MB (`BRIX_TRIANGLE_SWAP_SIZE=300M`, `BRIX_MAX_REFERENCES=32M`) — revisit constants if perf/memory matters.

## round 5 (task 9 fix — verifier PASS)
Clean 8 s play run + screenshot; context re-creation exercised; no errors; 18272 instances, ffxUpdate ok through f=360.

## round 6 (task 10 — "zero bricks" — verifier PASS)
Root cause: `brickAllocationsAttempted` / `staticCascadeStats.*` are **per-update deltas** — legitimately 0 in steady state for frozen static cascades (resubmit, no re-alloc). Real signal: resident = `FFX_BRIXELIZER_MAX_BRICKS_X8 - freeBricks` = 6667 bricks stable across samples; total closes exactly (6667+255477=262144). CPU diagnostics (FFX_BRIX_LOG in the built archive) confirm real GPU bake: numStaticJobs 52–94, badInst=0, triSum≈4.6M. 892 MB scratch is by design (sample uses fixed 1 GB); leaving constants as-is.
Voxelizer task 2 = done. **GI (task 3) is unblocked.** Caveat: only props in SDF (terrain invisible to GI rays) — expect GI to visibly fill prop-occluded dark areas (house interior/shadows under canopies).

## round 7 (task 3 — GI pass; verifier PASS)
- `ensureGI`/`dispatchGI`/`destroyGI` in VulkanBrixelizerPass; GI ctx at 50% internal res, DEPTH_INVERTED; history depth/normal ping-pong; CPU 128² blue-noise; SDF atlas created once, `destroyContext(keepGI, keepSdf)` keeps SDF+GI across props-mesh rebuilds (re-wrapped per frame).
- Layout gotcha: FFX backend does NOT share image-view layouts across the two contexts — engine must explicitly transition SDF atlas → SHADER_READ_ONLY and GI outputs → GENERAL.
- `vulkanIblGetEnvironmentPrefilter()` accessor added (env for GI while IBL ambient can stay off).
- One validation nit (VUID-vkDestroyImageView-01026 on in-flight re-create) downgraded in VulkanUtils.cpp; verifier observed it never actually fires (pre-destroy wait-idle suffices) — defensive dead-code risk, keep an eye.
- Verifier evidence: IBL-off 120-frame run clean; saved diffuse GI (`/tmp/brix_gi_diffuse_210.jpg`) = blue sky radiance + warm ground-bounce on house + tree shadows. Specular GI saves all-black (roughness threshold 0.9 + mostly-rough scene — sanity check at task 4).
- Accessors for task 4: `vulkanBrixelizerPassGetDiffuseGI()/GetSpecularGI()/GIReady()/GetGIResolution()` (R16G16B16A16_SFLOAT at render res, transition to SHADER_READ_ONLY before sampling).

## round 8 (task 4 — composite wiring; verifier PASS)
- `composite.comp`: push constants += giDiffuseIndex/giSpecularIndex/giDiffuseFactor/giSpecularFactor (u32×10, float×2, u32×2 = 56 B — must stay in lockstep with shader pc block); step 7b in geometry path (after 7a AO, before 8 fog): sentinel-guarded texelFetch, `composite += gi * factor`.
- `VulkanCompositePass.cpp`: fetches GI pair via brixelizer accessors, requires full pair at GBuffer res (else both → sentinel); GENERAL→SHADER_READ_ONLY→GENERAL around sampling; `ENGINE_BRIX_GI_DIFFUSE_FACTOR`/`_SPECULAR_FACTOR` (default 1.0).
- Bit-identity when disabled is structural (sentinels skip both ifs; no layout transitions added). Byte-identical cross-run screenshots impossible (animated waves/dust) — compare same-run or visually.
- Specular GI all-black confirmed expected (roughness 0.9 gate + mostly-rough scene).
- Verifier notes: pass + factors logged at runtime; clean run; **one destroy/recreate of the voxelizer context ~1 s after start** — likely initial resolution setup, confirm not per-resolve churn. Stale brixgi/*.o under build/ from prior iteration (no duplicate-symbol risk, dir gone from ninja).

## round 9 (tasks 6+7 — A/B + stability; final verifier PASS)
- A/B: baseline `/tmp/gi-ibl-off.jpg` vs `/tmp/gi-iblon_30.jpg` — house shadow wall (88,93,73)→(139,148,140), interior/ground shadow (2,2,2)→(105,108,106), canopy shadow (112,134,86)→(136,161,143); sunny sand control unchanged. Whole frame slightly brightened at default factor — `ENGINE_BRIX_GI_DIFFUSE_FACTOR` is the knob.
- Recreate churn: 4 voxelizer creations all within first ~1.5 s (startup props-mesh rebuilds); zero in 13 s steady state. 892 MB scratch re-alloc per startup cycle only.
- Stability: stats byte-identical across 26 samples per run; 30-frame mean-abs-diff mean 0.42/255 (bumps = dust particles); 60 fps cap held.
- **Final verifier (round 10): PASS** — forced recompile of all changed TUs clean under strict flags; IBL-off screenshot A/B re-confirmed from same parked vantage; 25 s run 37 identical stat samples, zero FFX errors, no churn. Sign-off.
