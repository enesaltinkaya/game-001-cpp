# Brixelizer GI — Re-implementation Plan

Plan for re-attempting FidelityFX Brixelizer + Brixelizer GI (roadmap
[`plans/fidelityfx-sdk-expansion.md`](fidelityfx-sdk-expansion.md) Phase 6).

**This is a re-attempt, not a fresh integration.** Phase 6 was implemented
2026-08-23 (phases 6.0–6.4), debugged to "no known engine bugs" through five
sessions (see `inprogress/` history: "giDiffuse is all blue" → env-intensity
calibration + sample-default alignment), then **removed from the engine** on
2026-08-24 (commits `c4c29c0` + `349eda9`). Immediately afterwards the
**entire IBL system was also removed** (`0a381ef`: `VulkanIbl.*` deleted, all
`ibl` shaders deleted, `LightSystem` ambient zeroed). The current tree has
**no ambient light at all** — shadowed areas are pure black.

That changes the re-attempt fundamentally — for the better:

- **GI becomes the sole ambient source**, exactly the AMD sample's model
  (its `lightinggi.hlsl` composites direct sun + GI diffuse/specular; there
  is no IBL alongside). The first attempt's hardest problem — calibrating GI
  against a scaled-down IBL (`giIblSpecScale`, boost factors, the "no
  difference" and "all blue" debug sagas) — is structurally gone.
- The previously-validated engine code is recoverable wholesale from git
  (commit `be60942`, the last working state before removal).
- The SDK side is fully done and stays done: `brixelizer` + `brixelizergi`
  remain in `ENABLED_COMPONENTS`, all fork patches are committed in the
  fsr3.1 fork (context-size bumps, `constantBuffers[4]` OOB fix, per-voxel
  reference clamp, 2D→3D distance-cull port, descriptor-pool fix), and both
  archives (linux + win) are built. `nm` confirms all symbols exported.

## Status

- [ ] R0 — Preflight + restore inventory
- [ ] R1 — Voxelizer core restore + self-test
- [ ] R2 — GI dispatch + environment cube restore
- [ ] R3 — Consumption: GI as sole ambient
- [ ] R4 — Tuning, validation, docs

## What is restored vs re-written

### Wholesale restore from `be60942` (new files, no conflicts)

| File | Lines | What |
| ---- | ----- | ---- |
| `c-engine/renderer/vulkan/pass/gi_brixelizer/VulkanBrixelizerPass.h` | 114 | pass + instance-manager + GI API |
| `c-engine/renderer/vulkan/pass/gi_brixelizer/VulkanBrixelizerPass.cpp` | 3386 | FFX contexts, voxelizer, GI dispatch, self-test, dumps |
| `c-game/game/azgaar/AzgaarBrixelizerProxy.h/.cpp` | 46+1121 | terrain LOD batches, props, character dynamic instance |
| `c-engine/data/pak_0_engine/shaders/pass/gi/normal_decode.comp` | 40 | oct rg16f → rgba16f world-normal pre-pass |

### Hand re-application (shared files — **cannot** `git checkout`, the IBL
removal also touched them and its shader includes are gone)

`Vulkan.cpp` (include, `addPass(&vulkanBrixelizerPass)` between Composite and
TAA, `giSpec` debug-dump tokens) · `VulkanResourceManager.{h,cpp}`
(`vulkanResourceSetGi` + sampled-pool publish) · `globalset.shader` (`GiData`
block) · `scene.frag`, `heightmap_terrain.frag`, `azgaar_props.frag`,
`triangle.frag`, `oit_accumulate.frag`, `triangle_reflection.frag` (GI
branches — **new model**, see R3) · `composite.comp` (giSpec roughness-band
blend) · `AzgaarProps.{h,cpp}` (GI-LOD swap hooks) · `AzgaarStreaming.cpp`
(proxy lifecycle) · `Player.cpp` (5-line hook) · `graphics.html` +
`SettingsGraphicsGui` (toggle + quality slider).

### New code (did not exist before)

1. **Environment cubemap source** — `desc.environmentMap` was the IBL
   prefiltered cube; IBL is gone. See R2.
2. **BRDF integration LUT** for the GI-specular split-sum — the sample's
   `getIBLContribution` uses `reflectance0 * brdf.x + reflectance90 * brdf.y`;
   `brdf_lut.frag` was deleted with IBL. See R3.
3. `gi_common.shader` include — shared GI composite math for the material
   passes (replaces what `ibl_common.shader` did for IBL).

## Matrix conventions (the DX12-vs-Vulkan trap)

The SDK headers say every matrix is "row major". **That comment documents
the DX12 build only.** On Vulkan the FFX backend uploads the constant
buffers with a raw `memcpy` and the GLSL passes declare `mat4` uniforms —
which GLSL reads **column-major**. The previously-validated mapping:

| FFX API input | Engine source | Layout to pass |
| ------------- | ------------ | -------------- |
| `desc.view` / `desc.projection` / `desc.prevView` / `desc.prevProjection` | `cameraUbo.view` / `.projection` (+ cached prev) | **cglm column-major, raw `memcpy` — NO transpose** |
| debug-vis `inverseViewMatrix` / `inverseProjectionMatrix` | `cameraUbo.invView` / `.invProjection` | **column-major `memcpy`** |
| instance `transform` (3×4, 12 floats) | proxy-built | **row-major**: 3 rows of `(basis_i \| t_i)`, translation in `.w` of each row |
| `cameraPosition` | `invView[12..14]` | column-major translation slots |
| `motionVectorScale` | engine velocity buffer | `(-1/w, -1/h)` — see below |

Evidence trail (do not re-derive, but re-verify cheaply at R1):

- Row-major (transposed) view/projection input produces an **all-miss SDF
  trace with no error anywhere** — rays unproject into scrambled directions.
  Found in 6.1 by A/B against the debug-visualization trace image; re-verified
  in the 08-24 forensics (decoded all 8 mat4s in the capture: column-major,
  `inv_view·view = I`, `inv_view` translation == camera position).
- The row-major 3×4 instance transform **is** correct as documented: the GLSL
  `LoadInstanceTransform` builds `mat3x4` from three `vec4`s and applies
  `mat * vec4` — the 12 floats are three rows (basis + translation in w).
  The self-test's spatial asserts (hit blob at the projected cube position)
  are the guard: a scrambled transform fails them.
- **Verification protocol**: any matrix doubt is settled by the SDK SDF-trace
  debug-vis (`ENGINE_BRIXELIZER_DEBUGVIS=1`, ITERATIONS/GRADIENT modes) plus
  the self-test spatial asserts — not by reading the docs.

Other non-matrix conventions (from the validated run):

- `FFX_BRIXELIZER_GI_FLAG_DEPTH_INVERTED` — engine is reverse-Z D32
  (`ffxIsBackground(d) = d < 1e-12`, closer = larger). Correct, verified.
- `normalsUnpackMul/Add = 1/0` — the GI shader's decode is affine mul/add
  only, so the oct rg16f G-buffer **must** be pre-decoded by
  `normal_decode.comp` into a linear rgba16f world-normal image.
- `isRoughnessPerceptual = 1`, `roughnessChannel = 0` — the material image's
  R channel holds perceptual roughness (SSSR lesson).
- Velocity buffer is in **pixels**, cur−prev, y-down; the GI reprojection is
  `prev = cur + mv·scale`, so `motionVectorScale = (-1/w, -1/h)` (the same
  value the FSR pass passes for this buffer).

## GI dispatch input → engine source map (current tree)

| GI input | Source | Notes |
| -------- | ------ | ----- |
| `depth` | `vulkanFrameResourcesGetDepth()` | D32 reverse-Z |
| `normal` | `giNormal` (from `normal_decode.comp`) | oct→linear pre-pass, same frame |
| `roughness` | `vulkanFrameResourcesGetMaterial()` | R8G8B8A8, perceptual R |
| `motionVectors` | `vulkanFrameResourcesGetVelocity()` | R16G16 pixels |
| `prevLitOutput` | `giLitHistory` (persistent copy of composite color) | multi-bounce: composite includes GI |
| `historyDepth/Normal` | persistent ping copies | copied after dispatch |
| `environmentMap` | **new** sky cubemap (R2) | `FFX_RESOURCE_STATE_GENERIC_READ` |
| `noiseTexture` | 16-plane `giNoise[frame % 16]` cache | restore old generation code |
| `view/proj/prev*` | `CameraUbo` | column-major memcpy |
| `sdfAtlas`/`bricksAABBs`/24×(tree+map) | the Brixelizer pass | GENERAL layout atlas, COMPUTE_READ buffers |
| raw context | `ffxBrixelizerGetRawContext()` | after this frame's update |
| outputs | `giDiffuse` / `giSpecular` rgba16f | `displaySize` = render res (FSR upscales later) |

CameraUbo has no separate `prevView`/`prevProjection` — cache them in the
pass after each dispatch (old code did exactly this; first frame reuses the
current matrices, `giHasPrev` flag).

## Phases

### R0 — Preflight + restore inventory (~half day)

- Verify the archive still links from a current tree: `./scripts/build.sh`
  (components are in the archive but unused — the build must be unchanged),
  `./scripts/run.sh play log 5000` regression-clean. The dormant state is
  bit-identical by design (lazy contexts).
- Extract the restore set from git for reference:
  `git show be60942:<path>` for the four wholesale files; `git show
  c4c29c0 -- <path>` diffs for the hand-applied hunks (they show exactly
  what was removed).
- Confirm `shaderFloat16` is still enabled in `Vulkan.cpp` (it is — the GI
  shaders declare SPIR-V Float16).
- Park this plan as the detailed plan; update the roadmap Phase 6 pointer.

### R1 — Voxelizer core restore + self-test (~1 day)

Restore `VulkanBrixelizerPass.{h,cpp}` + `normal_decode.comp` (the GI half
of the pass can initially stay compiled-out — but restoring it whole is
simpler; it self-gates on `ENGINE_GI_ENABLED`).

- Pass lands between **Composite and TAA** (`addPass` slot). FFX backend
  sized for both contexts; lazy create; `vulkanWaitIdle` + recreate on
  resize; teardown in `removed()`.
- Host resources: 512³ R8 3D SDF atlas (128 MB, `vulkanCreateImage` supports
  `VK_IMAGE_TYPE_3D`), `brickAABBs` + 24×(aabbTree+brickMap) SSBOs, scratch
  buffer that grows via `outScratchBufferSize` (realloc on waitIdle).
- Cascades: **7, STATIC|DYNAMIC, 0.25 m base doubling to 16 m**
  (`ENGINE_BRIX_VOXEL` tunable). Raw layout
  `[static 0..N-1][dynamic N..2N-1][merged 2N..3N-1]` — trace/GI use the
  **merged** range.
- Restore the game proxy (`AzgaarBrixelizerProxy`): terrain 3-level
  static batches (8 m/±4 km, 64 m/±32 km, 160 m/80 km), props per
  (species,variant) position-only meshes + streamed tile instances,
  character dynamic instance. Re-apply the `AzgaarProps` GI-LOD swap
  (near trees voxelize `_far` geometry — the 29.3M→1.5M triangle fix) and
  the `AzgaarStreaming` lifecycle hooks.
- **Gate: `ENGINE_BRIXELIZER_SELFTEST=1`** must pass (synthetic cube+sphere+
  orbiting cube; 64 round-robin updates; SDF-trace readback with spatial +
  dynamic-motion asserts). This validates matrices (see the conventions
  section), merged-cascade range, and the dynamic refresh path — before any
  GI wiring.
- Live-world check: `ENGINE_BRIXELIZER_ENABLED=1`, `uninit=0` on all
  cascades, ~11.8k bricks in cascade 0, `triSum` sane (~1.5M/bake), no
  MarkFailed/failed-voxel wedging.

### R2 — GI dispatch + environment cube (~1–2 days)

Restore the GI half: context (`DEPTH_INVERTED | specular | denoiser` on,
internal resolution default 50%), persistent images, 16-plane noise cache,
history copies, dumps/stats tooling.

**New: the environment cubemap.** With IBL gone there is no cube to bind.
The sky is procedural (`skybox.frag` gradient). Recommended: **render the
actual skybox shader into a 128² RGBA16F cubemap once at GI init** (6 faces,
per-face view matrices; guarantees the GI miss term matches the visible sky —
one source of truth). Alternative (cheaper plumbing): CPU-generate the cube
by porting the gradient math — acceptable only with a "keep in sync with
skybox.frag" comment. Reuse the old `wrapImageResourceCube` (re-derives the
cube flag from `viewType`), bind as `FFX_RESOURCE_STATE_GENERIC_READ`.

- Dispatch the 19-pass chain after the voxelization update in the same
  command buffer. **Exactly one GI-context call per frame** (see gotchas);
  debug-vis alternates frames with the main dispatch.
- Prev-frame bookkeeping: cache `giPrevView/giPrevProjection`, publish
  `giDiffuse/giSpecular` through the resource manager sampled pool.
- **Gate**: `ENGINE_GI_DUMP=<late frame ≥ 500>` — float-domain stats
  (diffuse mean, red/blue dominance) + `ENGINE_GI_DEBUG=1|2` radiance/
  irradiance-cache vis show scene content (the 08-24 session-3 evidence
  tables are the reference: direct-SH wall bricks at full magnitude, probe
  hit histogram 2–8 m band, red bleed present in giDiffuse).

### R3 — Consumption: GI as sole ambient (~2–3 days)

The big delta from the first attempt. Follow the sample's
`getIBLContribution` model — no IBL to scale against:

```glsl
// gi_common.shader (new) — sample model, lightinggicommon.h
vec3 giDiffuseTerm  = giDiffuse.rgb  * baseColor * diffuseGIFactor;   // 1.5
vec3 giSpecularTerm = giSpecular.rgb * (F0 * brdf.x + F90 * brdf.y) * specularGIFactor;  // 3.0
// ambient = giDiffuseTerm + giSpecularTerm;  NOT multiplied by shadow factors
```

- **Wire every material pass, not just scene**: `scene.frag`,
  `heightmap_terrain.frag`, `azgaar_props.frag` (terrain is most of the
  screen in this world — the first attempt's known gap: GI changed only ~7 %
  of pixels because terrain never received it), `triangle.frag`,
  `triangle_reflection.frag`, `oit_accumulate.frag` (transparents currently
  have no ambient at all — GI gives them one for free via prevLit).
- **BRDF split-sum LUT** (new): the specular term needs `brdf.x/y`.
  Recommended: CPU-generate a 512² RG16F LUT at startup (standard Karis
  GGX integration loop, ~40 lines), disk-cache like `gi_noise16.bin` — no
  shader restore, no pipeline. Fallback if precision matters: restore
  `brdf_lut.frag` from `0a381ef~1` and port it to compute
  (`normal_decode.comp` pattern).
- Direct-sun shadows must **not** attenuate the GI ambient (probes already
  trace visibility — the session-4 fix). Keep CACAO as a local multiply on
  top if it reads well.
- `composite.comp`: keep the existing SSR blend for sharp reflections; the
  restored roughness-band `giSpec` mix (`smoothstep(0.2,0.45,r) * (1 -
  smoothstep(0.55,0.8,r))`) covers the mid-roughness band where SSR dies.
  Matte surfaces get rough specular from `giDiffuse` alone — no IBL
  prefilter exists anymore, by design.
- Settings GUI: "Global Illumination" toggle + internal-resolution slider
  (25/50/75/100 %), quality change recreates the GI context. **Default on**
  — it is the ambient; toggling off gives pure-black shadows (an honest
  debug view, and the A/B is unmissable — the first attempt's "no
  difference" calibration trap cannot recur).
- Env overrides (headless validation): `ENGINE_GI_ENABLED / _QUALITY /
  _DEBUG / _TMAX / _PUSHOFF / _SPEC_PUSHOFF / _SPEC_ROUGHNESS / _DENOISER /
  _ENV_INTENSITY / _DIFFUSE_BOOST / _SPECULAR_BOOST / _DUMP / _DUMP_RECT /
  _STATS_INTERVAL / _AMBIENT_OFF`.

### R4 — Tuning, validation, docs (~1–2 days)

- Parked-camera A/B on/off (screenshot + float stats), with the
  `DebugMarker` cube as the albedo probe (it survived the removal, emissive
  already removed — it exists *for* this).
- Light-bleed checks through walls/ground; grazing-angle terrain check (the
  old "house color mirrored on terrain" bug — the fixed mid-roughness band
  weighting is the guard).
- Streaming pop-in: teleport across the x = −2048 m tile boundary (user
  approves moving the parked player; restore db bit-exact afterwards).
- Perf: bake ~0.81 ms + dispatch ~1.63 ms at 2880×1627 @50 % (previous
  measurements) — re-measure; per-cascade cost report
  (`vulkanCreateProfile("gi_brixelizer")` + `FfxBrixelizerStats`).
- Temporal stability while strafing (motion-vector scale + reprojection);
  NaN/feedback-loop watch via `ENGINE_GI_STATS_INTERVAL` (tooling restored
  in R2 — it exists because the last session used it).
- Docs: restore/refresh the `docs/fsr3.1.md` engine-integration section,
  roadmap Phase 6 status, this file's status blocks. Both archives are
  already built (win + linux) — only `release.sh` smoke links if touched.
- Optional quality follow-ups (only if artifacts show): blue-noise LDR_RG01
  planes instead of procedural white noise; frozen spawn jitter when the
  camera is static (the TempSpawnMask==0 observation — re-traces every
  frame; a perf idea, not a bug).

## Gotchas checklist (all empirically established — do not rediscover)

1. **One GI-context call per frame max.** Every dispatch's
   `UnregisterResourcesVK` advances the backend's per-frame view-slot
   rotation; a second call in the same frame destroys views still in flight
   (validation error). Debug-vis alternates frames.
2. **Merged cascades only**: trace `2N..3N−1` (raw layout
   static/dynamic/merged). Static- or dynamic-only ranges trace stale
   geometry.
3. **SDF atlas lives in GENERAL forever** — the brixelizer passes bind it as
   UAV and sampled with fixed GENERAL layout. Wrap with GENERAL-mapped
   states so the backend emits no hidden transitions; the engine's layout
   tracker desyncs otherwise (readback copies after trace frames need
   explicit GENERAL→TRANSFER_SRC barriers and back).
4. **`FFX_BRIXELIZER_CONTEXT_FLAG_ALL_DEBUG`** makes the stats readback work
   (silently all-zero without it) but the backend `vkCmdCopyBuffer`s debug
   counters out of the scratch buffer — it needs
   `VK_BUFFER_USAGE_TRANSFER_SRC_BIT`.
5. **One transient command buffer per frame** in self-tests — the FFX
   backend destroys per-update image views in rotating per-queued-frame
   slots; a 64-frame buffer references destroyed views.
6. **Dynamic instances**: the high-level API gives DYNAMIC-flag instances no
   `outInstanceID`, and deletes only compact the static range — the per-frame
   refresh uses the STATIC flag with delete+recreate.
7. **GI-LOD swap** for authored trees (87k–225k tris/variant): voxelize the
   `_far` simplified geometry near, or the reference/triangle budgets blow
   (MarkFailed voxels wedge the brick map permanently — the fork's reference
   clamp + distance cull are defense, not cure).
8. **`FFX_BRIX_LOG` stride must be odd** (one bake/frame ⇒ even strides
   never sample cascade 0).
9. The character mesh is on an armature child of the player entity — the
   proxy scans for the largest `Mesh`, caches entity + local AABB;
   `totalIdx` is indices, triangles = `/3`.
10. `TERM=xterm` for non-interactive runs (`run.sh`'s `clear` under
    `set -e`).
11. Env-map + atlas wrapped `FFX_RESOURCE_STATE_GENERIC_READ`,
    buffers `FFX_RESOURCE_STATE_COMPUTE_READ` — layout-preserving states.
12. GI outputs are rgba16f = engine R16G16B16A16_SFLOAT — no format fork
    patch needed (unlike CACAO/SPD/DOF).
13. First frames: radiance cache starts empty (misses read env map at 0.1
    intensity); judge dumps at frame ≥ 500. `vulkanSaveImage` normalizes
    per-channel — the logged float stats are the ground truth.
14. One-frame-lag consumption is inherent (materials sample last frame's
    `giDiffuse/giSpecular`); the history copies (depth/normal/composite)
    happen after the dispatch in the same pass.

## Budgets (previous-run measurements)

- VRAM: atlas 128 MB + cascade SSBOs ~41 MB + GI internals ~50–150 MB (at
  internal res) + persistent history images ≈ **< 0.5 GB**.
- CPU: contexts ≈ 35 MB (Linux `wchar_t`-inflated) + 8.4 MB baked update.
- GPU/frame: bake 0.81 ms + GI dispatch 1.63 ms @ 2880×1627, 50 % internal,
  cascade-0 ≈ 152k tris/bake. Quality slider covers the dispatch cost.
- Instance cap 65 536 — grass/reeds excluded by the proxy (species filter).

## Risks

1. **Env-map/sky mismatch** (CPU-port drift) — mitigated by rendering the
   real skybox shader into the cube (R2 recommendation).
2. **Terrain GI wiring is new surface area** (first attempt never did it) —
   terrain materials are custom mixes; the shared `gi_common.shader` must
   handle the pass-specific material paths. Keep the per-pass GI branch
   small: ambient += gi terms, nothing else.
3. **Specular without IBL prefilter**: rough-matte specular now comes only
   from `giDiffuse` + the mid-band `giSpec` — flatness risk vs the old IBL
   path; the boosts are the knobs, tuned with the DebugMarker cube and
   float stats, not eyeballs-on-jpg (normalization crushes HDR).
4. **`prevLitOutput` feedback** (composite-with-GI feeds next frame's
   radiance cache): multi-bounce by design, but watch for runaway with
   `ENGINE_GI_STATS_INTERVAL` (the tooling exists for exactly this).
5. CameraUbo lacks prevView/prevProjection — cached in-pass (old pattern);
   if the camera system ever exposes them properly, switch.
