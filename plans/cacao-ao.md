# CACAO AO Integration

Integrate AMD CACAO 1.4 (ambient occlusion) from the FSR 3.1 SDK source
(`cpp-thirdparty/fsr3.1`) into the engine, as a selectable alternative to the
existing XeGTAO-based AO pass.

## Background

- CACAO ships in the FSR 3.1 SDK but our `build.sh` only compiles the
  FSR3 upscaler component. CACAO pieces available in the SDK:
  - Host API: `include/FidelityFX/host/ffx_cacao.h` — 4-call API:
    `ffxCacaoContextCreate` / `ffxCacaoContextDispatch` /
    `ffxCacaoContextDestroy` / `ffxCacaoUpdateSettings`
  - Component: `src/components/cacao/ffx_cacao.cpp`
  - VK GLSL shaders: `src/backends/vk/shaders/cacao/` (31 compute passes)
  - Blob accessor: `src/backends/shared/blob_accessors/ffx_cacao_shaderblobs.cpp`
    (unconditionally includes 124 permutation headers: 31 shaders ×
    {base, wave64, 16bit, wave64_16bit})
- CACAO is runtime-independent of FSR: it reads depth + normals, writes an AO
  buffer. Works with FSR, TAA, or native — it just post-processes the
  framebuffer we give it.
- The engine already has `c-engine/renderer/vulkan/pass/ao/VulkanAOPass.cpp`
  (XeGTAO ray pass + G-TAO temporal accumulation, HiZ-assisted). The
  composite pass consumes it via `vulkanAOPassGetOutput()` /
  `vulkanAOPassIsDisabled()` (absent-sentinel index pattern).
- Inputs available for CACAO:
  - depth: `vulkanFrameResourcesGetDepth()` — `D32_SFLOAT`, stores **view
    depth** (not clip [0,1]) → `DepthUnpackConsts` = mul 1.0, add 0.0
  - normals: `vulkanFrameResourcesGetNormals()` — `R16G16B16A16_SFLOAT`,
    view-space, [-1,1] → `normalUnpackMul` = 1, `normalUnpackAdd` = 0
    (or set `generateNormals = true` and let CACAO reconstruct from depth)
  - projection + normal matrix: from `CameraUbo`
- CACAO output is `rgba16f`, `.r` = AO (1 = unoccluded) — same convention as
  the existing AO accumulator's `.r`, so the composite sampling is
  compatible.

## Phase 1 — Third-party build (`cpp-thirdparty/fsr3.1/build.sh`)

Goal: extend the existing build so the static library contains both
`fsr3upscaler` and `cacao` components. Keep a **single library**
(`libffx_fsr3upscaler_vk.a`) — a second `.a` would duplicate the shared
objects (`ffx_vk.o`, `ffx_shader_blobs.o`, …) and complicate linking.
No game-side CMake change needed (already linked in `c-game/CMakeLists.txt`).

### 1.1 Compile CACAO shaders (124 permutation headers)

Mirror the existing fsr3upscaler loop in `build.sh`, using the args from
`include/FidelityFX/gpu/cacao/CMakeCompileCACAOShaders.txt` +
`src/backends/vk/CMakeShadersCACAO.txt`:

- base args: `-reflection -deps=gcc -DFFX_GPU=1`
- api args: `-compiler=glslang -e CS --target-env vulkan1.2 -S comp -Os -DFFX_GLSL=1`
- permutation args: `-DFFX_CACAO_OPTION_APPLY_SMART={0,1}`
- includes: `-I$GPU_DIR -I$GPU_DIR/cacao`
- 4 variants per shader (same scheme as fsr3upscaler, see
  `include/FidelityFX/gpu/CMakeCompileShaders.txt`):
  - `-name=<shader>` with `-DFFX_HALF=0`
  - `-name=<shader>_wave64` with `-DFFX_HALF=0`
  - `-name=<shader>_16bit` with `-DFFX_HALF=1`
  - `-name=<shader>_wave64_16bit` with `-DFFX_HALF=1`
- Output into the same `src/backends/vk/shader_output/` directory.
- All 31 shaders listed in `CMakeCompileCACAOShaders.txt`
  (apply, apply_non_smart[_half], clear_load_counter, edge_sensitive_blur_1..8,
  generate_importance_map[_a/_b], generate_q0..q3[_base],
  prepare_downsampled_depths[_and_mips/_half], prepare_native_depths[...],
  prepare_downsampled_normals[_from_input_normals],
  prepare_native_normals[_from_input_normals], upscale_bilateral_5x5).

The 16-bit variants are required even though the engine will use 32-bit
permutations: `ffx_cacao_shaderblobs.cpp` includes all 124 headers
unconditionally.

### 1.2 Add C++ sources + define

Append to `SOURCES` in `build.sh`:

```
src/components/cacao/ffx_cacao.cpp
src/backends/shared/blob_accessors/ffx_cacao_shaderblobs.cpp
```

Add `-DFFX_CACAO` to `DEFINES` (alongside `-DFFX_FSR3UPSCALER`).
`ffx_shader_blobs.cpp` already routes `FFX_EFFECT_CACAO` to the cacao
accessor when that define is set; `ffx_vk.cpp` is generic and untouched.

### 1.3 Verification

- `shader_output/` gains 124 `ffx_cacao_*_permutations.h` (+ blob headers).
- Both Linux and Windows `.a` build clean (warning-strict flags).
- `nm build-linux/libffx_fsr3upscaler_vk.a | grep ffxCacaoContextCreate`
  resolves.
- **Context-size guard:** `FfxCacaoContext` is `uint32_t data[301054]`
  (≈1.2 MB) and the private struct holds 33 × `FfxPipelineState`, each with
  `wchar_t name[64]` (256 B on Linux vs 128 B on Windows). The 1.2 MB budget
  is far larger than the struct (~20 KB), so no overflow is expected — but
  add a `static_assert(sizeof(FfxCacaoContext_Private) * 4 <=
  FFX_CACAO_CONTEXT_SIZE)`-style check (or a one-off size print) during the
  build to confirm, given the earlier `FFX_SDK_DEFAULT_CONTEXT_SIZE` wchar_t
  patch.
- Update `docs/fsr3.1.md` ("What's Built" / "What's NOT Built" tables).

## Phase 2 — Engine pass (`c-engine`)

### 2.1 New pass: `renderer/vulkan/pass/cacao/VulkanCacaoPass.{h,cpp}`

Follow the `VulkanAOPass` / `VulkanFsrPass` System pattern:

- `added()`: subscribe to `swapchainCreated`; create the `FfxInterface`
  exactly like the FSR pass (`ffxGetScratchMemorySizeVK` +
  `ffxGetInterfaceVK`, own scratch buffer).
- `swapchainCreated`: `ffxCacaoContextDestroy` old context, then
  `ffxCacaoContextCreate` with render width/height,
  `useDownsampledSsao = true` (recommended), the shared `FfxInterface`.
  Create the engine-owned output image (`R16G16B16A16_SFLOAT`,
  sampled + storage usage).
- `update()` (only when CACAO is the active AO impl and not disabled):
  - Wrap depth / normals / output via `ffxGetResourceVK` (extract the
    `wrapImageResource` helper out of `VulkanFsrPass.cpp` into a shared
    header, e.g. `VulkanFfxUtils.h`, so both passes use it).
  - `ffxCacaoUpdateSettings`: radius, quality level, `blurPassCount`,
    fade-out range, `generateNormals`, and the TSS offsets driven from the
    TAA jitter phase (`vulkanFsrGetJitterPhaseCount` /
    `vulkanFsrGetJitterOffset` already exist in `VulkanFsrUtils.h`).
  - `ffxCacaoContextDispatch(cmd, depth, normals, output, proj,
    normalsToView, 1.0f, 0.0f)`.
- Expose: `vulkanCacaoPassGetOutput()` (same contract as
  `vulkanAOPassGetOutput`), enable/disable + settings setters,
  cpu/gpu elapsed for the debug GUI.

### 2.2 AO implementation selector (keep composite untouched)

The composite pass keeps sampling `vulkanAOPassGetOutput()` /
`vulkanAOPassIsDisabled()`. Add to `VulkanAOPass`:

```c
typedef enum { AO_IMPL_XEGTAO = 0, AO_IMPL_CACAO } AoImpl;
void vulkanAOPassSetImpl(AoImpl impl);
```

- `AO_IMPL_XEGTAO` (default): current behavior, CACAO idle.
- `AO_IMPL_CACAO`: the XeGTAO ray/temporal dispatches are skipped;
  `vulkanAOPassGetOutput()` returns the CACAO output image instead.
- Disabled: existing clear-accumulator behavior (pixel-identical to no-AO).

This keeps the absent-sentinel logic and the composite in one place; CACAO
becomes an "AO algorithm" option, orthogonal to the upscaler choice
(FSR / TAA / native).

## Phase 3 — Game side + validation

- Graphics settings (c-game): AO algorithm option (XeGTAO / CACAO / Off),
  persisted like other settings. Env override for testing, e.g.
  `ENGINE_AO_IMPL=cacao` (mirrors the `ENGINE_AO_DISABLED` pattern).
- Build: `./scripts/build.sh` (full pipeline incl. the updated ffx lib).
- Visual check (parked player, do not move it):
  - `ENGINE_HIDE_GUI=1 ./scripts/run.sh play screenshot /tmp/ao_xegtao.jpg`
  - `ENGINE_AO_IMPL=cacao ENGINE_HIDE_GUI=1 ./scripts/run.sh play screenshot /tmp/ao_cacao.jpg`
  - Compare contact-shadow quality in crevices/corners; verify no
    self-shadowing artifacts (tune `horizonAngleThreshold` if needed).
- Verify the AO convention matches (`.r` = 1 unoccluded) — if CACAO's
  output is inverted, flip in the composite or a 1-tap fixup.
- Depth/normal convention check: D32 view-depth with `DepthUnpackConsts`
  (1, 0) — a wrong convention shows up as "AO everywhere" or "no AO";
  confirm with the screenshots above.
- GPU cost: compare `vulkanAOPass.gpuElapsed` (XeGTAO) vs CACAO dispatch
  time; optionally RenderDoc capture (`docs/renderdoc-capture.md`) to
  inspect the 31 CACAO passes.
- TAA interaction: with FSR off + TAA on, confirm temporal stability
  (no AO shimmer) — CACAO's internal Q-mips + importance map provide its
  own temporal behavior; the TSS angle offset should track the jitter
  sequence.

## Open questions

1. Replace or coexist: plan assumes **coexist as a setting** (XeGTAO stays
   default). If CACAO is meant to fully replace XeGTAO, drop the selector
   and delete the XeGTAO pipelines in Phase 2.2.
2. `useDownsampledSsao`: start with `true` (recommended, cheaper); the
   bilateral 5×5 upscale pass reconstructs full-res AO.
3. Normal source: start with the existing normal buffer (unpack 1/0);
   `generateNormals = true` is the fallback if the normal buffer proves
   unsuitable (e.g. missing for some surfaces).