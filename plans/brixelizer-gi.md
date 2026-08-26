# Brixelizer GI (FidelityFX) — Integration Plan

Integrate AMD FidelityFX **Brixelizer GI** (SDF voxelizer + screen-probe GI) into the
engine, as a new ambient/specular GI layer on top of the existing forward-lit pipeline.

Method: **small, independently verifiable steps**. Each step has an explicit **gate** —
we do not start the next step until the current one is proven correct (screenshot /
buffer dump / stats / validation layer clean). The FidelityFX sample
(`fsr3.1/git/samples/brixelizergi`) is the reference implementation; the engine's existing
FFX passes (`VulkanFsrPass`, CACAO in `VulkanAOPass`, `VulkanLpmPass`, `VulkanLensPass`,
`VulkanDofPass`) are the reference for engine-side patterns.

## Status

- [ ] Step 0 — SDK side verified (library + sample run as reference)
- [ ] Step 1 — Engine plumbing: FFX device, voxelizer context + resources (no instances)
- [ ] Step 2 — Voxelizer smoke test: one instance + SDF debug visualization
- [ ] Step 3 — Real scene meshes registered (static world)
- [ ] Step 4 — Terrain SDF meshes (streaming tiles)
- [ ] Step 5 — Props (vegetation / buildings) SDF, budgeted
- [ ] Step 6 — GI inputs: blue noise, environment cube, history buffers
- [ ] Step 7 — GI context + dispatch + raw GI output verified
- [ ] Step 8 — Albedo G-buffer + GI compositing into the lit image
- [ ] Step 9 — Settings / debug GUI + performance tuning
- [ ] Step 10 — Dynamic geometry + robustness (later)

## Background

Brixelizer GI is two FFX components:

1. **Brixelizer (voxelizer)** — bakes scene geometry (vertex/index buffers + per-instance
   transform + AABB) into a 3D SDF "brixel" representation: a 512³ R8 SDF atlas, a brick
   AABB list, and per-cascade brick maps + AABB trees (24 cascades max). Cascades are
   voxel grids of increasing voxel size (×2 per level), centered on a world-space point
   (`sdfCenter`, which follows the camera). Static instances are baked once and
   incrementally invalidated; dynamic instances re-bake every frame.
2. **Brixelizer GI** — per frame: fills screen-space probes, ray-marches the SDF
   cascades (diffuse + specular rays, SDF ray-marching — **no hardware ray tracing /
   ray queries needed**; verified: none of the 19 GI GLSL shaders use ray queries),
   accumulates a radiance/irradiance cache, denoises temporally, and outputs
   per-pixel **diffuseGI** + **specularGI** (R16F) at `displaySize` (internally
   computed at a chosen sub-resolution, e.g. 50%, then upscaled).

Why it fits: the engine's ambient is zero (`LightSystem`: "shadowed areas are pure
black"), there is no IBL, and SSR only covers reflections. Brixelizer GI gives soft
colored ambient + specular GI.

## Current state (already done — do not redo)

**SDK fork (`cpp-thirdparty/fsr3.1`):**

- `build.sh` `ENABLED_COMPONENTS` already contains `brixelizer` + `brixelizergi`
  (plus `classifier`, `denoiser`). `git/sdk/build-linux/libffx_fsr3upscaler_vk.a`
  contains all `ffxBrixelizer*` / `ffxBrixelizerGI*` symbols (verified with `nm`),
  and all SPIR-V permutation headers (124 brixelizer + 608 GI) are compiled.
- Linux fork patches already committed: context-size bumps (wchar_t inflation),
  `ffx_brixelizergi_private.h` OOB `constantBuffers[3]→[4]` fix, narrowing-cast fixes,
  bindless pool `FREE_DESCRIPTOR_SET_BIT` fix, per-voxel reference clamp + 2D→3D
  distance cull (dense-scene defenses). See `docs/fsr3.1.md`.
- Sample: `git/samples/brixelizergi` (Cauldron-based, built for both DX12 and VK) with
  `build-brixgi-sample.sh` cross-building a Wine-runnable `FFX_BrixelizerGI_VK.exe`
  (`git/bin/`). Config: `samples/brixelizergi/config/brixelizergiconfig.json`
  (resource formats: HistoryDepth R32F, HistoryNormals/GI outputs RGBA16F).

**Engine:**

- No brixelizer code remains from the earlier attempt (only empty
  `shaders/pass/{brixgi,gi}/spv` dirs). This plan rebuilds the engine side from
  scratch in small steps.
- Patterns to copy:
  - FFX backend interface creation + resource wrapping: `VulkanFsrPass.cpp`
    (`ffxGetScratchMemorySizeVK` / `ffxGetInterfaceVK` / `ffxGetResourceVK` +
    `wrapImageResource`), context recreation on `swapchainCreated`.
  - Dispatch + settings + absent-sentinel output: CACAO in `VulkanAOPass.cpp`,
    `VulkanLpmPass.cpp`.
  - Frame resource ownership: `VulkanFrameResources` (per-res images, getters).
  - Debug buffer dumps: `ENGINE_DEBUG_DUMP_IMAGES` token table in `Vulkan.cpp`
    (extend it for GI buffers).
- Available G-buffer / camera state (see `VulkanFrameResources`, `CameraComponent`):
  - `depth` D32, **REVERSE-Z: depth 1 = near, 0 = far/background** (built with
    `glm_perspective(yfov, aspect, zfar, znear)` — near/far args swapped, explicit
    `// (Reverse-Z)` comment; cglm `persp_rh_zo` maps the near-arg→0/far-arg→1 so the
    swap yields near=1/far=0). Depth test is `GREATER_OR_EQUAL`; depth clears to 0 and
    sky pixels read `depth == 0.0` (`composite.comp` `isSky`). No gl_FragDepth override.
  - `worldNormal` R16G16B16A16_SFLOAT — full 3-component **world-space** normals,
    written by the depth pre-pass for all geometry (terrain included).
  - `normals` R16G16 — oct-encoded *view* normals (NOT usable by GI's affine unpack).
  - `material` R8G8B8A8: .r roughness (perceptual), .g metallic, .b alphaMask, .a 0.
    **No albedo in the G-buffer.**
  - `velocity` R16G16 — pixel units, `current − previous` (y flipped), written by the
    depth pre-pass (+ sky/scene velocity passes).
  - `cameraUbo`: view/projection (jittered) + inv* + prev*ViewProjection (products
    only — no separate prev view/proj), `renderLocation` = camera position,
    `znear/zfar/yfov`.
  - Geometry: per-scene `VulkanScene` — one interleaved `vertexBuffer`
    (`SceneVertex`, 56 B, position at offset 0), one u32 `indexBuffer`, per-draw
    `GpuDrawInstance` (firstIndex, indexCount, vertexOffset, boundingSphere),
    CPU `Transform` (quat + pos + scale) / `WorldTransform` per entity.
    Iterate with `vulkanGetVisibleScenes()`.
  - Terrain: heightmap tiles (2048 m, 512² CPU height grid `heights[]` in metres,
    streaming 5×5 window) — **not a mesh**; no vertex/index data to feed the
    voxelizer (Step 4 generates SDF meshes from the grids).
  - Props: `VulkanAzgaarPropsPass` — merged species mesh (`PropsVertex`, **72 B** —
    too wide for the voxelizer's 6-bit stride field, Step 5 uses position-only
    buffers) + per-tile `PropInstance` scatter (pos, yaw, scale, species, variant).
  - Sky: procedural shader (`skybox.frag`: gradient + sun disc/glow), **no cube
    environment map** (Step 6 generates one).
  - No blue-noise texture anywhere (Step 6 generates one).

## Conventions & pitfalls (verified in the SDK source — the usual DX12→VK traps)

These were checked against the fork's actual code, not the docs:

1. **GI dispatch matrices (`view`, `projection`, `prevView`, `prevProjection`) are
   COLUMN-MAJOR** — pass the engine's cglm `mat4` by `memcpy`, exactly like CACAO does
   for `proj`. The `ffx_brixelizergi.h` comment "row major order" is **wrong**.
   Evidence: the sample `memcpy`s Cauldron's `Mat4` (Sony VectorMath `Matrix4`,
   stored as `mCol0..mCol3` = column-major); the host's `matrixMul(view, projection,
   out)` only yields the correct `P·V` when inputs are column-major (its
   `a[row*4+i]*b[i*4+col]` loop is the row-major formula, which on column-major
   data computes `B·A`); the GLSL UBO `mat4` is column-major in memory, so a
   byte-identical memcpy is correct. If GI reprojection is ever wrong, this is the
   first thing to suspect — but do NOT transpose.
2. **Voxelizer instance transforms
   (`FfxBrixelizerInstanceDescription.transform[16]`) are ROW-MAJOR** — opposite
   convention from (1)! The GLSL `LoadInstanceTransform` explicitly transposes:
   *"Instance transforms as stored in rows, so load in the 3 rows"*. The sample
   writes `transform[row*4+col] = mat.getCol(col)[row]`. Build the 4×4 from the
   engine quat/pos/scale in row-major (or transpose the cglm mat4).
   **Same API, two different conventions — do not mix them up.**
3. **Depth: the engine is REVERSE-Z (1 = near, 0 = far/background) → set
   `FFX_BRIXELIZER_GI_FLAG_DEPTH_INVERTED`.** Evidence: swapped near/far args in
   `glm_perspective` (`CameraSystem.cpp`, commented Reverse-Z), `GREATER_OR_EQUAL`
   depth test, clear-to-0, and `isSky = (depth==0.0)`. Setting the flag flips exactly
   the right GLSL branches for reverse-Z: `ffxIsBackground(d) = d < 1e-12` (background
   = 0), `BackgroundDepth = 0.0`, and the closer/farther compare ops (`a > b` =
   closer, `max` = closer-op). This is the same convention the FSR upscaler's
   `ENABLE_DEPTH_INVERTED` flag already (correctly) assumes — consistent, not a
   red herring. `ffxGetLinearDepth` uses `abs(viewZ)` (convention-independent), and
   we pass the engine's reverse-Z projection so unprojection stays consistent.
4. **Normals**: GI reads `.xyz` of the normal texture with an affine unpack
   (`normalsUnpackMul/Add`) and treats the result as **world-space**. Use the
   engine's `worldNormal` image with `mul = (1,1,1)`, `add = (0,0,0)`. Do **not**
   use the oct-encoded `normals` image (same issue as CACAO's `generateNormals`).
5. **Motion vectors**: GI computes `prev_uv = uv + mv * motionVectorScale`. Engine
   velocity is in pixels (current − previous, y-flipped) →
   `motionVectorScale = (-1.0f/renderW, -1.0f/renderH)`. (The sample passes
   `{1,1}` because Cauldron's MVs are already UV offsets.) A wrong scale shows up
   as temporal shimmer/ghosting when the camera moves — verify in Step 7 with a
   moving-camera run.
6. **Roughness**: engine material `.r` is perceptual roughness →
   `isRoughnessPerceptual = true` (the shader squares it), `roughnessChannel = 0`.
7. **Environment map must be a cube** (`textureCube` in the GLSL; the FFX VK
   backend maps a `CUBE_COMPATIBLE` 6-layer 2D array to a cube view).
8. **Vertex stride is packed into 6 bits (≤ 63 B)** in the GPU instance table.
   `SceneVertex` (56 B) fits; `PropsVertex` (72 B) does **not** → props use
   position-only (12 B) buffers.
9. **Instance table cap: 65536** (`FFX_BRIXELIZER_MAX_INSTANCES`) — the props
   per-tile cap (2 M) is far above it; Step 5 budgets instances.
10. **GI `displaySize` is fixed at context creation** → recreate the GI context on
    resolution change (FSR pass pattern). Internal resolution (50/75/100%) is a
    context parameter too.
11. **FFX resource states**: the FFX dispatch does not manage the engine's image
    layouts — transition engine images to `GENERAL` before the dispatch and back to
    `SHADER_READ_ONLY_OPTIMAL` after (the FSR pass pattern). FFX manages its own
    internal resources.
12. **One `FfxInterface` per scratch buffer.** Use one interface with
    `ffxGetScratchMemorySizeVK(pdev, 2)` / `ffxGetInterfaceVK(..., 2)` shared by the
    brixelizer + GI contexts (sample pattern). The FSR pass keeps its own
    (count 1) — separate scratch, no conflict.

## Resources the engine must provide (from the sample's `Init()`)

Voxelizer (Step 1):

| Resource | Type | Size |
| --- | --- | --- |
| `sdfAtlas` | 3D image R8_UNORM, STORAGE\|SAMPLED | 512³ |
| `brickAABBs` | buffer (u32) | `FFX_BRIXELIZER_BRICK_AABBS_SIZE` |
| `cascadeAABBTrees[24]` | buffer | `FFX_BRIXELIZER_CASCADE_AABB_TREE_SIZE` |
| `cascadeBrickMaps[24]` | buffer | `FFX_BRIXELIZER_CASCADE_BRICK_MAP_SIZE` |
| `gpuScratch` | buffer | start 256 MiB; assert `outScratchBufferSize` fits |

GI (Steps 6–7): `outputDiffuseGI` / `outputSpecularGI` (R16F RGBA, render res),
`historyDepth` (R32F — sample convention; engine has `vulkanCopyDepthToColorImage`; clear 0.0 = background since reverse-Z, pitfall #3),
`historyNormal` (R16F RGBA, copy of `worldNormal`), `prevLitOutput` (R16F RGBA,
copy of the composited color), `noiseTexture` (RG8 blue noise), `environmentMap`
(cube, R16F), plus all voxelizer resources (passed as COMPUTE_READ).

---

## Step 0 — Verify the SDK side (library + sample as reference)

Goal: prove the whole FFX library path works on this machine before writing engine
code. The sample is the ground truth for "what correct looks like".

0.1 **Rebuild the FFX library** (ensure the `.a` is current with all fork patches):

```bash
cd /home/enes/Projects/c/cpp-thirdparty/fsr3.1 && ./build.sh
```

- Verify: `nm git/sdk/build-linux/libffx_fsr3upscaler_vk.a | grep ffxBrixelizerGIContextCreate`
  (and `ffxBrixelizerContextCreate`, `ffxBrixelizerRegisterBuffers`).
- Verify the Windows `.a` still builds too (release packaging depends on it).
- If `build.sh` output changed (new permutation headers are expected to reorder),
  note it in `docs/fsr3.1.md`.

0.2 **Fetch the sample media** (not in the tree — the sample loads the Toyshop
scene, IBL textures, and 16 `LDR_RG01_*.png` noise maps from `media/`; without
the noise maps the render module never becomes ready):

```bash
cd /home/enes/Projects/c/cpp-thirdparty/fsr3.1/git
wine sdk/tools/media_delivery/MediaDelivery.exe \
     --target-sha256=7c82a9704f11c082ad12115eadb944e7884a73cb678bb51e8c7486a3190c7f98
```

0.3 **Build + run the sample (Vulkan, Wine)**:

```bash
cd /home/enes/Projects/c/cpp-thirdparty/fsr3.1 && ./build-brixgi-sample.sh
cd git/bin && wine FFX_BrixelizerGI_VK.exe
```

- Let it run into the toyshop scene; set Output Mode = "Diffuse GI" / "Specular GI"
  and "Radiance Cache" / "Irradiance Cache"; take screenshots of each.
- These screenshots are the **visual reference** for Step 7's gates.

0.4 (Optional, if time permits) run the sample under the validation layer or RenderDoc
(`docs/renderdoc-capture.md`) to capture a known-good frame for later comparison.

**Gate 0:** library symbols present in both `.a`s; sample runs under Wine and renders
all four GI output modes without FFX errors. If the sample itself is broken on VK,
fix that first — it validates the entire library + shader path.

## Step 1 — Engine plumbing: FFX device + voxelizer context + resources

New System: `c-engine/renderer/vulkan/pass/brixelizer/VulkanBrixelizerPass.{h,cpp}`
(System name `"brixelizer"`), registered in `Vulkan.cpp` after `vulkanAOPass`
(before `vulkanCompositePass`). It owns:

- **FFX backend interface** (shared by both contexts): on `swapchainCreated`,
  `ffxGetScratchMemorySizeVK(vulkan.physicalDevice, 2)` → calloc → `ffxGetDeviceVK`
  → `ffxGetInterfaceVK(&iface, device, scratch, size, 2)` (copy `VulkanFsrPass`'s
  `ensureContext`, but count = 2). Expose `vulkanBrixelizerPassGetInterface()` so
  the GI part (Step 7) reuses it. Destroy in `removed()`.
- **Engine-owned GPU resources** (table above), created on `swapchainCreated` via
  `vulkanCreateImage` (3D support already exists in `VulkanImage.cpp`) /
  `vulkanCreateGpuBuffer`. SDF atlas: `VK_IMAGE_TYPE_3D`, `R8_UNORM`,
  `STORAGE|SAMPLED`. Buffers: `STORAGE_BUFFER|VERTEX_BUFFER|INDEX_BUFFER` as
  appropriate (FFX reads them as shader storage).
- **Voxelizer context**: `ffxBrixelizerContextCreate` with
  - `numCascades = 8`, cascade flags `FFX_BRIXELIZER_CASCADE_STATIC` (dynamic
    cascades come in Step 10; with no dynamic geometry the merged cascade is
    redundant), `voxelSize = 2.0f * (2.0f ^ i)` (2 m … 256 m; far cascade spans
    256 m × 64 = 16.4 km — covers the 10.24 km streaming window; document this
    choice), `sdfCenter = {0,0,0}` at creation (updated per frame later).
  - `FfxBrixelizerContextDescription.backendInterface = iface`.
- **Wrap helpers**: extract `wrapImageResource` / `makeImageCreateInfo` from
  `VulkanFsrPass.cpp` and a new `wrapBufferResource(VulkanBuffer*, usage, state,
  name)` (uses `ffxGetBufferResourceDescriptionVK` — synthesize the
  `VkBufferCreateInfo` from `VulkanBuffer.size` + usage) into a shared
  `VulkanFfxUtils.h` (the FSR/AO passes keep working unchanged or get migrated in a
  follow-up; do not block on it).
- **Per-frame** (`update()`): `ffxBrixelizerBakeUpdate` (with
  `updateDesc.outScratchBufferSize` checked against the scratch buffer size,
  `sdfCenter` = camera position, `maxReferences` / `triangleSwapSize` /
  `maxBricksPerBake` from the sample: 32 M / 300 M / 16384 — adjust in Step 9) +
  `ffxBrixelizerUpdate` on the baked desc. With zero instances this is a
  no-op-but-executed cascade pass set — measure its cost.

No instances yet. No GI context yet.

**Gate 1:** context create returns `FFX_OK`; a `play log 5000` run completes with
the validation layer on and **zero** FFX-related warnings/errors; stats struct
(`FfxBrixelizerStats` via `outStats`) shows `freeBricks > 0`; profile shows the
empty-update cost (record it).

## Step 2 — Voxelizer smoke test: one instance + SDF debug view

Prove the voxelizer actually bakes geometry through our resources.

2.1 **Test instance**: register one small buffer pair — either a tiny generated cube
(positions-only 12 B, 36 indices) or the scene's first mesh's sub-range — via
`ffxBrixelizerRegisterBuffers` (wrap `VulkanBuffer` with
`FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ`, as the sample's `GetBufferIndex` does).
Create **one static instance**:
- `aabb` = world-space bounds (for the cube: parked-player-relative, e.g. 10 m
  ahead of the camera spawn so the parked view sees it),
- `transform` = **row-major** 4×4 (pitfall #2) — for the cube, translation only,
- `indexFormat = FFX_INDEX_TYPE_UINT32` (or UINT16 for the cube),
  `vertexFormat = FFX_SURFACE_FORMAT_R32G32B32_FLOAT`, `vertexStride = 12`,
  `triangleCount`, `maxCascade = 0` (near cascades only), `flags = NONE`.

2.2 **SDF debug visualization**: add a `VulkanImage` `BrixelSdfDebug`
(R16F RGBA, render res, STORAGE|SAMPLED) + fill `FfxBrixelizerDebugVisualizationDescription`
(`FFX_BRIXELIZER_TRACE_DEBUG_MODE_DISTANCE`, then `..._GRAD`, `..._BRICK_ID`;
`inverseViewMatrix`/`inverseProjectionMatrix` = cglm inverses, **column-major
memcpy** — pitfall #1 applies to these too; `tMin/tMax/sdfSolveEps` from the
sample; `renderWidth/Height` = render res; `output` = the debug image,
`FFX_RESOURCE_STATE_UNORDERED_ACCESS`). Pass it via
`updateDesc.debugVisualizationDesc`.

2.3 **Dump hook**: extend the `ENGINE_DEBUG_DUMP_IMAGES` token table in
`Vulkan.cpp` with `brixelSdf` (and later `giDiffuse` / `giSpecular`).

2.4 **Verify**: with the parked player (do not move it):

```bash
ENGINE_DEBUG_DUMP_IMAGES=brixelSdf ./scripts/run.sh play screenshot /tmp/brix_sdf.jpg
```

**Gate 2:** the dump shows the SDF of the instance (distance field with a clean
zero-level set at the surface; gradient view smooth; brick-ID view shows brick
boundaries); `stats` shows allocated bricks/triangles > 0 and no "failed voxel"
behavior; validation layer clean. If the SDF looks transposed/mirrored, the
transform convention (pitfall #2) or matrix convention (pitfall #1) is wrong —
fix before moving on.

## Step 3 — Register the real scene meshes (static world)

Replace the test cube with the actual world geometry.

3.1 **Buffer registration**: for each visible scene (`vulkanGetVisibleScenes()` →
`scene->backendScene`), register `VulkanScene.vertexBuffer` once (stride **56**,
offset per-draw `vertexOffset * 56`, `R32G32B32_FLOAT`) and
`VulkanScene.indexBuffer` once (u32, offset `firstIndex * 4`). Re-register on
scene create/destroy (`rendererSceneCreate` / `rendererSceneDestroy` hooks —
subscribe like the FSR pass subscribes to `swapchainCreated`).

3.2 **Instance creation**: one instance per (entity, primitive) where the entity
is **not skinned** (`Skin` component / `DRAW_FLAG_SKINNED` — skip in v1):
- `aabb` = `GpuDrawInstance.boundingSphere` (local center + radius) transformed to
  world by the entity's `WorldTransform` (fall back to `Transform`),
- `transform` = row-major 4×4 from quat/pos/scale (cglm: quat → 3×3, compose;
  **transpose before storing** — pitfall #2),
- `indexBufferOffset = firstIndex * 4`, `triangleCount = indexCount / 3`,
  `vertexBufferOffset = vertexOffset * 56`, `vertexCount` = primitive vertex count,
  `maxCascade` scaled by instance size (big objects → high cascade; small objects
  → low `maxCascade` so tiny geometry isn't wasted on far cascades — the sample
  leaves it 0; use a size heuristic in Step 9),
- `flags = FFX_BRIXELIZER_INSTANCE_FLAG_NONE` (static).
- Log the total instance count + triangle count; assert < 65536 (pitfall #9).

**Gate 3:** SDF debug dump (parked player) shows the surrounding world geometry
(terrain underfoot, props, buildings) with clean surfaces; first-bake cost
(allocations + voxelize) measured via the profile and logged — acceptable at
load time (target: < 200 ms, record actual); `trianglesAllocated` within the
`maxReferences` budget (the fork's per-voxel clamp patches are in place, but
watch for "failed voxel" log spam); validation layer clean.

## Step 4 — Terrain SDF meshes (streaming tiles)

The heightmap terrain has no mesh — generate one per tile for the voxelizer.

4.1 **Per-tile SDF mesh**: when a `HeightmapTile` reaches `HEIGHTMAP_TILE_READY`
(hook into the heightmap system's readiness, same place `AzgaarProps` polls
tiles), generate a decimated grid from `tile.heights` (512², metres):
- resolution `N` (default **65** → 32 m spacing for a 2048 m tile; env override
  `ENGINE_BRIXEL_TERRAIN_RES`), world-space positions
  `(originX + i*step, heights[i][j], originZ + j*step)`, positions-only 12 B
  buffer + u16 index buffer (N² < 65536 vertices), `N²*2` triangles (8192 for
  N=65).
- Upload with the engine's staging pattern (`vulkanCreateCpuToGpuBuffer` +
  `vulkanCopy`), register with the voxelizer (position-only buffer, stride 12),
  create **one static instance per tile**: AABB = tile bounds (min/max height),
  identity transform (positions already world-space), `maxCascade` by size.
- On tile eviction: `ffxBrixelizerDeleteInstances` for that tile's instance ID.
- `updateDesc.sdfCenter` = camera position every frame (cascades follow the
  camera; the sample's `m_SdfCenterFollowCamera` behavior).

**Gate 4:** SDF debug shows terrain under the player; tile streaming works —
with a *temporary* camera move (ask the user; never move the parked player) the
SDF follows and new tiles appear / evicted ones disappear without permanent
"UNINIT holes" in the brick map (the fork's clamp patches should prevent
wedge-locks); per-tile bake cost logged; total instances (25 tiles + scene
meshes + later props) stay under the 65536 cap.

## Step 5 — Props (vegetation / buildings) SDF, budgeted

5.1 **Position-only buffers**: at `azgaarPropsInit` (where the merged species mesh
is built), extract per-(species, variant) positions-only sub-buffers (12 B/vertex —
pitfall #8: 72 B `PropsVertex` does not fit the stride field) + per-variant index
ranges (u16, counts are small).
5.2 **Instances**: when a tile's scatter finalizes (same hook as the props pass
receives it), create one static instance per `PropInstance` that survives a
**budget**: global cap (default 40 k, `ENGINE_BRIXGI_PROP_BUDGET`), priority =
distance-to-camera then species (canopy species / buildings first; grass tufts
last — they add little SDF value at cascade resolutions). Transform =
T(pos)·R_y(yaw)·S(scale), row-major. `maxCascade` by species size (trees high,
grass low).
5.3 Log accepted vs dropped counts.

**Gate 5:** SDF debug shows trees/rocks/buildings around the player; budget
respected (log line); bake cost with props included measured (the fork's
dense-scene patches should keep reference counts sane); no wedge-locks after
streaming in/out of tiles with props.

## Step 6 — GI inputs: blue noise, environment cube, history buffers

6.1 **Blue noise**: generate a 256×256 RG8 blue-noise texture on the CPU (standard
best-candidate Poisson-disk algorithm; or port a known 256² blue noise generator),
upload once as a sampled `VulkanImage` (`BrixelBlueNoise`). (The sample loads 16
`LDR_RG01_*.png` from `media/` and cycles by frame; a single static texture is
fine for v1 — revisit if banding shows up.)
6.2 **Environment cube**: one-shot compute dispatch evaluating the procedural sky
(factor the sky color + sun function out of `skybox.frag` into a shared include,
`includes/sky.shader`) into a **128×128×6 R16F cube** (`VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT`
+ `VK_IMAGE_VIEW_TYPE_CUBE` — the engine already does this for `dummyCubeImage`;
the FFX backend maps it to a cube view, pitfall #7). Re-render on swapchain create
(v1: also fine at load only).
6.3 **History buffers** (render res, all STORAGE|SAMPLED|TRANSFER):
- `HistoryDepth` **R32F** (sample convention — D32→R32F via
  `vulkanCopyDepthToColorImage`; clear **0.0** = background, since the engine is
  reverse-Z — see pitfall #3; do NOT clear to 1.0),
- `HistoryNormal` R16F RGBA (copy of `worldNormal`; clear 0),
- `HistoryLitOutput` R16F RGBA (copy of the composited color **after** GI
  compositing — Step 8 adds the copy; for now copy the plain composite; clear 0).
- Copies run at end of frame (new tiny step in the brixelizer pass's
  `postUpdate`, or a dedicated `VulkanBrixelizerHistoryPass` if ordering gets
  awkward — decide in Step 8).

**Gate 6:** dump each new resource (`ENGINE_DEBUG_DUMP_IMAGES` tokens
`giNoise`, `giEnv` (dump one cube face), `giHistDepth`): noise is uniform-ish
blue noise (no large low-freq blobs); the cube face shows the sky gradient + sun
at the right elevation; the history depth copy matches the D32 dump. Validation
clean.

## Step 7 — GI context + dispatch + raw GI output

7.1 **GI context**: `ffxBrixelizerGIContextCreate` with
`flags = FFX_BRIXELIZER_GI_FLAG_DEPTH_INVERTED` (engine is reverse-Z — pitfall #3;
no `DISABLE_SPECULAR`; no `DISABLE_DENOISER`), `internalResolution = FFX_BRIXELIZER_GI_INTERNAL_RESOLUTION_50_PERCENT`,
`displaySize = render size`, shared FFX interface. Recreate on `swapchainCreated`
/ size change (FSR pattern). `outputDiffuseGI` / `outputSpecularGI` = new R16F
RGBA render-res images (STORAGE|SAMPLED).
7.2 **Per-frame dispatch** (pass ordering: after the depth pre-pass + scene,
before composite — i.e. register between `vulkanAOPass` and
`vulkanCompositePass`; transition inputs to SHADER_READ_ONLY, outputs to GENERAL
around the call — pitfall #11):
- matrices: `view` / `projection` = `camera->cameraUbo.view/.projection`
  (**jittered**, matching the depth buffer — column-major memcpy, pitfall #1);
  `prevView` / `prevProjection` = our own saved previous-frame copies of the same
  (the engine only keeps products, so keep the pair in pass state);
- `cameraPosition = cameraUbo.renderLocation.xyz`;
- `startCascade = 0`, `endCascade = 7` (static-only cascade layout, Step 1);
- `rayPushoff = 0.25`, `sdfSolveEps = 0.5`, `specularRayPushoff = 0.25`,
  `specularSDFSolveEps = 0.5`, `tMin = 0`, `tMax = 10000` (sample defaults);
- `normalsUnpackMul = (1,1,1)`, `normalsUnpackAdd = (0,0,0)` (worldNormal —
  pitfall #4);
- `isRoughnessPerceptual = true`, `roughnessChannel = 0`, `roughnessThreshold = 0.9`
  (pitfall #6);
- `motionVectorScale = (-1.0f/renderW, -1.0f/renderH)` (pitfall #5);
- `environmentMapIntensity = 0.1` (sample default; tune in Step 9);
- `noiseTexture` = `BrixelBlueNoise` (frame-independent for v1);
- all SDF resources wrapped COMPUTE_READ; `brixelizerContext` =
  `ffxBrixelizerGetRawContext(&brixelizerContext, &rawPtr)`;
- frame 0 after context creation: zeroed history (Step 6.3) + first frame runs
  with multi-bounce disabled (sample's `m_FrameIndex == 0 ? 0 : 1` pattern — the
  GI context has no explicit multiBounce flag; it is implied by the radiance
  cache being cleared, so a one-frame warmup with cleared caches is the
  equivalent; note it in the code).
7.3 **Debug cache views**: `ffxBrixelizerGIContextDebugVisualization` into
  `BrixelGIDebug` (R16F) — `FFX_BRIXELIZER_GI_DEBUG_MODE_RADIANCE_CACHE` /
  `_IRRADIANCE_CACHE`; add dump tokens `giDiffuse`, `giSpecular`, `giCache`.

**Gate 7 (the big one):** with the parked player:
- `giDiffuse` dump: bright where sky is visible, dark under/inside geometry,
  plausible color tint (bluish sky) — **compare against the Step 0 sample
  screenshots** (different scene, same character);
- `giSpecular` dump: sun glint + sky reflection pattern, not uniform white;
- `giCache` views coherent (no garbage/NaN — a NaN fill shows as flat color);
- **mv-scale check**: a short *user-approved* camera-move run (or a temporary
  `ENGINE_`-driven camera path) shows no temporal smearing/ghosting proportional
  to motion — if it ghosts, `motionVectorScale` is wrong (sign or units);
- validation layer clean; GPU cost of the 19 GI passes recorded (profile).

## Step 8 — Albedo G-buffer + GI compositing

8.1 **Albedo G-buffer**: the composite needs per-pixel albedo to weight the
diffuse GI (`L += albedo * E_diffuse`). The G-buffer has none. Add a 4th color
attachment:
- extend the pipe infra with `colorFormat4` / `clearColor4(Enabled)`
  (`VulkanPipe`, render-pass creation, `vulkanBeginRender` — a generic,
  default-off change),
- new frame resource `albedo` (R8G8B8A8, cleared 0): written by `scene.frag`
  (`baseColor.rgb` — location 3), the terrain depth frag (v1: neutral gray or the
  terrain's per-tile tint), the props depth frag (part color × instance tint);
  water/roads leave it 0 (no diffuse GI on water — acceptable for v1).
- (Alternative, if the 4th attachment is too invasive: encode albedo into
  `material.a` + two spare bits — rejected: 3 channels needed; do it properly.)

8.2 **Composite**: extend `composite.comp` (absent-sentinel pattern, like
`aoIndex` — when GI is disabled the term is skipped entirely so frames stay
pixel-identical):
- `composite += albedo.rgb * diffuseGI.rgb * diffuseFactor` (non-sky pixels only,
  `diffuseFactor` default 1.5 — sample default),
- `composite += specularGI.rgb * specularFactor * (1 - roughness) * (1 - metallic-ish mask)`
  (default 3.0; v1: additive on top of the existing SSR blend — blending SSR out
  is a Step 9 decision, not a correctness gate).
- **History copy**: after the composite writes `compositeColor`, copy
  `compositeColor → HistoryLitOutput` (this frame's GI-inclusive lit output is
  the next frame's `prevLitOutput` — the sample's `CopyHistoryResources` order).

**Gate 8:** parked-player A/B screenshots (`ENGINE_BRIXGI=0` vs `1`):
- shadowed areas (interior of structures, under trees, terrain crevices) gain
  soft **colored** ambient — red walls get red-tinted ambient, not white;
- no white-wash on colored surfaces (albedo weighting correct);
- sky/surface boundary clean (no GI on sky pixels);
- TAA and FSR paths both stable (run with upscaler on and off); no GI ghosting
  on camera motion;
- the disabled path is **pixel-identical** to pre-GI (diff the dumps).

## Step 9 — Settings, debug GUI, performance

- Settings: GI on/off (persisted like other settings, `settingsGetBool`),
  internal resolution (50/75/100%), diffuse/specular factors; env overrides
  `ENGINE_BRIXGI=0/1`, `ENGINE_BRIXGI_RES=50/75/100`, `ENGINE_BRIXGI_SDF_DEBUG=...`
  (mirrors the `ENGINE_AO_IMPL` pattern).
- Debug GUI section (settings GUI): SDF debug mode selector (distance/gradient/
  brick ID), GI cache view toggle, stats (free bricks, static/dynamic
  triangles/refs/bricks from `FfxBrixelizerStats`).
- Performance: profile the voxelizer update (24 cascade ops) + 19 GI passes;
  RenderDoc capture (`docs/renderdoc-capture.md`) to inspect individual passes
  when optimizing. Tune: cascade count (8 → fewer if far cascades cost more
  than they contribute), voxel size base (2 m), `tMax`, `sdfSolveEps`,
  `maxBricksPerBake`, terrain SDF resolution, props budget.
- **Gate 9:** per-frame cost at the parked scene recorded and accepted (fill in
  measured numbers: voxelizer ___ ms, GI ___ ms); settings persist across
  restarts; GUI toggles work; `docs/fsr3.1.md` updated with the final
  configuration.

## Step 10 — Dynamic geometry + robustness (later)

- Switch to the sample's 3-per-level cascade layout (static + dynamic + merged,
  8 levels = 24 cascades) and resubmit dynamic instances (player, enemies)
  every frame (`FFX_BRIXELIZER_INSTANCE_FLAG_DYNAMIC`); GI traces the merged
  cascades (offset 16).
- Teleport / scene-cut handling: clear history buffers + reset frame counters
  (GI denoiser re-converges cleanly instead of smearing from the old position).
- Verify resize recreation end-to-end (both contexts).

## Per-step validation protocol

1. Build: `./scripts/build.sh` (full pipeline — shaders + assets + this code).
2. Run: `./scripts/run.sh play screenshot /tmp/<name>.jpg` (parked player —
   **never move it**; ask the user for any other vantage point).
3. Buffer dumps: `ENGINE_DEBUG_DUMP_IMAGES=<tokens> ./scripts/run.sh play
   screenshot /tmp/x.jpg` → `<x>_<token>.jpg` next to the screenshot.
4. Validation layer / debug: `ENGINE_DEBUG=1` (or the `renderdoc` variant for
   frame captures — `docs/renderdoc-capture.md`).
5. Log check: `./scripts/run.sh play log 5000 && cat build/c-game/data/game.log`
   (≥ 5000 ms — asset loading needs it).
6. A reference screenshot from the Step 0 sample is the visual yardstick for
   "does the GI look right".

## Open questions (decide as the steps land)

1. Cascade layout: static-only 8 cascades (Steps 1–9) vs the sample's 24-cascade
   3-per-level layout from the start (Step 10 upgrades in place — the GI
   `startCascade`/`endCascade` offsets change, nothing else).
2. Specular GI vs SSR: keep both (additive) until the SSR pass can be retired.
3. Terrain SDF resolution (65) and props budget (40 k) are starting points —
   tune against bake cost + SDF quality in Steps 4–5, not by guesswork.
4. Blue noise: single static texture vs the sample's 16-texture cycle (only if
   banding is visible).
5. `maxReferences` / `triangleSwapSize` / `maxBricksPerBake` (sample: 32 M /
   300 M / 16384) may need scaling for the azgaar world's density — the fork's
   clamp patches make failures non-fatal, but the stats should stay green.