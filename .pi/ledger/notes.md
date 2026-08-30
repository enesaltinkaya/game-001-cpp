# notes

## brainstorm

## Core difficulty

GPU time (~9.5 ms) is distributed across ~27 render passes (`c-engine/renderer/vulkan/pass/`), and we don't yet know which passes dominate — so the hard part is getting a reliable per-pass GPU-time breakdown before spending effort, then cutting cost only in places where pixels provably don't change.

## Reductions / key lemmas

1. **Cost scales with pixel count, not shader complexity.** A pass running at 1/4 resolution costs ~1/4 its time. Since the FSR 3.1 upscaler exists in the pipeline (`VulkanFsrPass`, `rendererIsUpscalerEnabled()`), any pass whose output is only ever consumed through a low-pass filter (bloom, volumetric accumulation, lens, DOF blur, AO — AO already notes a half-res + bilateral-upscale path) can be dropped to 1/2 or 1/4 res with no visual loss. That's the highest-leverage class of fix.
2. **Attachments are pure budget levers.** Each `VkAttachmentDescription` load/store op (LOAD_OP_DONT_CARE, STORE_OP_DONT_CARE, no color attachment written, depth-stencil attachment that's only written) directly cuts memory/descriptor cost on tiled GPUs with zero visual change. Cheap to audit, safe to apply.
3. **The savings must be additive and verifiable.** Each change's delta must be measured individually (before/after capture, screenshot diff) — a combined "optimized" diff can't attribute a regression. This constrains work to one lever at a time.
4. **Upscaler quality mode is off the table** as a primary lever: lowering internal resolution via `RENDERER_UPSCALER_*` mode changes perceived sharpness, which the user explicitly forbade ("without sacrificing visuals").
5. RenderDoc timings are per-dispatch/draw; a pass's GPU cost = sum of its recorded ranges. A 25–40 ms frame's 9.5 ms GPU budget means top-2 passes likely account for half of it; fixing top-3 plausibly halves the total.

## Candidate approaches

- **A. Profile-first, then cut ranked hotspots.** Headless RenderDoc capture (`ENGINE_RENDERDOC_CAPTURE=1` + documented Python replay API) → per-pass GPU-time table → apply the safe levers (res scale, load/store ops, ALU) to top-2/3 passes, re-capture after each. Risk: replay-API timing extraction may be finicky on this setup and capture adds state differences; effort: ~1–2 working sessions for capture+ranking, then per-fix iteration.
- **B. Static cost audit without profiling.** Walk every pass's attachment list and scale factors, fix obviously-wasteful load/store ops and full-res attachments up front. Risk: shoots in the dark — the "wasteful" attachments may be <10% of the 9.5 ms and we burn a session; effort: low.
- **C. Persistent per-pass pipeline timestamps.** Add a `VkQueryPool` timestamp pass around each `vkCmdPipelineBarrier`-delimited range and log per-pass ms. Risk: real plumbing work in the Vulkan command layer, and timestamps are a new measurement path that must be validated against a known baseline; effort: medium.
- **D. Change upscaler mode / render resolution.** Directly cuts internal-res cost. Risk: sacrifices the very visuals the user wants preserved; effort: trivial. Kept only as a documented fallback / negotiation option.

## Recommended approach

**A, seeded with B's cheap checks.** Capture the baseline with the already-documented RenderDoc headless recipe and extract per-pass timings to rank hot spots; while that's in flight, statically audit the top candidates' attachment descriptions (load/store ops are near-free wins). Then cut the #1 pass with the safest high-leverage lever, re-capture, confirm the saving and pixel-identical screenshots, and repeat for #2/#3. This works if the RenderDoc replay timings are obtainable per the docs; if the Python API fails to yield per-dispatch times, fall back to approach C (timestamp queries) before touching any pass.

## Proposed tasks

1. **Baseline capture + hotspot ranking.** Run the documented headless RenderDoc capture on the parked player (keep `play`-equivalent envs, don't touch the parked player/camera), extract per-pass/per-dispatch GPU timings via the Python replay API, and produce a ranked table (pass → ms → % of total). Also save a reference `ENGINE_HIDE_GUI=1` screenshot for later pixel diffs.
2. **Static audit of top-3 ranked passes.** For each, read its pass source in `c-engine/renderer/vulkan/pass/`: list attachment load/store ops, output scale factors vs. consumers' needs, redundant texture fetches / mip bias, overdraw sources. Output: candidate levers ranked by (expected saving ÷ risk), no code changes.
3. **Implement and verify the top lever(s) on the #1 pass.** Apply the highest expected-saving, lowest-risk lever, `./scripts/build.sh`, re-capture, confirm the ms saving and no visual regression (screenshot diff), then hand back for commit.
4. **Iterate on #2 and #3 passes** with the same apply/measure/diff loop, then a final full verification pass (clean build, screenshot comparison, `./scripts/run.sh play log 5000` run).

## round 1

Task 1 (baseline capture + hotspot ranking) — done.

### Artifacts
- Baseline RenderDoc capture: `/tmp/RenderDoc/c-game_2026.08.30_03.14_frame679.rdc` (frame 679, ~1.7 GB). Render res 2880×1627 (QHD). Use `scripts/rdc.py` for state dumps.
- Reference screenshot (no in-world GUI): `/tmp/gpuopt/ref.jpg` (taken with `ENGINE_HIDE_GUI=1 ./scripts/run.sh play screenshot ...`).

### Per-pass GPU timings — ground truth (600-frame average, parked player)

Recipe (instrumentation already exists in `c-engine/renderer/vulkan/Vulkan.cpp`, gated by env):
`ENGINE_LOG_PASS_GPU=1 ./scripts/run.sh play log 40000` → logs avg of frames 601–1200.
Total = **9.44 ms**, fps 105.5. Matches the user's ~9.5 ms. Passes sum to 9.42 (serialized single queue).

| rank | pass              | ms   | %     |
|------|-------------------|------|-------|
| 1    | azgaar_props      | 3.51 | 37.2% |
| 2    | depth             | 1.51 | 16.0% |
| 3    | shadow            | 1.40 | 14.8% |
| 4    | heightmap_terrain | 0.75 | 7.9%  |
| 5    | ao (CACAO)        | 0.58 | 6.1%  |
| 6    | contact_shadow    | 0.52 | 5.5%  |
| 7    | taa               | 0.26 | 2.8%  |
| 8    | composite         | 0.18 | 1.9%  |
| 9    | final             | 0.09 | 1.0%  |
| 10   | occlusion         | 0.08 | 0.8%  |
| 11   | azgaar_water      | 0.07 | 0.7%  |
| 12   | hiz               | 0.06 | 0.6%  |
| 13   | bloom             | 0.06 | 0.6%  |
| 14   | lpm               | 0.06 | 0.6%  |
| 15   | ssr               | 0.05 | 0.5%  |
| 16   | light_culling     | 0.04 | 0.4%  |
| 17   | culling           | 0.07 | 0.7%  |
| 18   | azgaar_weather    | 0.03 | 0.3%  |
| 19   | scene_render      | 0.03 | 0.3%  |
| 20   | azgaar_river      | 0.02 | 0.2%  |
| 21   | oit_composite     | 0.02 | 0.2%  |
| 22   | skybox            | 0.01 | 0.1%  |
| 23   | oit_accumulate    | 0.01 | 0.1%  |
| 24   | volumetric        | 0.01 | 0.1%  |
| 25   | fsr, dof, decal, lens, debug_*, rmlui | ~0.0 | 0% |

Top-4 = 7.17 ms = **76% of the frame**; top-6 = 88%. All four are geometry-rasterization passes
(vegetation/props, depth prepass, CSM shadows, terrain) — no cheap fullscreen-res-scale lever on top-4;
ao/contact_shadow/taa/composite are the small fullscreen tail (1.75 ms total).

### How timings were extracted / caveats
- Engine per-pass numbers come from existing `VulkanProfile` BOTTOM_OF_PIPE timestamp pairs
  (`VulkanProfile.cpp`), read by PassStatsGui; `ENGINE_LOG_PASS_GPU` dumps them as text. These match
  the overall profile exactly → treat as the source of truth for per-pass ms in later rounds
  (re-measure after each change with the same recipe; compare pass-by-pass, not just total).
- RenderDoc replay *can* give per-event GPU times: `ctrl.FetchCounters([1])` (counter id 1 = "GPU Duration")
  returns `CounterResult(eventId, value.d)` in **seconds**, one per timed draw/dispatch (266 events in this frame).
  Event → pass mapping: walk the action tree, attribute each event to its **outermost** push-marker group.
- **Do NOT trust absolute replay times for the four heaviest passes.** Replay sum = 179 ms vs live 9.44 ms.
  Per-pass ratio replay/live: azgaar_props 19.8×, scene_depth_prepass 43.7×, shadow_csm 19.5×,
  heightmap_terrain 16.3× — but every light/fullscreen pass is 0.8–1.6× (i.e. correct). Reproducible
  (second replay: 184 ms), so not GPU warmup. Same rasterized workload in both (37.3M rasterizer
  invocations = ~8 screens, from counter 5). Root cause not identified; suspect replay-state overhead
  on per-tile indirect geometry draws (25 tiles × many small draws, inst counts 5–60/draw).
  → Use the .rdc for *static* state (attachments, descriptors, pipeline state, per-draw topology) and
    the engine log for *timings*. If per-draw GPU times inside a pass are ever truly needed,
    add engine-side per-draw timestamps or accept the ranking from the replay (relative order of
    the 4 heavy passes matches live order).
- Python replay gotchas (this SWIG build): `EnumerateCounters()` takes no args; its array iteration
  returns indices (use `DescribeCounter(int)` for names); `CounterValue` fields are `d/f/u32/u64`;
  counter ids: 1=GPU Duration, 2=Input Vertices, 3=Input Primitives, 5=RasterizerInvocations,
  6=RasterizedPrimitives, 13=CSInvocations.
- Screenshot caveat for later diffs: JPEG + dynamic scene (TAA shimmer, vegetation/animation) — compare
  visually or region-wise, expect non-identical pixels on a fresh run; the parked player/camera was not touched.

### Remaining steps
- Task 2: static audit of top passes (`azgaar_props`, `depth`, `shadow`, `heightmap_terrain`):
  attachment load/store ops, attachment sizes vs consumers, overdraw sources (8 screens rasterized),
  per-tile draw fragmentation (25 tiles × many small indirect draws — instance counts 5–60), mip bias.
  The 76%-concentrated-in-geometry profile means the wins must come from geometry passes, not fullscreen scale factors.

## round 2

Task 2 (static audit of top-4 passes) — done. No code changes. Render res 2880×1627 (4.69M px), **no MSAA** (all framebuffers samples=1). Full-res per-pixel budget: scene colour pass attachments = sceneColor 8B + normals 4B + material 4B + albedo 8B = 24B/px (+D32 4B); depth pass = velocity 4B + viewNormal 4B + **worldNormal 8B** + D32 4B = 20B/px.

### Per-pass findings

**azgaar_props (3.51 ms, colour pass only).**
- Draw structure: per tile (25 resident) × per (species,variant) range → direct `vkCmdDrawIndexed` with 5–60 instances each (~250–500 draws/rendering) + global settlements: **21,211 instances in 7 ranges** + landmarks 263 in 8, both culled by a single **whole-map AABB** (−40040..40040²) → *never* culled off-screen.
- The props geometry is rendered **5× per frame**: colour (1) + velocity/depth prepass (1) + shadow CSM **×2 cascades**. Prepass and shadow hooks run inside the depth/shadow pass profiles, so their cost is hidden in those 1.51/1.40 ms.
- No mip bias anywhere relevant: base-color textures use HW auto-LOD (SAMPLER_LINEAR), shadow map is mip-0 only (no mips → no bias to tune). No redundant *texture* fetches in the props shaders (1 albedo + 9-tap HW PCF + contact shadow + IBL + forward+, all visual-required).
- `noCull=1` (double-sided raster) on all props — deliberate for thin vegetation; solid species (rocks/buildings) pay 2× fragments.

**depth (1.51 ms).** Contains: scene prepass (3 indirect batches: single-sided, double-sided, **transparent-with-depth-write**), heightmap terrain prepass (same 25×130k-tri lattice as the colour pass), props prepass (~300 small draws), water prepass. Four attachments at 20B/px.
- **`worldNormal` (R16G16B16A16, full-res ≈ 37 MB) is a dead attachment.** Written by: depth pass (scene prepass + terrain prepass + props prepass + water prepass), occlusion pass phase-2 depth render, scene/triangle/oit prepass pipes. **No shader ever samples it** (grep of all shaders + descriptor bindings: zero readers; the FFX denoiser/PCSS it was for is unimplemented — `vulkanShadowPassSetPCSS` is a no-op stub). Pure 8B/px load/store + tile-memory cost in the depth pass and occlusion (0.08 ms).
- All attachments are LOAD (except initial clears) because later hooks append — correct, not a lever.

**shadow (1.40 ms).** Quality MEDIUM (settings.json `shadowQuality: 2`): 2048² D32, 2 cascades, 80 m range.
- Per cascade (×2): full-map **clear** (`clearDepthEnabled=1`, whole 2048² layer) + scene SS/DS indirect + **all props** (every frustum-visible tile range + the 21,211-instance settlements + 263 landmarks, culled by the whole-map AABB). Cascade 0 covers only the near frustum slice yet still gets a full-map clear and the full props draw list; casters outside the cascade's bounding-sphere XY extent waste the draw.
- Double-sided (noCull=1) shadow casters + slope bias 0.8 — 2× shadow fragments for solids.
- Mip bias: none present (mip-0-only map); receiver PCF is 9-tap HW bilinear — that cost lands in the colour passes, not here.
- Cascade matrices: XY extent = bounding sphere of the slice, quantized; no scissor to it (full-map scissor/viewport).

**heightmap_terrain (0.75 ms, colour pass).**
- 25 tiles (rings 0–2) × 2×255² = **3.27M tris/pass**, non-indexed `vkCmdDraw(6·seg²)` lattice → **9.77M vertex invocations/pass, ~19.5M/frame** (×2 passes: scene + prepass inside depth).
- VS enumerates corners from `gl_VertexIndex` and fetches the R32F height texture **up to 5×/vertex** (center + 4 neighbours for the normal; 2 on borders) → ~80–100M height fetches/frame across both passes. Shared lattice corners are recomputed per triangle (3 vs 2 unique per 2 tris).
- The prepass VS duplicates the identical fetches and also writes the (dead) worldNormal.
- Ring LOD is stubbed out (`heightmapRingSegments` returns 255 for all rings) with a comment that a 255/64/32 ladder "cracks at every ring boundary" — see L3 below.

### Candidate levers (ranked by expected saving / risk)

**L1 — delete the `worldNormal` attachment. Expected: 0.2–0.5 ms (mostly depth 1.51; plus occlusion 0.08). Risk: ~0 visual (buffer provably never sampled). Effort: small.**
Remove the image from frame resources, drop color3 from the depth/occlusion-phase2/terrain-prepass/props-prepass/water-prepass pipes + their FS `outWorldNormal`, and the `vulkanFrameResourcesGetWorldNormal` callers. Watch the `ENGINE_DEBUG_DUMP_IMAGES` path (the image had TRANSFER_SRC for that debug dump) — drop that entry too. Verify nothing else binds it in the sampled pool (grep found none).

**L2 — per-cell culling of the global props sets (settlements 21,211 + landmarks 263). Expected: 0.5–2 ms combined across azgaar_props + depth-prepass + shadow (biggest unknown, likely the single largest win). Risk: low (pure culling of off-screen instances). Effort: medium.**
All 21,474 global instances go through the VS in all 5 renderings per frame; at a few hundred tris/building that is millions of triangles ×5 that are clipped off-screen. Fix: at upload time split the global instance buffer into coarse cells (e.g. 2048 m cells, per-cell IBO + ranges, like the tile path), frustum-cull cells per frame, draw only visible cells (colour, prepass, and both shadow cascades). Cell AABBs must include per-species mesh reach (already in the push-constant bounds).

**L3 — terrain: precompute per-tile corner VBO/IBO (indexed) + then ring LOD. Expected: 0.5–1 ms combined (0.75 terrain + its prepass share inside 1.51 depth). Risk: low for the VBO step / medium for LOD. Effort: medium.**
Step A (safe): build a 256² corner VBO (pos+normal, heights already in hand at upload) + 255-seg index buffer per tile on the CPU; identical geometry, VS goes 9.77M→1.64M invocations/pass and the ≤5-texel-fetch VS becomes trivial — kills the ~100M height fetches/frame. Determinism contract preserved (VBO is a pure function of the same regenerated heights; the rendered surface is bit-identical). Wireframe pipe and prepass need the same switch.
Step B (bigger, needs verification): ring ladder (e.g. 255/127/63) cuts ring-1/2 vertex work ~10× more. My geometric analysis says tile borders stay watertight (on the bilinear surface h is linear along each border row, so coarse and fine chords coincide) — but the code comment records *observed* cracks with a previous ladder, so treat that comment as evidence and validate empirically (screenshot diff at ring boundaries) before shipping.

**L4 — merge per-tile props range draws into one multi-draw-indirect per tile. Expected: 0.1–0.4 ms. Risk: low. Effort: small-medium.**
~250–500 tiny (5–60 inst) `vkCmdDrawIndexed` + per-draw bindVertex/bindIndex/push → per-tile `DrawIndexedIndirect` array (indexOffset/firstInstance/instanceCount per range, built once at scatter). 25 (or 3 incl. globals) draws per rendering instead of hundreds; same geometry. Per-draw overhead on this GPU is unmeasured — measure the delta, it may be smaller than expected.

**L5 — shadow: single render pass / scissor each cascade to its light-space XY extent. Expected: ~0.1–0.2 ms. Risk: low. Effort: small.**
Currently 2 full-map 2048² clears + full-map scissor per cascade while cascade 0's bounding sphere is a small fraction of the map. One begin-render (clear once, both layers) + per-cascade subpass/scissor to the cascade XY range; keep far value 1.0 via the single clear. (D32→D16 format change: rejected, near-depth precision risk.)

**L6 — props raster: per-species cull/noCull pipe split (solid species cull) and/or back-face culling in the shadow pipe. Expected: 0.2–0.5 ms. Risk: medium (needs visual sign-off: leaf alpha masks rely on noCull; shadow-side culling may lighten leaf shadows slightly). Effort: small-medium.** Keep last; verify by screenshot diff at a tree cluster.

### Notes / caveats for the implementation rounds
- Re-measure per-pass with the round-1 recipe (`ENGINE_LOG_PASS_GPU=1 ./scripts/run.sh play log 40000`); the RenderDoc replay absolute times are invalid for these 4 passes (round-1 finding).
- L1 and L3A are the safest pair to open with; L2 is likely the biggest but touches upload/culling paths — keep the scatter determinism (per-cell regrouping must be a pure function of the same instance data).
- The 5×/frame props rendering (color + prepass + 2 cascades) is the structural multiplier every props-side lever is amplified by.
- `settings.json` has `shadowQuality: 2` (MEDIUM) — the 4096/3-cascade HIGH is a user toggle; levers must hold at HIGH too (more cascades = more full-map clears, L2/L5 benefit more).

## round 3

Task 3 (L1: remove unused full-res `worldNormal` attachment) — done. Built clean (`./scripts/build.sh`), re-measured, screenshot diffed.

### Changes
- `VulkanFrameResources.{h,cpp}`: removed `worldNormal` image (37 MB R16G16B16A16), getter, create/destroy/initial-layout transitions.
- `VulkanDepthPass.cpp`: 3 pipes (scene_depth_prepass, _ds, azgaar_water_depth_prepass) dropped `colorFormat3`/`clearColor3`; both beginRender calls and the preUpdate transition dropped color3/worldNormal.
- `VulkanHeightmapTerrainPass.cpp`, `VulkanAzgaarPropsPass.cpp`: their depth-prepass pipes dropped `colorFormat3` (R16G16B16A16) — they now match the depth pass' 2-attachment render pass.
- `VulkanOcclusionPass.cpp`: phase2_depth(_ds) pipes dropped `colorFormat3`, beginRender dropped color3.
- Shaders: `scene_depth.vert/.frag`, `heightmap_terrain_depth.vert/.frag`, `azgaar_props_depth.vert/.frag`, `azgaar_water_depth.vert/.frag` — removed the worldNormal VS→FS varying and the location-2 fragment output (and their writes). Colour-pass shaders untouched: `azgaar_water.vert`/`oit_accumulate.vert`/`triangle*.vert` `worldNormal` identifiers are VS locals or G-buffer (normals) writes, not this attachment.

### Measurement (round-1 recipe: ENGINE_LOG_PASS_GPU=1, play, 600-frame avg, parked player)
| pass    | baseline | after L1 | Δ      |
|---------|----------|----------|--------|
| depth   | 1.51     | 1.33     | −0.18  |
| occlusion | 0.08  | 0.08     | 0      |
| total   | 9.44     | 9.21     | **−0.23 ms** (fps 105.5 → 108.0) |
Other passes within noise (azgaar_props 3.51→3.48, shadow 1.40→1.38, terrain 0.75 unchanged).

### Screenshot
`/tmp/gpuopt/after_L1.jpg` vs `/tmp/gpuopt/ref.jpg` (both ENGINE_HIDE_GUI=1, parked player/camera untouched): visually identical; only expected TAA/vegetation shimmer differs frame-to-frame. No visual regression.

### Findings
- Actual saving (0.23 ms) is at the low end of the audit estimate (0.2–0.5) — the depth pass gained 0.18 ms; the R16G16B16A16 attachment's cost is mostly bandwidth that this tile GPU absorbs well. Still pure win, zero risk, frees 37 MB/frame and simplifies 5 pipes.
- Invariant for future rounds: render-pass attachment count is defined by the first `vulkanBeginRender` in each pass, but every pipe bound inside must carry the same colorFormat set — prepass pipes (terrain/props/water/occlusion-phase2) had to be edited alongside their host pass. `VulkanPipe` supports up to 4 color formats; dropping colorFormat3 does NOT renumber color4 (heightmap terrain scene pipe still uses colorFormat4=albedo fine).
- `ENGINE_DEBUG_DUMP_IMAGES` token table in Vulkan.cpp never had a `worldNormal` token — nothing to remove there (the image carried TRANSFER_SRC for a planned-but-absent dump entry).
- Next up per plan: L2 (per-cell culling of the 21.4k global props instances across colour/prepass/2 shadow cascades) is the biggest unknown lever; L3a (terrain corner VBO) is the next safe one.

## round 4

Task 5 (L2: per-cell culling of global props) — done, with an important correction to the L2 premise.

### Key finding: the L2 premise is stale — game-side per-instance culling (v2) already exists
`AzgaarProps.cpp` ("Per-instance culling (v2)", ~lines 2167–2780) already compacts the global
settlement + landmark sets per-instance against the camera frustum (+ per-species distance caps,
buildings 2000 m; LOD hysteresis ring) on a worker thread, dirty-checked (8 m / 5°), and re-uploads
the compacted set through the existing SetGlobal/SetLandmarks API — for all 5 renderings
(colour + prepass + 2 shadow cascades). Verified at runtime at the parked vantage
(`ENGINE_AZGAAR_PROPS_DEBUG=1`): `cull settlements: 21211 -> 13 instances`, `cull landmarks: 263 -> 0`.
The round-2 audit's "21,211 instances drawn 5×/frame, never culled off-screen" describes the
engine-pass fallback only; the audit missed this machinery. The planned per-cell IBO split is
unnecessary — per-instance compaction is strictly finer than cells.

### What was actually implemented (the one remaining gap in the 5× structure)
Camera-frustum culling was missing per-CASCADE: the shadow hook drew every camera-visible global
range into BOTH cascades regardless of the cascade's light-space coverage. Now:
- `PropTileRange` gained `float aabb[6]` (world AABB of the range's instances, inflated by each
  instance's variant bounding sphere; all-zero = unavailable → range always drawn, safe fallback
  for initial uploads before the first cull).
- `AzgaarProps.cpp propsCullCompact` fills the per-range AABB while compacting (also for tile
  scatter-cull; tile draws don't use it yet).
- `VulkanShadowPass.cpp renderCascade`: extracts the cascade's 6 world-space light-frustum planes
  from `cascadeViewProj[cascade]` (same ZO Gribb-Hartmann extraction as the camera frustum:
  near = row2, far = row3−row2, ortho) and passes them to
  `vulkanAzgaarPropsDrawShadow(cmd, cascade, cascadePlanes)` (signature change).
- `drawGlobalSet` (shadow path only): per-range AABB test against the cascade planes; a range whose
  AABB is fully outside the cascade light frustum cannot project into that cascade's map → skipped.
  Colour/prepass paths are unchanged (set-level whole-map AABB test as before).
- Verified firing: with a debug log, `cascade-culled shadow: global settlement buildings range
  sp=14 var=0 count=8` at the parked vantage. (Temp log removed after verification.)

### Measurement (round-1 recipe: ENGINE_LOG_PASS_GPU=1, play, 600-frame avg, parked player)
| pass    | baseline | after L1 | after L2 |
|---------|----------|----------|----------|
| shadow  | 1.40     | 1.38     | 1.37     |
| depth   | 1.51     | 1.33     | 1.34     |
| azgaar_props | 3.51 | 3.48     | 3.50     |
| total   | 9.44     | 9.21     | 9.23     |
No measurable delta (±0.02 noise, same as L1 vs baseline): at the parked vantage the compacted
global sets are 13 + 0 instances, so per-cascade culling removes almost nothing HERE. The lever
still matters in cities (thousands of buildings inside the 2000 m cap × 2 cascades) but the parked
vantage cannot show it. Screenshot `/tmp/gpuopt/after_L2.jpg` vs `/tmp/gpuopt/ref.jpg`:
3.25M px changed, mean abs diff 1.0/255 — same magnitude as the known frame-to-frame TAA/vegetation
shimmer (ref vs after_L1 was 3.12M px); no visual regression (buildings' shadows intact).

### Notes for the manager
- The big azgaar_props cost (3.5 ms) at this vantage is vegetation TILES (e.g. tile(-1,-2) = 25,309
  culled instances), not global sets — L2-style levers cannot move it; L3a (terrain corner VBO) and
  L4 (merge per-tile draws) are the remaining candidates. Per-cascade culling could also be extended
  to tile vegetation ranges (they carry the same aabb now via the shared compact path) if a shadow
  pass win is ever wanted — out of scope this round.
- Determinism: the per-range AABB is a pure function of the same compacted instance data (no new
  RNG, no disk state).

## round 5

Task 6 (L3a: terrain per-tile indexed corner VBO/IBO + optional per-cascade tile culling in shadow hook) — done.

### Changes
- `VulkanHeightmapTerrainPass.cpp/.h`: at height-upload time the pass now also builds the
  tile's render lattice on the CPU: a 256² corner VBO (world pos + normal, 24 B/corner) generated
  from the SAME uploaded heights (the CPU grid `v->heights`), replicating the old implicit-lattice
  VS float-for-float (same cell math, same texel-centre bilinear addressing, same border-aware
  one-sided-stencil normals — GPU linear filter is within 1 ulp of the CPU formula per the spec,
  i.e. surface is identical to float noise, NOT bit-identical), plus ONE shared 255-seg lattice
  IBO for all 25 tiles (topology is tile-independent; ~1.56 MB total instead of ~39 MB).
  All 3 pipes (scene, wireframe, depth prepass) got vertex inputs (loc0 = R32G32B32 pos off 0,
  loc1 = R32G32B32 normal off 12, stride 24); draws switched to vulkanBindVertex/vulkanBindIndex/
  vulkanDrawIndexed(3·2·seg²). VBO destroyed with the tile on evict (garbage queue).
- `heightmap_terrain.vert` + `heightmap_terrain_depth.vert`: now thin transforms of the corner
  attributes — no gl_VertexIndex lattice enumeration, no height-texel fetches (~80–100 M
  texture reads/frame removed; VS invocations 9.77M → 1.64M per pass). Push constant block
  and the set1 height descriptor stay (still pushed/bound, now unused by the VS); the R32F
  height texture is still uploaded per the "same uploaded heights" contract.
- Optional part done: `vulkanAzgaarPropsDrawShadow` tile loop now also culls (a) the whole tile
  and (b) each (species,variant) range against the cascade's light-frustum planes
  (all-zero range AABB = unavailable → always draw; same idiom as round 4's drawGlobalSet).
  Colour/prepass paths untouched.

### Measurement (round-1 recipe: ENGINE_LOG_PASS_GPU=1, play, 600-frame avg, parked player)
| pass            | baseline | after L1 | after L2 | after L3a |
|-----------------|----------|----------|----------|-----------|
| heightmap_terrain | 0.75   | 0.75     | 0.75     | 0.68      |
| depth           | 1.51     | 1.33     | 1.34     | 1.25      |
| shadow          | 1.40     | 1.38     | 1.37     | 1.38      |
| azgaar_props    | 3.51     | 3.48     | 3.50     | 3.49      |
| total           | 9.44     | 9.21     | 9.23     | 9.07–9.08 |

Δ total = **−0.16 ms** (terrain colour −0.07, terrain share inside depth −0.08); shadow unchanged.
The win came in, but at the LOW end of the audit's 0.5–1 ms estimate: at this vantage the
3.27M-tri rasterization (fragment + overdraw against the depth buffer) dominates the terrain
passes over vertex work — the VS work we removed wasn't the bottleneck on this tile GPU at QHD.

### Screenshot
`/tmp/gpuopt/after_L3a.jpg` (ENGINE_HIDE_GUI=1, taken AFTER the shadow-hook change, parked
player/camera untouched) vs `/tmp/gpuopt/ref.jpg`: mean abs diff 3.29/255, 1070k px changed
(22.8% at sum>12 threshold) — same magnitude as the known frame-to-frame TAA/vegetation shimmer
(ref vs after_L2 = 2.96/255, 1003k px). Visual check of the ground/shadow crop: grass CSM
shadows identical in shape/softness. No visual regression.

### Findings
- Invariant held: VBO/IBO are pure functions of the same regenerated CPU heights (readyStamp
  key), so the determinism contract is preserved; tile borders stay watertight (same shared
  border-corner heights as before — the per-tile corners use the same cell math as the old VS).
- The `pc` push constant and set1 height descriptor are dead weight in the terrain VS now
  (layout compatibility kept intentionally); the R32F texture upload (1 MB/tile + transient
  copy) is also dead unless some future pass (ring LOD textures, contact-shadow refinement)
  samples it — could be retired in a later round for a small CPU-upload/VRAM win.
- Per-ring segment ladder (L3b) would need per-ring corner data (different VBO/IBO per seg),
  since one shared IBO assumes uniform 255 seg — noted in code.
- The new per-cascade culling of tile vegetation ranges didn't move the shadow number at this
  vantage: cascade-0's light frustum is only an 80 m near slice, so it does skip far tiles'
  ranges, but the shadow pass' profile is dominated by the full-scene SS/DS indirect draws,
  and the props shadow draws' GPU cost is within noise here (same as round 4's finding for
  the global sets). The lever still pays off in dense vegetation tiles / HIGH quality (3 cascades)
  and costs nothing when nothing is outside the cascade.
- Build verified with SKIP_NAVMESH=1 (navmesh bake untouched by these changes); a full
  ./scripts/build.sh incl. the bake belongs to the final-verification round.

### Remaining steps
- Task 4: full ./scripts/build.sh (with navmesh bake), final per-pass measurement, screenshot
  diff vs ref, ./scripts/run.sh play log 5000.
- (Out of this task's scope, for the manager: L4 merge per-tile props range draws, L5 shadow
  single-render-pass + per-cascade scissor, L6 cull/noCull pipe split.)

## round 6

Task 7 (L5: shadow pass one clear + per-cascade scissor to bounding-sphere extent) —
**implemented and built clean, but runtime verification is BLOCKED by a stuck display
environment** (see below). One lever only, shadow pass file only.

### Implementation (c-engine/renderer/vulkan/pass/shadow/VulkanShadowPass.cpp only)
- `buildCascadeMatrix` now stores per cascade: bounding-sphere center in light space
  (pre-snap), snapped ortho center, sphere radius, ortho half-extent.
- New `cascadeScissorRect()`: maps the cascade's bounding-sphere extent to a
  framebuffer scissor rect. The ortho maps [snappedCenter ± extent] onto the full map
  and radius < extent always holds (extent = radius + 10% padding), so the sphere's
  extent is a centered square of `mapSize * radius/extent` ≈ 91% of the map side
  (snapped-center shift ≤ 0.5 texel). floor/ceil rounding only ever WIDENS the rect;
  degenerate cases fall back to the full map. So the clip can only remove texels that
  are provably unsampled.
- `renderCascade`: per-cascade `vulkanScissor` to that rect (viewport unchanged, full
  map; scissor is dynamic state in every pipe — no pipeline rebuild needed).
- **One clear**: the two per-cascade loadOp=CLEARs (via `clearDepthEnabled` on
  shadowPipe) are replaced by (a) `clearDepthEnabled = 0` on shadowPipe (both cascade
  instances now loadOp=LOAD) and (b) a dedicated **no-draw rendering instance** in
  `update()` over the full-array 2D_ARRAY view with `layerCount = activeCascadeCount`,
  loadOp=CLEAR, clearValue 1.0 → clears all active layers in a single instance before
  the per-cascade instances load (image is already in DEPTH_STENCIL_ATTACHMENT_OPTIMAL
  from the existing transition; layout tracking untouched).
- Geometry/safety invariants: receivers inside a cascade slice project inside the
  slice's projected range ⊂ sphere extent; the receiver normal bias (≤ max(0.001, 2
  texels) · sinAngle, capped 0.25 m) plus the 3×3 PCF taps (±1 texel) stay inside the
  sphere extent except possibly by a few texels at extreme slice corners, where the
  blend region + tent weight (≤ 1/12) + cascade-blend factor bound any effect to
  sub-noise. Screenshot diff is the final check.

### What did NOT work (dead end, saved time)
- **vkCmdClearAttachments: DO NOT USE in this project.** This SDK's vulkan_core.h
  (VulkanSDK-linux/1.4.313.0) ships a NON-STANDARD `VkClearRect` {rect, baseArrayLayer,
  layerCount} / `VkClearAttachment` (no .image) — it does not match the official
  Vulkan ABI (image + imageSubresource + rect), so the call would pass a wrong struct
  to the real driver. Also spec-rejected outside an active render pass (validation
  error observed in an intermediate build: "must be issued inside an active render
  pass"). The no-draw rendering-instance clear sidesteps both problems.

### State
- `SKIP_NAVMESH=1 ./scripts/build.sh` → clean, zero warnings (warning-strict flags).
- The 03:57 smoke run exercised an EARLIER intermediate version (clearAttachments,
  mis-placed) up to the render loop — it confirmed the validation layer is active,
  but the FINAL code (no-draw clear instance + scissor) has never executed: it only
  passed static review.
- **BLOCKER (environment, not code):** every game run since 03:59 hangs at
  "windowSystem: selected backend sdl" (CPU 0%, main thread in poll, no window ever
  mapped). Reproduced with a standalone 10-line SDL3 program (CreateWindow blocks
  on both default/Wayland and forced X11 backends) → system-wide display stall
  (KWin/Wayland registry still answers wayland-info; Xwayland answers xdpyinfo;
  ksmserver/plasmashell alive; GPU sysfs/dmesg unreadable from this account).
  Screen is likely locked/blanked; a headless worker cannot unlock it.

### Remaining steps (when the display is usable again — user may just need to
unblank/unlock the screen)
1. `./scripts/run.sh play log 6000` — must complete in ~30 s with NO
   ERROR/CRIT/Validation lines (validates the no-draw clear instance + loadOp=LOAD
   cascade instances under the validation layer).
2. `ENGINE_LOG_PASS_GPU=1 ./scripts/run.sh play log 40000` — expect shadow ≤ 1.38 ms
   (baseline after L3a); the saving, if any, shows there + total (9.07–9.08).
   Audit estimate was 0.1–0.2 ms; if the driver fast-clears depth, the delta may be
   ≈0 — report the measured numbers honestly either way.
3. `ENGINE_HIDE_GUI=1 ./scripts/run.sh play screenshot /tmp/gpuopt/after_L5.jpg`
   then diff vs `/tmp/gpuopt/ref.jpg` (shimmer baseline ≈ 3.0/255 mean; watch the
   ground/shadow regions specifically for annulus artifacts at cascade corners).
4. Optional: flip `shadowQuality` to HIGH in settings.json and repeat (1) — the
   clear/scissor paths scale with activeCascadeCount (3 cascades) and that's the
   untested branch.

## round 7

Task 4 (final full verification) — build done; runtime steps BLOCKED by the still-down display, now with a precise root-cause diagnosis.

### Done
- Full `./scripts/build.sh` (NO SKIP_NAVMESH — navmesh bake ran, "navmesh: up to date"): **exit 0, zero warnings** (warning-strict). Log: /tmp/gpuopt/build_final.log.

### Display diagnosis (refines round 6's "SDL_CreateWindow hang")
- The game's static SDL3 = `cpp-thirdparty/sdl/git/build-linux/libSDL3.a` (SDL 3.5.0, **SDL_WAYLAND=OFF**, X11=ON — so on this Wayland/KWin session it can only drive X11 via Xwayland). Confirmed `platform_dir=build-linux` in CMakeLists.
- A probe linked against that exact archive: `SDL_Init(VIDEO|EVENTS|GAMEPAD)` OK, driver = **x11**, display mode 3840x2160@59, then **hangs inside `SDL_CreateWindow`** — even with plain flags (640x360, no VULKAN/RESIZABLE). `SDL_VIDEODRIVER=offscreen|dummy` works, so the hang is the X11 driver's fault.
- The X server is healthy: python-xlib gets instant round-trips (GetInputFocus, GetWindowAttributes) and MapNotify ~50 ms after map. But a new X11 window's **`GetWindowAttributes.map_state` stays IsUnMapped (0) even seconds after mapping** → the compositor (KWin) is not completing X11 window mapping (locked/blanked screen state).
- Where the hang is: SDL3 X11 driver `X11_SetWindowBordered`, `src/video/x11/SDL_x11window.c:1475` — `do { X11_XSync; X11_XGetWindowAttributes; } while (attr.map_state != IsViewable);` — an infinite busy-wait when the WM never maps the window. The game's log stops at "windowSystem: selected backend sdl" for exactly this reason.
- Nuance vs round 6: the *system* libSDL3.so.0 (Wayland-enabled) still creates windows fine — KWin's Wayland side works; only X11/Xwayland window mapping is dead. `SDL_LOG_VIDEO=verbose`/`SDL_LOG_X11=verbose` print nothing from this static build before the hang.
- Repro recipe: `clang -O1 -o /tmp/sdlprobe3 probe.c cpp-thirdparty/sdl/git/build-linux/libSDL3.a -lm -lpthread -ldl` (probe sources in /tmp/sdlprobe*.c); hangs at CreateWindow until the display recovers.

### User action needed
Unblank/unlock the screen, or restart KWin (`kwin_wayland`); then the remaining steps below are ready to run.

### Remaining steps (unchanged, display-dependent)
1. `./scripts/run.sh play log 6000` — must exit 0 with no ERROR/CRIT/Validation lines (validates L5 no-draw clear instance + per-cascade scissor under the validation layer).
2. `ENGINE_LOG_PASS_GPU=1 ./scripts/run.sh play log 40000` — pass-by-pass table vs baseline 9.44 (expect shadow ≤1.38, total ≈9.07–9.08).
3. `ENGINE_HIDE_GUI=1 ./scripts/run.sh play screenshot /tmp/gpuopt/final.jpg` — diff vs `/tmp/gpuopt/ref.jpg` (shimmer baseline ≈3.0/255), inspect ground/shadow corners for L5 cascade-extent edge cases.

## round 8

Task 4 (final display-dependent verification) — **done, all three checks PASS**. Display had recovered by 05:05 (probe /tmp/sdlprobe3 completed CreateWindow for plain/resizable/vulkan windows; the round-7 X11-map-state stall was resolved in the environment).

### Check 1 — `./scripts/run.sh play log 6000`: exit 0, zero ERROR/CRIT/Validation lines
Log: /tmp/gpuopt/final_run1.log (+ build/c-game/data/game.log). 47 WARN lines, all benign/known:
11× `vulkanCore: waitIdle!` (scene destroy / swapchain destroy / cleanup — shutdown path), 2× `decalAdd: persistent decal capacity reached (16384)`, RML shutdown + system-remove INFO-level teardown lines. No validation-layer errors → the L5 no-draw multi-layer clear instance + loadOp=LOAD cascade instances are clean under the active validation layer. The optional HIGH-quality (3-cascade) branch of the L5 path remains untested (would need flipping `shadowQuality` in settings.json — user preference, not done).

### Check 2 — `ENGINE_LOG_PASS_GPU=1 ./scripts/run.sh play log 40000` (exit 0, 0 ERROR, 0 validation)
600-frame average, parked player, run 05:06:

| pass            | baseline | after L3a | final |  | pass            | final |
|-----------------|----------|-----------|-------|--|-----------------|-------|
| azgaar_props    | 3.51     | 3.49      | 3.47  |  | depth           | 1.25  |
| shadow          | 1.40     | 1.38      | **1.38** | | heightmap_terrain | 0.68 |
| contact_shadow  | 0.52     | —         | 0.51  |  | ao              | 0.58  |
| taa             | 0.26     | —         | 0.25  |  | composite       | 0.18  |
| total           | **9.44** | 9.07–9.08 | **9.05** (fps 108.9) |  |                 |       |

**shadow = 1.38, exactly at the post-L3a value — the L5 scissor/single-clear lever produced no measurable GPU saving at this vantage.** This matches round 6's pre-stated expectation: the driver's depth fast-clear makes the per-cascade full-map clear cheap, and the 9% scissor reduction is below the measurement noise on this tile GPU. Reported honestly: L5 is correctness-neutral (still worth keeping — less wasted raster budget at HIGH quality / 3 cascades) but bought no measured ms here.

**Final result vs user's baseline: 9.44 → 9.05 ms (−0.39 ms, −4.1%), fps 105.5 → 108.9.** All of the measured gain came from L1 (−0.23) + L3a (−0.16); L2 and L5 are zero-at-this-vantage insurance levers (pay off in cities / dense veg / HIGH shadow quality).

### Check 3 — screenshot diff vs ref
`/tmp/gpuopt/final.jpg` vs `/tmp/gpuopt/ref.jpg` (both ENGINE_HIDE_GUI=1, parked player/camera untouched): mean abs diff **3.37/255**, 1,044,834 px changed at sum>12 (22.3%), 214,986 px at sum>40 (4.6%, scattered full-frame) — same magnitude as the established frame-to-frame TAA/vegetation-shimmer baseline (ref↔after_L2 2.96, ref↔after_L3a 3.29). Visual inspection of ground/shadow regions (crops: /tmp/gpuopt/final_cmp_detail.jpg player-shadow+grass close-up, /tmp/gpuopt/final_cmp_bottom.jpg bottom-half side-by-side): **grass-tuft CSM shadows and the player's shadow are identical in shape, softness, and extent; no annulus/edge artifacts at the cascade-extent corners** (the L5 failure mode). Differences are only expected vegetation-sway/TAA shimmer.

### Conclusion for the manager
The runtime side of the task is fully closed. If the manager wants more GPU time back, the remaining ranked levers from round 2 are L4 (merge per-tile props range draws into indirect arrays) and L6 (cull/noCull pipe split for solid props — needs user visual sign-off on leaf shadows); azgaar_props at 3.47 ms (38% of frame) is vegetation-tile rasterization/overdraw and is the only pass big enough to matter. Nothing in this round required or made source changes.
