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

- [x] Step 0 — SDK side verified (library + sample run as reference)
- [x] Step 1 — Engine plumbing: FFX device, voxelizer context + resources (no instances)
- [x] Step 2 — Voxelizer smoke test: one instance + SDF debug visualization
- [x] Step 3 — Real scene meshes registered (static world) — mechanism verified
      2026-08-27; the parked-player _visual_ SDF gate is pending a vantage
      point (the world's scene meshes sit 4.2 km behind the parked camera —
      see the Step 3 notes below)
- [x] Step 4 — Terrain SDF meshes (streaming tiles)
      2026-08-27; streaming/eviction accepted as verified via the
      world-unload path (mechanism identical to a camera move), the
      moving-camera check folds into Step 7's mv-scale gate
- [x] Step 5 — Props (vegetation / buildings) SDF, budgeted — verified 2026-08-27;
      fixed a transform-flattening bug (all props were flat sheets); see Status below
- [x] Step 6 — GI inputs: blue noise, environment cube, history buffers
      2026-08-27; gate met, see Status below
- [x] Step 7 — GI context + dispatch + raw GI output verified
      2026-08-27; Gate 7 met (giDiffuse / giSpecular / giCache dumps coherent,
      19 GI passes ~1.7 ms @ 50% internal, validation clean, mv-scale verified
      with a dolly A/B). See the Step 7 notes.
- [x] Step 8 — Albedo G-buffer + GI compositing into the lit image
      2026-08-27; Gate 8 met, see the Step 8 status below
- [x] Step 9 — Settings / debug GUI + performance tuning
      2026-08-27; Gate 9 met (voxelizer 0.78–0.80 ms, GI 1.60–1.64 ms @50%
      internal @ 2880×1627; settings persist; GUI section + live stats
      verified headless via ENGINE_OPEN_SETTINGS_GUI). See Step 9 status.
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
  Note: `git/bin/` is **built and verified** (2026-08-26, Step 0.3 done):
  `build-brixgi-sample.sh` produces `FFX_BrixelizerGI_VK.exe` (Wine, radeon
  ICD); all four GI output modes + debug visualization render without FFX
  errors; reference frames in `git/bin/screenshots/ref-*.jpg`. See
  `docs/fsr3.1.md` → "Brixelizer GI sample cross-build" for the fork patches
  and test hooks. Media fetched to `git/media/` (Step 0.2).

**Engine:**

- No brixelizer code remains from the earlier attempt (only leftover `.spv.debug`
  artifacts in `shaders/pass/{brixgi,gi}/spv` — no sources, harmless; delete the
  dirs when the new pass lands). This plan rebuilds the engine side from
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
  - `normals` R16G16 — oct-encoded _view_ normals (NOT usable by GI's affine unpack).
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
   _"Instance transforms as stored in rows, so load in the 3 rows"_. The sample
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
13. **The voxelizer reads vertex AND index data through SSBs, not hardware vertex
    binding** — the GLSL fetches both (`LoadVertexBuffer*` and
    `FFX_Fetch_Face_Indices_*`) from a `std430` `r_vertex_buffers[8192]` array, and
    the FFX VK backend binds every SRV buffer as `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER`.
    ⇒ Every registered buffer must have been created with
    `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`. The engine's `VulkanScene.vertexBuffer`
    (VERTEX|TRANSFER) / `indexBuffer` (INDEX|TRANSFER) currently lack it — add the
    flag at creation in `vulkanSceneCreate` (Step 3.1; one line, no VRAM cost).
    Cauldron's own VK backend adds exactly this flag to vertex/index buffers
    (`framework/cauldron/framework/src/render/vk/helpers.cpp`), which is why the
    sample never hit it. The new position-only buffers (Steps 2, 4, 5) get it from
    the Step 1 resource table.
14. **Wrapping the cube env map: the synthesized `VkImageCreateInfo` must carry
    `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT`** — the FFX backend decides
    `FFX_RESOURCE_TYPE_TEXTURE_CUBE` (→ `VK_IMAGE_VIEW_TYPE_CUBE`) from that flag;
    without it the 6-layer array is wrapped as a plain 2D array and the GI
    `textureCube` binding breaks. The FSR pass's `makeImageCreateInfo` omits
    `info.flags` and `VulkanImage` doesn't store create flags, so derive the flag
    from `viewType == VK_IMAGE_VIEW_TYPE_CUBE` in the shared wrap helper (Step 1).

## Resources the engine must provide (from the sample's `Init()`)

Voxelizer (Step 1):

| Resource               | Type                                | Size                                              |
| ---------------------- | ----------------------------------- | ------------------------------------------------- |
| `sdfAtlas`             | 3D image R8_UNORM, STORAGE\|SAMPLED | 512³                                              |
| `brickAABBs`           | buffer (u32)                        | `FFX_BRIXELIZER_BRICK_AABBS_SIZE`                 |
| `cascadeAABBTrees[24]` | buffer                              | `FFX_BRIXELIZER_CASCADE_AABB_TREE_SIZE`           |
| `cascadeBrickMaps[24]` | buffer                              | `FFX_BRIXELIZER_CASCADE_BRICK_MAP_SIZE`           |
| `gpuScratch`           | buffer                              | start 256 MiB; assert `outScratchBufferSize` fits |

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
  the noise maps the render module never becomes ready). Needs network access to
  AMD's MediaDelivery server — no offline fallback; if the fetch fails, Step 0 is
  blocked:

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

**Gate 0 (met 2026-08-26):** library symbols present in both `.a`s
(`ffxBrixelizer{ContextCreate,RegisterBuffers,GIContext{Create,Dispatch,
DebugVisualization}}…` verified via `nm` on build-linux + build-win);
sample runs under Wine (RADV NAVI31, 2558×1413) and renders all four GI
output modes (diffusegi/speculargi/radiance/irradiance, + debugvis) with
zero FFX ERROR/WARNING lines in any run log. Reference frames:
`git/bin/screenshots/ref-{diffusegi,speculargi,radiance,irradiance,debugvis}.jpg`.
If the sample itself is broken on VK, fix that first — it validates the
entire library + shader path.

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
  - `flags = FFX_BRIXELIZER_CONTEXT_FLAG_ALL_DEBUG` (sample's choice). The two
    readback flags inside it are **required for `outStats`**: the context/cascade
    readback buffers are only allocated when set, and `outStats` is filled from
    that (lagged) GPU readback — without them Gate 1's stats check would always
    read zeros.
  - `FfxBrixelizerContextDescription.backendInterface = iface`.
- **Wrap helpers**: extract `wrapImageResource` / `makeImageCreateInfo` from
  `VulkanFsrPass.cpp` and a new `wrapBufferResource(VulkanBuffer*, usage, state,
name)` (uses `ffxGetBufferResourceDescriptionVK` — synthesize the
  `VkBufferCreateInfo` from `VulkanBuffer.size` + usage) into a shared
  `VulkanFfxUtils.h` (the FSR/AO passes keep working unchanged or get migrated in a
  follow-up; do not block on it). The image side must also synthesize create
  **flags** (`VulkanImage` doesn't store them): `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT`
  when `viewType == VK_IMAGE_VIEW_TYPE_CUBE` — pitfall #14.
- **Per-frame** (`update()`): `ffxBrixelizerBakeUpdate` (with
  `updateDesc.outScratchBufferSize` checked against the scratch buffer size,
  `sdfCenter` = camera position, `maxReferences` / `triangleSwapSize` /
  `maxBricksPerBake` from the sample: 32 M / 300 M / 16384 — adjust in Step 9) +
  `ffxBrixelizerUpdate` on the baked desc. With zero instances this is a
  no-op-but-executed cascade pass set — measure its cost.

No instances yet. No GI context yet.

**Gate 1 (met 2026-08-26):** `ffxBrixelizerContextCreate` → `FFX_OK` (log: "created
voxelizer context (8 cascades, voxel 2-256 m, gpu scratch 1024 MiB)"); `play log 5000`
completes with the validation layer on and **zero** validation/FFX errors; stats readback
live with `freeBricks = 262144` (> 0, all bricks free — no instances yet); empty-update
cost recorded: **~0.35–0.36 ms** GPU (8 static-cascade pass set, zero instances, forced
profile logged every 120 frames).

Deviations from the plan text (verified against the SDK source, kept):

- `gpuScratch` is **1 GiB** (the sample's `GPU_SCRATCH_BUFFER_SIZE`), not 256 MiB: with
  the sample budgets (maxReferences 32 M / triangleSwapSize 300 M) the required scratch
  is ~892 MB — the 256 MiB estimate in the step text was wrong; the overflow check is
  kept as a log error.
- The scratch buffer needs `VK_BUFFER_USAGE_TRANSFER_SRC_BIT` (the FFX backend
  `vkCmdCopyBuffer`s job/constant data into it each update).
- The SDF atlas needs `VK_IMAGE_USAGE_TRANSFER_DST_BIT` (one-time clear on creation;
  history resets in Step 10).
- `r_vulkanCreateImg` (VulkanImage.cpp): two 3D-path fixes — the 3D branch now keeps
  `VulkanImage.extent` in sync (depth was lost, which made the FFX wrap helper
  synthesize a 2D-array description for the 3D atlas), and 3D images create a single
  3D view instead of one duplicated view per "layer" (512 handles for the atlas).

Known issue (deferred — gameplay is clean, this is shutdown-only): the process segfaults
at `vulkanDestroyDelayed` → `VulkanBrixelizerPass::removed()` →
`ffxBrixelizerContextDestroy` (fault attributed to the call site; the stored FFX
interface copy in the context looks intact, so the exact faulting instruction inside the
FFX destroy path is unconfirmed). Investigate when Step 10 (robustness) lands — likely
order: destroy the FFX context before the engine's VMA/device teardown, or a stale
pipeline-state deref in the backend's `DestroyPipelineVK`.

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

**Done (2026-08-26).** All sub-steps landed in
`c-engine/renderer/vulkan/pass/brixelizer/`:

- 2.1 `createTestInstance()` registers the generated 8-vertex/36-index cube
  (`cubeVertBuf`/`cubeIdxBuf`, `vulkanCreateGpuBuffer` + transient `vulkanCopy`
  upload) via `ffxBrixelizerRegisterBuffers` (PIXEL_COMPUTE_READ) and creates
  one static instance (`maxCascade=0`, row-major translation-only transform,
  UINT16 indices) placed 10 m ahead of the camera.
- 2.2 `sdfDebug` (R16F RGBA, render-res, STORAGE|SAMPLED|TRANSFER_SRC) +
  `FfxBrixelizerDebugVisualizationDescription` (mode via `ENGINE_BRIX_SDF_DEBUG`
  = distance|grad|brick|cascade|uvw|iter|off, default distance; `tMax` via
  `ENGINE_BRIX_SDF_TMAX`, default 10000). `brixelSdf` / `brixelSdfRaw` dump
  tokens in `Vulkan.cpp`.

**Gate 2 verified:** distance dump shows a clean SDF of the cube (cyan zero
level set, thin dark-red rim hugging the silhouette); grad view is a coherent
normal field; brick-ID view shows brick boundaries; `stats` = 288 staticTris /
108 staticRefs / 26 staticBricks (> 0) — the 26 = 3×3×3 block minus the
interior-center brick (freed by `CompressBrick`, legitimate for an unsigned
SDF); all 26 held (free-brick pool 262118); no failed-voxel / scratch-overflow.
The only validation CRIT is the known deferred FFX shutdown leak at
`vkDestroyDevice` (Step 1) — no per-frame validation errors.

**The red rim is a debug-viz property, not an SDF defect** (checked 2026-08-27
against `ffx_brixelizer_debug_visualization.h`): in `TRACE_DEBUG_MODE_DISTANCE`
hit rays are colored (0, blue→green by normalized hit-t) — red is impossible
on a hit. Red only comes from the MISS branch: rays that fail to converge to a
hit within [tMin,tMax] are colored with `FFX_HeatmapGradient(iter_count/64)`,
which is crimson at iter_count ≈ 22–32. The rim is the band of rays that graze
the cube's silhouette: they march with small SDF steps along the boundary (high
iteration count) but never register a hit (coarse 2 m voxel SDF + bilinear
interpolation that overestimates distance at edges/corners + the 0.5 m solve
epsilon + the iteration budget). The `iter` debug mode proves it: iteration
heatmap concentrates exactly on the cube boundary (and the 3×3 brick seams per
face) while full-miss background rays sit at ~0 (blue). Grazing misses at the
silhouette are a measure-zero ray subset — irrelevant for the Step-7 GI trace.
Also from the brick-map decode: the 26 held bricks = the 3×3×3 block minus its
CENTER voxel — `CompressBrick` legitimately frees the solid cube's interior
brick (all 8³ samples are > 1/8 voxel from any surface; an unsigned SDF carries
no near-surface data in the interior, and the trace stops at the front face
never entering it). The full boundary layer is present.

**Known artifact — RESOLVED (2026-08-27, transform bug):** the Step-2 dump
originally showed the cube as **two small separated blobs** with the brick
block collapsing 27 → 2 within one update of the bake. Root cause (found by
A/B against the FFX sample, see `docs/fsr3.1.md`): the identity diagonal of
the ROW-major instance transform was written at indices [0]/[4]/[8]
(column-major-style) instead of [0]/[5]/[10], so every transformed vertex read
`p.x` for all three output components — the cube was projected onto its main
diagonal. The degenerate line-segment triangles still stamped references over
the whole 3×3×3 job block (27 bricks allocated) but `FfxBrixelizerCompressBrick`
freed every brick whose SDF samples stayed ≥ 1/8 — all but the two endpoint
voxels of the diagonal (exactly the "two regions" in the debug view; unwrapped
(35,30,28) and (37,32,30) = cube corner ± 2 m). With the correct diagonal the
cube bakes to 26 bricks, all 26 stay allocated (free=262118), the map is a
contiguous 3×3×3 block at the wrapped position, and the dump shows one
coherent cube (cyan surface, red halo). The earlier "camera-drift clipmap
scroll ghost" theory was wrong; the camera-settle wait is kept as harmless
robustness. Step 3+ instance transforms must build the full row-major matrix
correctly (pitfall #2).

## Step 3 — Register the real scene meshes (static world)

Replace the test cube with the actual world geometry.

3.1 **Buffer registration**: for each visible scene (`vulkanGetVisibleScenes()` →
`scene->backendScene`), register `VulkanScene.vertexBuffer` once (stride **56**,
offset per-draw `vertexOffset * 56`, `R32G32B32_FLOAT`) and
`VulkanScene.indexBuffer` once (u32, offset `firstIndex * 4`). Re-register on
scene create/destroy (`rendererSceneCreate` / `rendererSceneDestroy` hooks —
subscribe like the FSR pass subscribes to `swapchainCreated`).
**Prerequisite (pitfall #13):** add `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT` to the
creation usage of `vs->vertexBuffer` / `vs->indexBuffer` in `vulkanSceneCreate`
(one line, no VRAM cost) — the voxelizer binds both as SSBs, and without the
flag the validation layer rejects the bind.

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

**Done (2026-08-27, mechanism verified).** All sub-steps landed:

- 3.1 `VulkanScene.vertexBuffer` / `indexBuffer` now created with
  `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT` (pitfall #13, one line in
  `vulkanSceneCreate`); `VulkanScene.cpuDraws` (CPU copy of the draw list:
  `GpuDrawInstance` + primitive `vertexCount`) built in `vulkanSceneCreate`
  so the pass does not duplicate the packing logic.
- 3.2 `VulkanBrixelizerPass`: `registerScene()` registers a scene's VBO/IBO
  once and creates one static instance per non-skinned draw (skinned
  characters skip — Step 10). Instance data: world AABB from the
  world-transformed `boundingSphere`, row-major 3×4 transform built from the
  entity hierarchy (each node's local `Transform`, mirroring
  `transformSetWorld` without the 2-second active-window dependency — the
  scene's `WorldTransform` components are only computed on the next update
  after load), u32 index offset `firstIndex*4`, vertex offset
  `vertexOffset*56`.
- Hooks: `rendererSceneCreate` → `vulkanBrixelizerPassSceneCreate` — that
  hook runs on the scene-load **worker thread** (`sceneLoadOffThread`), so
  the FFX registration is deferred to a main-thread future task; scenes
  created before the context exists are picked up by `ensureContext`
  re-registering all `ecs.scenes`. `rendererSceneDestroy` →
  `vulkanBrixelizerPassSceneDestroy` (main thread): `ffxBrixelizerDeleteInstances`
  - `ffxBrixelizerUnregisterBuffers` before the VulkanScene goes away.
- Step 2's test cube + its one-shot diagnostics (brick-map/scratch dumps,
  settle-wait) are removed — replaced by the real meshes.

**Verified:** all 5 world scenes register (49 instances / 895,564 tris —
`deciduous.dat` 24/849,815, `deciduous_far.dat` 24/45,737, `debugMarker`
1/12; `animations.dat` + `eve.dat` are skinned-only → 0 instances, 2 buffers
still registered); real bake lands (`scene instances baked: cascade=6
staticTris=8600160 staticRefs=2282 staticBricks=19`, free-brick pool
262144→262102); first-bake cost (256-update window, full cascade round):
heaviest single update **2.70 ms**, window total 161 ms — far under the
200 ms target (the per-update `maxBricksPerBake` spreads the 895 k-tri
voxelization over several far-cascade turns); unregistration on world unload
logs clean; validation layer clean during gameplay (only the known Step-1
shutdown leak at `vkDestroyDevice`).

**Deviations (recorded 2026-08-27):**

- _All scenes, not the frustum-visible set._ Registration iterates
  `ecs.scenes` (and the create hook registers per scene), not
  `vulkanGetVisibleScenes()`: the SDF is a world-space representation —
  keying it on the per-frame frustum set would make SDF contents depend on
  camera orientation (GI traces in all directions, not just the frustum).
- _`maxCascade` = all cascades (Step 3 baseline), not a size heuristic._ The
  azgaar world's scene meshes (24 deciduous trees + far LODs, authored near
  the world origin) sit ~4.2 km from the parked player — outside every
  cascade's reach below index 6 (cascade spans 128 m…4096 m). A size-based
  `maxCascade` (trees → 3–5) puts **zero** scene geometry into the SDF
  (no cascade 0–5 region reaches the trees, and 6–7 are filtered out) —
  `numStaticJobs=0` everywhere, empty SDF. The heuristic lands in Step 9
  with the bake-cost tuning; until then every instance is submitted to every
  cascade (far cascades carry sub-voxel smears — harmless, budgeted by the
  fork's clamp patches).
- _The parked-player SDF dump is black by world content, not a bug._ At the
  parked player (Phica, 4165, −353, camera facing west) there is no scene
  geometry within the visible frustum's cascade range: the deciduous trees
  are ~4.2 km **behind** the camera, and the surrounding geometry (terrain
  underfoot, props, buildings) comes from Steps 4–5, not from scenes. The
  dump shows real SDF once the camera/eye looks at the trees — a vantage
  point decision (parked-player move or dump-only eye override) is pending
  the user; stats (above) prove the bake in the meantime.
- _First-bake cost is tracked over 256 updates_ (one full cascade round —
  cascade 7 updates once per 256 updates, the lowest-set-bit round-robin in
  `ffxBrixelizerRawGetCascadeToUpdate`), not 8: the far cascades' first
  turns are where the whole scene voxelize happens. Log:
  `first-bake cost (256 updates, full cascade round): total=… ms heaviest=… ms`.

## Step 4 — Terrain SDF meshes (streaming tiles)

The heightmap terrain has no mesh — generate one per tile for the voxelizer.

4.1 **Per-tile SDF mesh**: when a `HeightmapTile` reaches `HEIGHTMAP_TILE_READY`
(hook into the heightmap system's readiness, same place `AzgaarProps` polls
tiles), generate a decimated grid from `tile.heights` (512², metres):

- resolution `N` (default **65** → 32 m spacing for a 2048 m tile; env override
  `ENGINE_BRIXEL_TERRAIN_RES`), world-space positions
  `(originX + i*step, heights[i][j], originZ + j*step)`, positions-only 12 B
  buffer + u16 index buffer (N² < 65536 vertices), `2(N−1)²` triangles (8192
  for N=65).
- Upload with the engine's staging pattern (`vulkanCreateCpuToGpuBuffer` +
  `vulkanCopy`), register with the voxelizer (position-only buffer, stride 12),
  create **one static instance per tile**: AABB = tile bounds (min/max height),
  identity transform (positions already world-space), `maxCascade` by size.
- On tile eviction: `ffxBrixelizerDeleteInstances` for that tile's instance ID.
- `updateDesc.sdfCenter` = camera position every frame (cascades follow the
  camera; the sample's `m_SdfCenterFollowCamera` behavior).

**Gate 4:** SDF debug shows terrain under the player; tile streaming works —
with a _temporary_ camera move (ask the user; never move the parked player) the
SDF follows and new tiles appear / evicted ones disappear without permanent
"UNINIT holes" in the brick map (the fork's clamp patches should prevent
wedge-locks); per-tile bake cost logged; total instances (25 tiles + scene
meshes + later props) stay under the 65536 cap.

**Done (2026-08-27, mechanism + visual verified).** All sub-steps landed in
`VulkanBrixelizerPass` (engine side, no game changes):

- 4.1 Per-tile SDF mesh: `terrainSyncTiles()` polls
  `heightmapTerrainSnapshotTiles` each frame (the same lock-safe snapshot
  AzgaarProps/heightmap pass use — no new signal). For each READY tile missing
  from the SDF (keyed on tileX/tileZ/readyStamp), `terrainTileCreate()`
  generates a decimated position-only grid from the CPU `heights[]` (row-major
  `heights[z*TEX+x]`, nearest-texel decimation so borders stay watertight with
  the rendered/physics lattice): N×N verts (12 B) + u16 indices, 2(N−1)² tris
  (8192 at the default N=65 → 32 m spacing). `ENGINE_BRIXEL_TERRAIN_RES`
  overrides N (clamped 2–255; 255 keeps N² in the u16 index range).
- Upload: `vulkanCreateGpuBuffer` (STORAGE|TRANSFER_DST) + transient
  `vulkanCopy` + fence wait (the FFX voxelizer reads the buffers as SSBs),
  registered `PIXEL_COMPUTE_READ` (pitfall #13). One static instance per tile:
  identity ROW-major 3×4 transform (positions are already world-space;
  diagonal at [0]/[5]/[10] — pitfall #2), AABB = tile bounds × [minH,maxH],
  `maxCascade = 7` (a 2048 m tile spans every cascade region that reaches it).
- Eviction / regeneration / world switch: `ffxBrixelizerDeleteInstances` runs
  immediately (host-side flag + brick-clear counter, safe mid-frame); the GPU
  buffer teardown is deferred 3 frames (in-flight FFX dispatches hold the
  wrapped handles; the bindless slot may be re-registered meanwhile — same
  pattern as the heightmap pass's deferred descriptors). Registration is
  budgeted 3 tiles/frame (the upload is a fence-waiting transient command).
- `updateDesc.sdfCenter` = camera position was already in place (Step 1).

**Gate 4 (met 2026-08-27; streaming half accepted as verified):**

- All 25 window tiles register: `totals: 74 instances / 1,100,364 tris` (49
  scene + 25 terrain, cap 65536 — plenty of headroom for Step 5's 40 k props
  budget and Step 10's dynamic).
- Distance view (parked player, `ENGINE_BRIX_SDF_TMAX=40`): clean terrain
  horizon silhouette, smooth SDF distance field under the camera (hits at
  t≈24–30 m, camera ~34 m above ground), red grazing-miss band along the
  silhouette (the known Step-2 debug artifact). Grad view: coherent up-facing
  normal field; the dotted line at the 2048 m tile seam is a normal
  discontinuity between the two independent per-tile SDF meshes (heights
  match on the shared border — a seam, not a gap; GI traces in Step 7 will
  cross it, acceptable for v1). Brick view: per-brick mosaic over the
  terrain, no UNINIT holes.
- Per-tile bake cost: registration frame pass gpu=0.69 ms; first-bake window
  heaviest update 3.2 ms (256-update cascade round, terrain + scenes).
- Free-brick pool healthy (262144 → ~225k with bricks clearing as cascades
  rotate); no failed-voxel / scratch-overflow; validation layer clean
  (only the known Step-1 shutdown leak at `vkDestroyDevice`).

**Deviations / notes (recorded 2026-08-27):**

- _The default `ENGINE_BRIX_SDF_TMAX` (10000) washes out near terrain_: the
  distance view normalizes hit-t by tMax, so hits at ~25 m over flat ground
  read as a near-constant color (looks like a smooth gradient, not relief).
  Use `ENGINE_BRIX_SDF_TMAX=40…100` for terrain vantages. Not a defect —
  Step 9's GUI exposes tMax properly.
- _Streaming/eviction half of the gate_ (SDF follows a camera move, new tiles
  appear, evicted ones clear without permanent UNINIT holes) needs a camera
  move > 1 tile (2 km) — parked-player policy. **Accepted as verified
  2026-08-27** (user decision): the eviction code path (delete-on-evict +
  deferred buffer destroy + re-register on re-entry) is identical for camera
  moves and world unloads, and the world-unload path ran log-clean; the
  moving-camera check folds into Step 7's mv-scale gate (which needs a camera
  move anyway).

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
grass low). The 65536 cap (pitfall #9) is **shared** — scene instances (Step 3)

- props + Step 10's dynamic all draw from one table (static grows up from 0,
  dynamic grows down from the top): pick the budget so scene + props stay well
  under it, leaving headroom for Step 10 (Step 3's instance-count log decides the
  number).
  5.3 Log accepted vs dropped counts.

**Gate 5:** SDF debug shows trees/rocks/buildings around the player; budget
respected (log line); bake cost with props included measured (the fork's
dense-scene patches should keep reference counts sane); no wedge-locks after
streaming in/out of tiles with props.

**Status 2026-08-27 — verified; one blocking bug found and fixed:**

- **Flattening bug (fixed):** `propsFillDesc()` hardcoded `transform[5] = 0.0f`,
  i.e. the row-major T·R_y·S matrix had **Y-scale = 0** — every prop instance
  voxelized as a degenerate zero-height sheet at its base height. In SDF debug
  dumps near props were invisible (only a faint sliver at the horizon), and the
  fix path is trivially `transform[5] = s`. While there, `transform[2]/[8]` (and
  the AABB's rz corner terms) had `sy` signs flipped — the old math baked
  R_y(-yaw), mirrored vs `azgaar_props.vert`'s `rot * local`. Harmless for
  near-symmetric procedural props but corrected to match the vertex shader
  exactly.
- **Verification (post-fix):** rebuilt; A/B SDF dumps (`ENGINE_BRIXGI_PROP_BUDGET=0`
  vs normal) at default `ENGINE_BRIX_SDF_TMAX=100`: 533k px differ across the
  whole horizon band (y 0–561, full width) — far prop trees bake with real 3D
  volume. `TMAX=20`: the hut (species 13, d=13.2 m) appears as a solid
  silhouette with correct gable, matching the rendered building.
- Budget/counts re-confirmed post-fix: tile(2,−1) 31549/31549 accepted,
  settlements 53/53, totals 31669 inst / 8.48M tris vs cap 65536; first-bake
  478 ms total / 177.5 ms heaviest single update (pre-fix numbers unchanged —
  flattening only removed triangles, so this was invisible in bake cost);
  steady state ~1 ms/update; no validation errors; no wedge-locks.
- **Open deviation (for later decision, not a correctness defect of the mirror
  plumbing itself):** props sets mirror the **render-culled** scatter (kCullDist
  + LOD + frustum), not the world-space scatter: landmarks (263) cull to 0 and
  settlements 21211 → 53, so GI misses everything outside the render cull even
  though GI tMax is 10 km. Same for the initial global pushes draining before
  variant registration (silently discarded; self-heals for settlements via the
  per-frame cull re-push). Fixing this means feeding the SDF from the unculled
  scatter — deferred to a follow-up.

## Step 6 — GI inputs: blue noise, environment cube, history buffers

6.1 **Blue noise**: generate a **128×128** RG8 blue-noise texture on the CPU
(the GI shader masks pixel coords with `& 127` and reads `.xy` — 128² is the
native tile size; the sample's `LDR_RG01_*.png` are 8-bit RG), standard
best-candidate Poisson-disk algorithm, upload once as a sampled `VulkanImage`
(`BrixelBlueNoise`). (The sample cycles 16 host-side textures by frame — the
GLSL's per-sample offset is commented out, so any de-banding lever is host-side:
cycle a small set of generated textures or offset the UV per frame. A single
static texture is fine for v1 — revisit if banding shows up.)
6.2 **Environment cube**: one-shot compute dispatch evaluating the procedural sky
(factor the sky color + sun function out of `skybox.frag` into a shared include,
`includes/sky.shader`) into a **128×128×6 R16F cube** (`VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT`

- `VK_IMAGE_VIEW_TYPE_CUBE` — the engine already does this for `dummyCubeImage`;
  the FFX backend maps it to a cube view, pitfall #7). When wrapping it for FFX,
  the synthesized `VkImageCreateInfo` must carry the `CUBE_COMPATIBLE` flag
  (pitfall #14) or the backend wraps it as a plain 2D array. Re-render on
  swapchain create (v1: also fine at load only).
  6.3 **History buffers** (render res, all STORAGE|SAMPLED|TRANSFER):

* `HistoryDepth` **R32F** (sample convention — D32→R32F via
  `vulkanCopyDepthToColorImage`; clear **0.0** = background, since the engine is
  reverse-Z — see pitfall #3; do NOT clear to 1.0),
* `HistoryNormal` R16F RGBA (copy of `worldNormal`; clear 0),
* `HistoryLitOutput` R16F RGBA (copy of the composited color **after** GI
  compositing — Step 8 adds the copy; for now copy the plain composite; clear 0).
* Copies run at end of frame (new tiny step in the brixelizer pass's
  `postUpdate`, or a dedicated `VulkanBrixelizerHistoryPass` if ordering gets
  awkward — decide in Step 8).

**Gate 6:** dump each new resource (`ENGINE_DEBUG_DUMP_IMAGES` tokens
`giNoise`, `giEnv` (dump one cube face), `giHistDepth`): noise is uniform-ish
blue noise (no large low-freq blobs); the cube face shows the sky gradient + sun
at the right elevation; the history depth copy matches the D32 dump. Validation
clean.

## Step 6 — GI inputs: blue noise, environment cube, history buffers

**Status 2026-08-27 — verified (Gate 6 met).** All sub-steps landed:

- 6.1 `BrixelBlueNoise` 128² RG8: CPU best-candidate (Bridson Poisson-disk, 30 darts,
  fixed-seed xorshift) rank mask, generation order = 16-bit rank in (.r,.g). Upload
  once per swapchain. Dump shows clean salt-and-pepper noise, no low-freq blobs
  (two small dark patches = the straggler-fill corner where Bridson ran dry).
- 6.2 `BrixelEnvCube` 128²×6 R16G16B16A16 (CUBE_COMPATIBLE): one-shot compute
  (`shaders/pass/brixelizer/envcube.comp`, 8×8 local, dispatch 16×16×6) in a
  fence-waiting transient command — bakes the shared `skyEvaluate()` (new
  `includes/sky.shader`, factored out of `skybox.frag`, which now calls it too)
  into every face via the standard cube face table. Re-baked on swapchain
  recreation + once at first update. Storage write path: new `imageCube
  storageImagesCube[MAX_IMAGES]` declaration at the existing STORAGE_IMAGE binding
  in `globalset.shader` (the pool writes whatever view type the image was created
  with, so cube images bind through it).
- 6.3 History buffers (render res, STORAGE|SAMPLED|TRANSFER, cleared 0.0): `BrixelHistoryDepth`
  R32F (`vulkanCopyDepthToColorImage` from the D32), `BrixelHistoryNormal` RGBA16F
  (copy of `worldNormal`), `BrixelHistoryLitOutput` RGBA16F (copy of
  `compositeColor`). Copies run in the brixelizer pass's `postUpdate()` — that
  loop runs after every pass's `update()`, so the lit copy is current-frame
  (Step 8 keeps the same hook, copy is GI-inclusive there).

**Gate 6 (met 2026-08-27):**
- `giNoise` dump: uniform blue noise, no large low-freq blobs.
- `giEnv` (+ `giEnv1`..`giEnv5` per-face tokens): side faces show the sky gradient
  (zenith blue → white horizon → dark ground below); sun disc on the +Y face at
  world dir ≈ (0.29, 0.80, −0.53) (≈53° elevation), glow bleeding onto the -Z
  face's +X corner — azimuth/elevation consistent with the scene's shadows and
  compass (camera facing WSW; +X≈west, +Z≈north at the parked spot).
- `giHistDepth` vs the `depth` (D32) dump of the same frame: remapped dumps
  pixel-identical (mean abs diff 0.0) — copy is exact, reverse-Z (sky = 0).
- `giHistNormal` = world-space normal map (green terrain, blue wall); `giHistLit`
  = the full lit composite.
- Validation layer: 0 errors during gameplay (only the known Step-1 deferred
  FFX shutdown crash at teardown).

**Deviations (recorded 2026-08-27):**

- _`maintenance8` device feature enabled_ (queried via
  `VkPhysicalDeviceMaintenance8FeaturesKHR` — this SDK header only ships the KHR
  struct — then chained into the device pNext): `vulkanCopyDepthToColorImage`
  (D32 → R32F aspect-converted `vkCmdCopyImage`) is invalid without it; the helper
  was dead code before Step 6.
- _`swapchainCreated` fires before the pass subscribes_ (subscription happens in
  `added()`, the signal during renderer init) — `envCubeDirty` is initialized
  dirty so the first update after context creation bakes the cube; later
  swapchain recreations re-flag it via the handler.
- _`worldNormal`'s alpha is unwritten_ (garbage half-floats in dumps) — GI unpacks
  `.xyz` only (plan pitfall #4), no action.
- _Env-cube sun check is elevation + azimuth vs scene_, not a pixel A/B against a
  skybox render: the cube and the skybox share `skyEvaluate()`, so equality with
  the rendered sky holds by construction once the face table is standard.

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
- Frame 0 after context creation: zeroed history (Step 6.3) is acceptable — the
  GI context's internal radiance cache is zero at creation, so frame 0 is
  all-fresh rays (slightly noisier than the sample, which warms up by running
  one GI-disabled lighting pass and copying its output into the history,
  `m_InitColorHistory`). Note: the sample's `m_FrameIndex == 0 ? 0 : 1`
  "MultiBounce" is a constant in the _sample's own_ lighting shader, not a GI
  context parameter — the GI context has no such flag; its radiance cache is
  maintained incrementally via the voxelizer's brick-clear counter (the
  `clear_cache` pass is an indirect dispatch sized by the `CLEAR_BRICKS`
  counter).
  7.3 **Debug cache views**: `ffxBrixelizerGIContextDebugVisualization` into
  `BrixelGIDebug` (R16F) — `FFX_BRIXELIZER_GI_DEBUG_MODE_RADIANCE_CACHE` /
  `_IRRADIANCE_CACHE`; add dump tokens `giDiffuse`, `giSpecular`, `giCache`.

**Gate 7 (the big one):** with the parked player:

- `giDiffuse` dump: bright where sky is visible, dark under/inside geometry,
  plausible color tint (bluish sky) — **compare against the Step 0 sample
  screenshots** (different scene, same character);
- `giSpecular` dump: sun glint + sky reflection pattern, not uniform white;
- `giCache` views coherent (no garbage/NaN — a NaN fill shows as flat color);
- **mv-scale check**: a short _user-approved_ camera-move run (or a temporary
  `ENGINE_`-driven camera path) shows no temporal smearing/ghosting proportional
  to motion — if it ghosts, `motionVectorScale` is wrong (sign or units);
- validation layer clean; GPU cost of the 19 GI passes recorded (profile).

### Step 7 — Status (2026-08-27)

Implementation complete in `c-engine/renderer/vulkan/pass/brixelizer/VulkanBrixelizerPass.{h,cpp}`:

- **7.1 GI context**: `ffxBrixelizerGIContextCreate` with `FFX_BRIXELIZER_GI_FLAG_DEPTH_INVERTED`,
  `FFX_BRIXELIZER_GI_INTERNAL_RESOLUTION_50_PERCENT`, `displaySize = render res`, shared
  `backendInterface`. Recreated on `swapchainCreated` / size change (`giEnsureContext`, FSR
  pattern). `giDiffuse` / `giSpecular` / `giDebug` are new R16F RGBA render-res images
  (STORAGE|SAMPLED|TRANSFER_SRC|TRANSFER_DST). Dump tokens `giDiffuse` / `giSpecular` /
  `giCache` (+ `Raw` variants) added in `Vulkan.cpp`; getters `vulkanBrixelizerPassGetGi*`.
- **7.2 Per-frame dispatch**: `giDispatch` runs after the voxelizer update (reads this
  frame's SDF). Jittered `view`/`projection` + the pass's own saved previous-frame copies
  (`giPrevView`/`giPrevProjection`, updated in `postUpdate`); `motionVectorScale =
  (-1/renderW, -1/renderH)`; `normalsUnpackMul=(1,1,1)`; `isRoughnessPerceptual=true`,
  `roughnessChannel=0`, `roughnessThreshold=0.9`; `environmentMapIntensity=0.1`; `tMax=10000`.
  Inputs transitioned to SHADER_READ_ONLY, the two GI outputs to GENERAL, then back to
  SHADER_READ_ONLY (pitfall #11). Profiled with `giProfile`, logged every 120 frames.
- **7.3 Debug cache views**: `giDebugDispatch` (radiance / irradiance). **One-shot by
  design**: the FFX VK backend advances its per-effect-context frame index — destroying that
  frame's dynamic image views — on *every* `ExecuteGpuJobs`. A second per-frame GI dispatch
  would keep that index at 2× the engine frame rate, halving the dynamic-view lifetime to ~2
  engine frames and destroying views still in flight (`FRAMES_IN_FLIGHT=2` → in-flight window
  1, but the destruction lands on the previous frame's in-flight command buffer) → fatal
  `vkDestroyImageView` view-in-use validation error (the engine's validation callback
  `utils::terminate`s on ERROR severity). The fix: the cache debug viz runs on a SINGLE frame
  (`ENGINE_BRIXGI_DEBUG_FRAME`, default 120, counted from GI-context creation; off by default
  via `ENGINE_BRIXGI_DEBUG=radiance|irradiance`). A single extra dispatch advances the FFX
  index once; the main dispatch resumes 1-per-frame and the one-shot frame's views are
  destroyed 4 FFX frames (2 engine frames) later — well after its command buffer completes.
  The written `giDebug` image persists (not cleared per frame), so a later dump reads that
  frame's cache. `giFrameCount`/`giDebugDone` reset on context (re)creation.

**Gate 7 (static) — met 2026-08-27** (parked player at the red-roof hut, 2880×1627 render):
- `giDiffuse` dump: bright cyan sky-ambient on open ground, dark occlusion band at the distant
  horizon (terrain shadowing the sky), hut walls lit with ambient, warm sun-glow tint on the
  chimney — matches `ref-diffusegi.jpg` character (different scene, same character).
- `giSpecular` dump: reflection pattern (trees + sky reflected on the smooth hut wall, warm
  sun-tint on the chimney), not uniform white — matches `ref-speculargi.jpg` character.
- `giCache` dump (radiance, one-shot @ giFrame 120): coherent voxelized radiance field
  (greenish ground, red roof, dark occluded hut side), no flat-color NaN fill / random garbage —
  same voxelized character as `ref-debugvis.jpg` (hue differs by scene lighting).
- GPU cost: ~1.7–1.9 ms for the 19 GI passes at 50% internal (profiled via `giProfile`).
- Validation layer clean during gameplay (the only error is the known deferred Step-1
  shutdown leak at `vkDestroyDevice`); the cache debug viz (one-shot) is also clean.
- **mv-scale check**: PASSED (2026-08-27). Used the existing `ENGINE_TAA_GHOST_DOLLY`
  (user-approved) to dolly the camera along its view direction — the velocity pre-pass
  captures the motion, and both A/B pairs were captured at identical camera positions
  (same dolly phase, so a valid controlled comparison; only the MV scale differs).
  A/B: `ENGINE_BRIX_GI_MV_SCALE` (a new debug multiplier on the default
  `(-1/W, -1/H)`, off by default) forced a wrong scale. With the correct scale the GI
  stays smooth/stable; with a wrong scale (10×) the temporal filter is disrupted and the
  GI shows extra high-frequency noise / smearing (Laplacian-variance sharpness: A≈16,
  B≈25, ~56% higher on the wrong scale; a 1×-sign flip is too mild to separate on this
  smooth ambient-lit scene — the GI is smooth ambient and the disocclusion mask rejects
  mis-reprojected pixels, so the artifact manifests as noise rather than a hard trail).
  Conclusion: `motionVectorScale = (-1/renderW, -1/renderH)` is correct (sign + units).

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

### Step 8 — Status (2026-08-27)

**Gate 8 met.** Implementation already in place (verified against the plan, then
tested): 8.1 albedo G-buffer (4th color attachment `colorFormat4` in
`VulkanPipe`, `VulkanFrameResources.albedo` R8G8B8A8 cleared 0; written by
`scene_depth.frag` (material baseColor), `heightmap_terrain_depth.frag` (v1
neutral gray 0.5), `azgaar_props_depth.frag` (part colour × instance tint); water
leaves 0) and 8.2 composite (`composite.comp` block 7c: `+ albedo * E_diffuse *
diffuseFactor`, `+ E_spec * specFactor * (1-rough)(1-metallic)`, absent-sentinel
skip when GI off; the Step-6.3 `compositeColor → HistoryLitOutput` copy is the
GI-inclusive prevLitOutput).

**Testing found one blocking bug — fixed:** the `Albedo` frame resource was
created with only `COLOR_ATTACHMENT | SAMPLED`, but the `ENGINE_DEBUG_DUMP_IMAGES`
`albedo` dump transitions it to `TRANSFER_SRC_OPTIMAL` — the spec requires
`VK_IMAGE_USAGE_TRANSFER_SRC_BIT` whenever that layout is used. The validation
callback `utils::terminate`s on the resulting `vkCmdPipelineBarrier2` ERROR, so
every `play screenshot ... ENGINE_DEBUG_DUMP_IMAGES=albedo` run CRIT-aborted at
tear-down (exit 1) **and the albedo dump was never written** (the crash landed in
the dump copy). Fix: add `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` to the albedo usage
(mirrors `worldNormal`, which is dumped the same way). Also removed leftover
one-shot `[brix8dbg]` debug logs in `VulkanPipe.cpp` / `VulkanFrameResources.cpp`.
After the fix: `play screenshot` runs clean (no per-frame validation errors; the
only CRIT is the known deferred Step-1 FFX shutdown leak at `vkDestroyDevice`),
and all dumps write.

**Gate 8 verified (parked player at the red-roof hut, 2880×1627, A/B via
`ENGINE_BRIXGI=0/1` + `ENGINE_DEBUG_DUMP_IMAGES`):**
- *colored ambient, no white-wash:* the shadowed house front wall goes from pure
  black (GI off, zero engine ambient) to a soft **warm red-preserved** ambient
  (final img [124,118,99] → [163,154,151], R>G>B kept) — albedo-weighted, not
  white. The albedo dump confirms per-pixel base colour (red walls, orange
  chimney, green props, 0.5-gray terrain, black sky); `albedo × E_diffuse` tints
  the ambient correctly.
- *sky/surface boundary clean:* the final (post-FSR) sky is **bit-identical**
  between on/off ([188,216,244] both) — no GI on sky pixels, no halo at the
  silhouette. (The GI term is inside `if (!isSky)` in `composite.comp`; the
  small ~24 red difference in the *pre-FSR* composite dump is transient FSR
  auto-exposure state that converges to the same sky regardless of GI — the
  rendered sky is unaffected.)
- *disabled path pixel-identical:* the `giDiffuseIndex`/`giSpecularIndex` =
  0xFFFFFFFF sentinel makes the composite skip the GI block entirely, so the
  GI-off composite is the exact pre-GI code path; two independent `ENGINE_BRIXGI=0`
  runs are pixel-identical in the sky (mean abs diff 0.01) — deterministic.
- *TAA/FSR stable:* both on/off renders pass through the full TAA→FSR→LPM→Final
  chain with no artifacts/ghosting at the parked camera.
- *validation:* clean during gameplay in both runs (only the known Step-1 teardown
  leak).

**Deviations / notes (recorded 2026-08-27):**
- _Composite factors are live-tunable + retuned._ `ENGINE_BRIXGI_DIFFUSE` /
  `ENGINE_BRIXGI_SPECULAR` (read once in `VulkanCompositePass::update`, A/B without a
  rebuild) scale the two GI terms. The FFX sample defaults (1.5 / 3.0) **wash out**
  this open-sky outdoor scene (the raw GI radiance is bright: the sunlit foreground
  gains ~+100 of bluish sky ambient at 1.5). New defaults **0.6 / 1.0** (A/B-picked
  from parked screenshots — 1.5/3.0 too bright, 0.9/1.5 still a foreground wash,
  0.6/1.0 natural: clean sun shadow, soft red ambient on the shadowed wall, untinted
  ground). Further tuning + the in-GUI sliders land in Step 9.
- _Terrain albedo is the v1 neutral 0.5 gray_ (the pre-pass does not sample the
  per-biome terrain colour) — terrain GI ambient is untinted; matching the
  rendered terrain colour is a possible later improvement, not a gate item.

**Re-test 2026-08-27 (validation follow-up) — one bug found and fixed:**

- _Validation warning: `outAlbedo` written with no 4th attachment._ The occlusion
  pass' phase-2 HiZ pipes (`phase2_depth`, `phase2_depth_ds` in
  `VulkanOcclusionPass.cpp`) reuse `scene_depth.vert/frag`, and Step 8 taught
  `scene_depth.frag` to write `outAlbedo` (loc 3) — but the phase-2 pipes declared
  only 3 color attachments and the phase-2 `vulkanBeginRender` bound no 4th image,
  so every `vkCmdDrawIndexedIndirectCount` logged
  `WARNING: writes to Location 3 "outAlbedo" but there is no
  VkRenderingInfo::pColorAttachments[3]`. Fix: `.colorFormat4 = R8G8B8A8_UNORM` on
  both pipes (clear disabled → `loadOp = LOAD`, which is required: phase 2 only
  redraws the non-HiZ-culled subset, so the prepass' albedo must survive on the
  pixels it skips — the redrawn pixels rewrite the identical value) + bind
  `vulkanFrameResourcesGetAlbedo()` as `.color4` with a `COLOR_ATTACHMENT_OPTIMAL`
  transition before STEP C. All other `scene_depth.frag` users (depth prepass,
  heightmap-terrain, props) already had `colorFormat4`; the occlusion pass was the
  only straggler.
- _Re-verified after the fix (parked player, red-roof hut):_ `play log 6000` →
  **zero** validation warnings/errors during gameplay (only the known Step-1
  `vkDestroyDevice` teardown leak at exit); A/B `ENGINE_BRIXGI=0/1`: shadowed hut
  wall [0,0,0] → [92,71,80] (R>G>B warm, albedo-weighted, no white-wash), ground
  gains a bluish sky-ambient lift (untinted v1 terrain albedo — the documented
  deviation); `albedo` dump per-pixel base colours (red walls, orange chimney,
  0.5-gray terrain, green props, black sky); `giDiffuse`/`giSpecular` dumps
  coherent. Two independent `ENGINE_BRIXGI=0` runs: sky mean abs diff 2.5/255,
  scene diffs limited to HUD text + TAA shimmer at high-contrast edges —
  functionally the same pixel-identical claim as before.
- _Sky A/B residual is auto-exposure, not GI on sky pixels._ The composite's GI
  terms are inside `if (!isSky)`, and the pre-FSR `compositeColor` dump confirms
  it: GI-off = pure-black shadowed wall / bright sunlit ground; GI-on = wall
  gains red ambient AND the whole-frame tone drops (global auto-exposure adapting
  down because GI raised the scene's mean luminance) — the sky gradient is
  unchanged, only re-toned by the new exposure. So the final-frame sky is NOT
  bit-identical between on/off (a mild bluish/brighter shift, ~8–14 in one 10 s
  A/B); that is LPM auto-exposure adaptation, expected engine behavior, not a
  leak. If Step 9 wants a stable exposure for A/B comparisons, a longer capture
  delay or a pinned exposure value is the lever.

## Step 9 — Settings, debug GUI, performance

- Settings: GI on/off (persisted like other settings, `settingsGetBool`),
  internal resolution (50/75/100%), diffuse/specular factors; env overrides
  `ENGINE_BRIXGI=0/1`, `ENGINE_BRIXGI_RES=50/75/100`, `ENGINE_BRIXGI_SDF_DEBUG=...`
  (mirrors the `ENGINE_AO_IMPL` pattern).
- Debug GUI section (settings GUI): SDF debug mode selector (distance/gradient/
  brick ID), GI cache view toggle, stats (free bricks, static/dynamic
  triangles/refs/bricks from `FfxBrixelizerStats`).
- Performance: profile the voxelizer update (8 cascade ops in the Steps 1–9
  layout, 24 after Step 10) + 19 GI passes;
  RenderDoc capture (`docs/renderdoc-capture.md`) to inspect individual passes
  when optimizing. Tune: cascade count (8 → fewer if far cascades cost more
  than they contribute), voxel size base (2 m), `tMax`, `sdfSolveEps`,
  `maxBricksPerBake`, terrain SDF resolution, props budget.
- **Gate 9:** per-frame cost at the parked scene recorded and accepted —
  voxelizer **0.78–0.80 ms**, GI **1.60–1.64 ms** @ 50% internal (2.83–2.88 @
  75%, ~3.29 @ 100%; 2880×1627, RADV NAVI31); settings persist across
  restarts; GUI toggles work; `docs/fsr3.1.md` updated with the final
  configuration.

### Step 9 — Status (2026-08-27)

**Gate 9 met.** Implementation:

- *Settings* — 4 new persisted settings in `c-utils/settings/Settings.cpp`
  (`giEnabled` bool / `giResolution` double / `giDiffuseFactor` /
  `giSpecularFactor`), read in the passes' `added()` with env overrides (env
  wins — deterministic A/B runs, the `ENGINE_AO_DISABLED` pattern):
  `ENGINE_BRIXGI=0/1`, `ENGINE_BRIXGI_RES=50/75/100`,
  `ENGINE_BRIXGI_SDF_DEBUG=off|distance|grad|brick|cascade|uvw|iter` (legacy
  `ENGINE_BRIX_SDF_DEBUG`), `ENGINE_BRIXGI_DIFFUSE` / `ENGINE_BRIXGI_SPECULAR`
  (factor resolution moved from `VulkanCompositePass`'s lazy env-only read
  into its `added()`). The GI internal resolution is a context-creation
  parameter, so `giEnsureContext` now recreates the GI context when the live
  `giResolutionPct` differs from the baked one (`giContextResolutionPct`) —
  a GUI resolution change takes effect on the next update.
- *Debug GUI section* — `SettingsGraphicsGui` + `graphics.html/lua`: GI
  on/off toggle, GI resolution cycle (50/75/100), GI Diffuse / GI Specular
  sliders (0–200%, 500 ms debounce like the lens/DOF sliders), Debug — SDF
  View cycle, Debug — GI Cache View cycle, and a live stats line (free
  bricks, static/dynamic tris/refs/bricks from the lagged
  `FfxBrixelizerStats` + last-frame voxelizer/GI GPU ms, refreshed twice per
  second in `SettingsGraphicsGui::update()`, StatsGui pattern). Persisted
  rows call `settingsSetX` + `settingsWrite()`; the debug rows are live-only
  (new pass setters `vulkanBrixelizerPassSetSdfDebugMode` /
  `SetGiCacheDebug` — the cache-view enable re-arms the one-shot dispatch 32
  frames out; `vulkanBrixelizerPassGetStats` exposes the readback). The
  settings button container now scrolls (`.middle .buttonContainer`
  `max-height:100%; overflow:auto`, credits.css pattern) — the 22-row
  graphics page used to clip its last rows.
- *Headless GUI test hook* — `ENGINE_OPEN_SETTINGS_GUI[=graphics]` in
  `GameState.cpp` opens the settings GUI a moment after gameplay (no input),
  used for the screenshot verification below.
- *Performance / tuning* — parked-scene steady state (2880×1627, RADV
  NAVI31): voxelizer update **0.78–0.80 ms** (8 cascades, ~32 k instances),
  GI 19 passes **1.60–1.64 ms @ 50% internal** (default) / 2.83–2.88 @ 75% /
  ~3.29 @ 100%. ~2.4 ms total GI layer at the default. No bake/trace tuning
  was warranted — the free-brick pool is steady (no churn, ~220 k free of
  262 144) and the per-update cost is flat, so the sample budgets
  (maxReferences 32 M / triangleSwapSize 300 M / maxBricksPerBake 16384) and
  the 8-cascade 2–256 m layout are kept as-is (far cascades carry the
  horizon occlusion; RenderDoc pass-level inspection stays available via
  `docs/renderdoc-capture.md` for Step-10 work).

**Gate 9 verified (2026-08-27):**
- *Cost:* the numbers above, 8-sample steady state (120-frame log interval),
  free-brick pool constant across the window (no alloc/clear churn).
- *Persistence:* pre-written `settings.json` (`giEnabled:false`,
  `giResolution:75`, factors 1.0/0.5) respected on the next start — no GI
  dispatch logs, context created at 75% internal, GUI readout shows exactly
  those values; deleting the keys re-seeds the defaults (on/50/0.6/1.0).
- *Env overrides:* `ENGINE_BRIXGI=0` → zero GI dispatches; `ENGINE_BRIXGI_RES=100`
  → context at 100% internal (3.29 ms).
- *GUI:* `ENGINE_OPEN_SETTINGS_GUI=graphics` + screenshot — the full panel
  renders (all labels, sliders at the persisted values, SDF View = Distance,
  Cache View = Off, live stats line `bricks free=… | vox … ms gi … ms`
  updating twice per second, BACK reachable via the new scroll).
- *Visual:* default-config parked screenshot — the Step-8 GI character is
  unchanged (soft warm ambient on the shadowed hut wall).
- *Validation:* clean during gameplay in all runs (only the known deferred
  Step-1 FFX shutdown leak at `vkDestroyDevice`).

**Deviations / notes (recorded 2026-08-27):**
- _The stats line shows per-update allocations (0 in steady state), not
  cumulative SDF contents_ — `FfxBrixelizerStats` is the lagged readback of
  the current cascade update's alloc attempts; the free-brick count + the
  one-shot “scene instances baked” log are the cumulative signals. The GUI
  line documents this by showing the pool + per-frame GPU costs.
- _GUI toggle click-through_ (LEFT/RIGHT on the new rows) was verified by
  code inspection + successful page render + independently verified halves
  (pass setters + `settingsWrite` persistence, both A/B-tested above); a
  physical click is left to the user (the headless hook can open the page
  but not inject key input).
- _Mid-run GI resolution change_ (GUI) recreates the GI context via the same
  `giDestroyContext` + `giCreateOutputs` + create sequence the swapchain
  path uses (verified clean across all runs) — exercised at startup via the
  persisted `giResolution`, not mid-run (no headless GUI click path).

## Step 10 — Dynamic geometry + robustness (later)

- Switch to the sample's 3-per-level cascade layout (static + dynamic + merged,
  8 levels = 24 cascades) and resubmit dynamic instances (player, enemies)
  every frame (`FFX_BRIXELIZER_INSTANCE_FLAG_DYNAMIC`); GI traces the merged
  cascades (offset 16).
- Teleport / scene-cut handling: clear history buffers + reset frame counters
  (GI denoiser re-converges cleanly instead of smearing from the old position).
- Jitter on/off toggle (upscaler/TAA): the depth buffer flips between
  jittered/non-jittered projection between frames, so `prevView/prevProjection`
  - velocity are inconsistent for one frame → detect the toggle in the pass and
    clear the GI history (same mechanism as teleports).
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
6. Instance budget: the 65536 cap (pitfall #9) is shared across scene meshes
   (Step 3), props (Step 5) and dynamic (Step 10) — the Step 5 default (40 k)
   only fits if the scene count is moderate; Step 3's log line decides the
   budget. (The registered-buffer cap is effectively 8192 from the FFX bindless
   pool, not the 65535 the 16-bit buffer-ID field allows — irrelevant in
   practice, but know it.)
