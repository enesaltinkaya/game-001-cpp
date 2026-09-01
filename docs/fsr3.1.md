# FSR 3.1 — Static Library Build (Vulkan, component registry)

Custom build of the AMD FidelityFX FSR 3.1 SDK for use from a **C++ game engine**
via Vulkan. Currently enabled via `ENABLED_COMPONENTS` in `build.sh`:
the **FSR3 Upscaler**, **CACAO** (ambient occlusion), **SPD** (Single Pass
Downsampler), **Lens**, **DOF** and **LPM** (Luma Preserving Mapper —
tone/gamut mapping, replacing the engine's custom tonemapping) components
(no frame generation, no
DX12 backend). **SSSR** + **Denoiser** (SSSR's temporal denoiser) were
integrated in Phase 5 of the SDK-expansion plan but were reverted on
2026-08-23 in favor of the engine's custom SSR pass — their registry blocks
and SDK fork patches are kept, so re-enabling is a one-line
`ENABLED_COMPONENTS` edit. The SDK tree ships 21 components; enabling more
is a registry edit (see "Build Script" below).

## What's Built

### Static Libraries

| Platform                | Output                                         |
| ----------------------- | ---------------------------------------------- |
| Linux (clang++)         | `git/sdk/build-linux/libffx_fsr3upscaler_vk.a` |
| Windows (clang++/MinGW) | `git/sdk/build-win/libffx_fsr3upscaler_vk.a`   |

### Compiled C++ Sources

| File                                                                  | Purpose                                                                                   |
| --------------------------------------------------------------------- | ----------------------------------------------------------------------------------------- |
| `src/components/fsr3upscaler/ffx_fsr3upscaler.cpp`                    | FSR3 upscaler core logic                                                                  |
| `src/components/cacao/ffx_cacao.cpp`                                  | CACAO AO core logic (`ffxCacaoContextCreate/Dispatch/Destroy`, `ffxCacaoUpdateSettings`)  |
| `src/components/dof/ffx_dof.cpp`                                      | DOF core logic (`ffxDofContextCreate/Dispatch/Destroy`, `ffxDofCalculateCoc{Scale,Bias}`) |
| `src/components/lpm/ffx_lpm.cpp`                                      | LPM core logic (`ffxLpmContextCreate/Dispatch/Destroy`, `FfxPopulateLpmConsts`)           |
| `src/shared/ffx_assert.cpp`                                           | Assert/debug utilities                                                                    |
| `src/shared/ffx_message.cpp`                                          | Message/logging utilities                                                                 |
| `src/shared/ffx_object_management.cpp`                                | Internal object management                                                                |
| `src/backends/vk/ffx_vk.cpp`                                          | Vulkan backend implementation                                                             |
| `src/backends/shared/ffx_shader_blobs.cpp`                            | Shader blob dispatch (routes by `FFX_FSR3UPSCALER` / `FFX_CACAO` defines)                 |
| `src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp` | Precompiled SPIR-V shader permutations (FSR3)                                             |
| `src/backends/shared/blob_accessors/ffx_cacao_shaderblobs.cpp`        | Precompiled SPIR-V shader permutations (CACAO)                                            |
| `src/backends/shared/blob_accessors/ffx_dof_shaderblobs.cpp`          | Precompiled SPIR-V shader permutations (DOF)                                              |
| `src/backends/shared/blob_accessors/ffx_lpm_shaderblobs.cpp`          | Precompiled SPIR-V shader permutations (LPM)                                              |
| `../../ffx_stubs.cpp`                                                 | Stubs for unbuilt components (breadcrumbs, frame interpolation swapchain)                 |

All SDK paths relative to `git/sdk/`. `ffx_stubs.cpp` lives next to `build.sh`.

### Compiled Shaders (GLSL → SPIR-V)

FSR3 upscaler: 10 shaders × 4 variants (wave32, wave64, 16bit, wave64+16bit)
= 40 permutation headers.

CACAO: 31 shaders × 4 variants = 124 permutation headers (all variants are
required — `ffx_cacao_shaderblobs.cpp` includes them unconditionally, even
though the engine uses the 32-bit permutations). CACAO permutation args come
from `gpu/cacao/CMakeCompileCACAOShaders.txt` (only
`-DFFX_CACAO_OPTION_APPLY_SMART={0,1}`).

DOF: 5 shaders × 8 permutations (MAX_RING_MERGE_LOG × COMBINE_IN_PLACE ×
REVERSE_DEPTH) × 4 variants. The VK backend forces `fp16Supported = false`
(1080 Ti compat), so the engine selects the 32-bit permutations — the
internal UAVs (bilateral color, near/far, radius) stay `rgba32f`/`rg32f`,
matching the GLSL qualifiers without any host-side format patch.

Output: `git/sdk/src/backends/vk/shader_output/`.

Shaders compiled via `wine git/sdk/tools/binary_store/FidelityFX_SC.exe`,
through a bounded parallel pool (wine takes ~2s to tear down per run;
override with `MAX_SHADER_JOBS=N`).

**Wine gotcha:** `-output` must be a _relative_ path (the script runs from
`git/sdk`). The wine prefix's drive mappings (`x: -> /home/enes`) make the
SC tool resolve absolute POSIX paths against the current drive, silently
writing to `/home/enes/home/enes/...` instead.

FSR3 upscaler shaders:

- `ffx_fsr3upscaler_accumulate_pass`
- `ffx_fsr3upscaler_autogen_reactive_pass`
- `ffx_fsr3upscaler_debug_view_pass`
- `ffx_fsr3upscaler_luma_instability_pass`
- `ffx_fsr3upscaler_luma_pyramid_pass`
- `ffx_fsr3upscaler_prepare_inputs_pass`
- `ffx_fsr3upscaler_prepare_reactivity_pass`
- `ffx_fsr3upscaler_rcas_pass`
- `ffx_fsr3upscaler_shading_change_pass`
- `ffx_fsr3upscaler_shading_change_pyramid_pass`

CACAO shaders (31): `ffx_cacao_apply_pass`, `ffx_cacao_apply_non_smart_pass`,
`ffx_cacao_apply_non_smart_half_pass`, `ffx_cacao_clear_load_counter_pass`,
`ffx_cacao_edge_sensitive_blur_{1..8}_pass`,
`ffx_cacao_generate_importance_map{,_a,_b}_pass`,
`ffx_cacao_generate_q{0,1,2,3}_pass`, `ffx_cacao_generate_q3_base_pass`,
`ffx_cacao_prepare_downsampled_depths{,_and_mips,_half}_pass`,
`ffx_cacao_prepare_native_depths{,_and_mips,_half}_pass`,
`ffx_cacao_prepare_downsampled_normals{,_from_input_normals}_pass`,
`ffx_cacao_prepare_native_normals{,_from_input_normals}_pass`,
`ffx_cacao_upscale_bilateral_5x5_pass`.

DOF shaders (5): `ffx_dof_downsample_depth_pass`,
`ffx_dof_downsample_color_pass`, `ffx_dof_dilate_pass`, `ffx_dof_blur_pass`,
`ffx_dof_composite_pass`.

LPM: 1 shader, no permutation axes → 1 × 4 variants = 4 permutation
headers: `ffx_lpm_filter_pass`.

SSSR + Denoiser shaders (5 + 8, **currently not built** — see intro): the
registry blocks in `build.sh` remain, so these lists document what a
re-enable compiles: `ffx_sssr_{depth_downsample, classify_tiles,
prepare_blue_noise_texture, prepare_indirect_args, intersect}_pass` (single
`FFX_SSSR_OPTION_INVERTED_DEPTH={0,1}` axis; the engine selects the
inverted-depth permutation) and
`ffx_denoiser_{prepare_shadow_mask, shadows_tile_classification,
filter_soft_shadows_{0,1,2}, prefilter_reflections, reproject_reflections,
resolve_temporal_reflections}_pass` (all 8 compiled; only the 3 reflections
passes are ever dispatched — the SSSR-embedded denoiser context is created
with `FFX_DENOISER_REFLECTIONS`).

## What's NOT Built

- **DX12 backend** (`src/backends/dx12/`) — not needed
- **ffx-api layer** (`ffx-api/`) — has hard-coded Windows/DX12 dependencies; we use the SDK-level API directly
- **Frame generation / interpolation** (`src/components/frameinterpolation/`, `src/components/opticalflow/`)
- **All other FidelityFX effects** (blur, CAS, FSR1, FSR2, VRS, etc.)
- **All other shader blob accessors** — only the accessors of the enabled components (`ffx_fsr3upscaler`, `ffx_cacao`, `ffx_spd`, `ffx_lens`, `ffx_dof`, `ffx_lpm`) are compiled

## SDK Header Patches

The upstream SDK headers are C++ oriented. The following patches make them
includable from C11 code (committed in git):

### `sdk/include/FidelityFX/host/ffx_types.h`

- Added `#include <stdbool.h>` for C (SDK uses `bool` in structs)
- Wrapped `#include <mutex>` / `<shared_mutex>` in `#ifdef __cplusplus`; C gets `#define FFX_MUTEX int` (dummy, not used in C API)
- Fixed `FfxDescriptiorType` typo → added `FfxDescriptorType` typedef alias
- Fixed `FfxConstantAllocation` / `FfxRootConstantAllocation` typedef mismatch
- Guarded `static` member functions in `FfxResourceInitData` with `#ifdef __cplusplus`
- Guarded MSVC `#pragma warning` with `#ifdef _MSC_VER`
- Added braces to `s_FfxViewDescInit` union initializers (silence `-Wmissing-braces`)
- Increased `FFX_SDK_DEFAULT_CONTEXT_SIZE` on Linux (`1024*256` vs `1024*128`) — `wchar_t` is 4 bytes on Linux, making the private context struct larger

### `sdk/include/FidelityFX/host/ffx_util.h`

- Wrapped `ffxCountBitsSet()` (`noexcept`, `static_cast`) in `#ifdef __cplusplus` with a plain C fallback
- Added `#include <bit>`: `std::popcount` lives in
  `<bit>`; the header only compiled where `<bit>` was pulled in transitively
  (Linux libstdc++ did, llvm-mingw's headers did not)

### `sdk/include/FidelityFX/host/backends/vk/ffx_vk.h`

- Wrapped C++ default arguments in `#ifdef __cplusplus` / `#else` blocks (`ffxGetResourceVK`, `ffxGetBufferResourceDescriptionVK`, `ffxGetImageResourceDescriptionVK`)
- Guarded C++ reference parameters with `#ifdef __cplusplus` (`ffxReplaceSwapchainForFrameinterpolationVK`, `ffxGetFrameinterpolationCommandlistVK`)
- Added `typedef` to `struct FfxSwapchainReplacementFunctions`

### `sdk/src/backends/vk/ffx_vk.cpp`

- Replaced deprecated `std::wstring_convert<std::codecvt_utf8>` with `mbstowcs` / `wcstombs` (non-Windows path)
- Added fallback for `vkGetBufferMemoryRequirements2KHR`: if `vkGetDeviceProcAddr`
  returns NULL for the KHR name, retries with the Vulkan 1.1 core name
  `"vkGetBufferMemoryRequirements2"`. Modern drivers (e.g. NVIDIA 5090) may only
  expose the core symbol after the extension was promoted.
- Aligned `pEffectContexts` mapping in `CreateBackendContextVK` to 32 bytes
  before casting to `EffectContext*`, and added 32 bytes of padding to
  `ffxGetScratchMemorySizeVK`. The `EffectContext` struct is declared
  `alignas(32)` but the scratch buffer sub-allocation used sequential pointer
  arithmetic, producing misaligned addresses. Clang's optimizer emits `movdqa`
  (aligned SSE store) for the `nextDynamicResourceView` loop, which faults on
  misalignment. MSVC generates scalar stores for the same code, so this bug is
  latent on Windows — the UB exists but doesn't crash.

### `sdk/include/FidelityFX/host/ffx_cacao.h`

- `FFX_CACAO_CONTEXT_SIZE` is 301054 uint32s (≈1.2 MB) on Windows but the
  Linux `FfxCacaoContext_Private` is 2,302,456 bytes (33 `FfxPipelineState`
  members, each with `wchar_t name[64]` at 4 bytes). Bumped to 600000
  uint32s on non-Windows, guarded by `#if defined(_WIN32)` — same pattern as
  the `FFX_SDK_DEFAULT_CONTEXT_SIZE` patch above. The SDK's own
  `FFX_STATIC_ASSERT` in `ffxCacaoContextCreate` catches any regression at
  compile time.

### `sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h`

- Changed `rw_luma_history` image declaration from `rgba8` to `rgba16f`.
  The SDK creates `LumaHistory` as `FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT`, but
  the GLSL callback header declared the UAV with an `rgba8` image format
  qualifier. That mismatch triggered Vulkan validation warnings during the
  `luma_instability` pass (`rw_luma_history`) and is undefined behavior.

### `sdk/include/FidelityFX/gpu/cacao/ffx_cacao_callbacks_glsl.h`

- Fixed the storage-image format qualifiers to match the resource formats the
  SDK allocates in `ffx_cacao.cpp` (and the DX12 backend): the callbacks
  declared `g_RwDeinterleavedDepth`/`g_RwDepthMips` `r32f`,
  `g_RwDeinterleavedNormals` `rgba32f`, `g_RwSsaoBufferPing(Pong)` `rg32f`,
  and `g_RwImportanceMap(Pong)` `r32f`, but the host creates them as
  `R16_FLOAT`, `R8G8B8A8_SNORM`, `R8G8_UNORM`, and `R8_UNORM` respectively.
  Vulkan validation flagged the `OpTypeImage` vs `VkImageView` format mismatch
  on every CACAO dispatch (undefined behavior). Qualifiers changed to
  `r16f`/`rgba8_snorm`/`rg8`/`r8` — restoring the lean formats (DX12 parity)
  instead of fattening the host allocations. The glslang bundled in
  `FidelityFX_SC.exe` accepts these with `#version 450` (emits
  `StorageImageExtendedFormats`). All 124 CACAO permutation headers were
  regenerated and both static libs rebuilt.

### `sdk/include/FidelityFX/gpu/spd/ffx_spd_callbacks_glsl.h` (SPD round)

- Output UAV format qualifiers `rgba32f` → `rgba16f`
  (`rw_input_downsample_src_mips[]` + `rw_input_downsample_src_mid_mip`):
  the engine's HDR render targets are R16G16B16A16*SFLOAT, and the storage
  image format qualifier is baked into the precompiled SPIR-V — only
  R16G16B16A16_SFLOAT images can be downsampled (format-incompatible views
  are illegal, not just wasteful). Consequence: SPD here is a \_runtime HDR
  mip-chain* utility; load-time texture mips (8/16-bit UNORM/SRGB) keep the
  blit path. Engine dispatchers must set `FFX_RESOURCE_USAGE_ARRAYVIEW` on
  the wrapped resource: the shader's UAVs are `image2DArray`, so non-array
  images need `VK_IMAGE_VIEW_TYPE_2D_ARRAY` views (legal on single-layer
  images; handled by the backend when the flag is set).

### `sdk/include/FidelityFX/host/ffx_spd.h`

- `FFX_SPD_CONTEXT_SIZE` 9300 → 18000 uint32s on non-Windows (Linux
  `wchar_t` inflation, same pattern as the CACAO bump; the SDK's
  `FFX_STATIC_ASSERT` in `ffxSpdContextCreate` catches regressions).

### `sdk/src/backends/vk/ffx_vk.cpp` (SPD round)

- Global descriptor pool `poolSizeCount` 5 → 6: upstream declares 6 pool
  sizes (including `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER`) but passes 5, so
  any effect binding a storage buffer (SPD's atomic counter is the first)
  allocates from an unsized type — validation warning, OOM on strict
  drivers.

### `sdk/include/FidelityFX/gpu/dof/ffx_dof_callbacks_glsl.h` (DOF round)

- Output UAV `rw_output_color` qualifier `rgba32f` → `rgba16f`: the DOF
  output is an engine-provided image, and the engine's HDR render targets
  are R16G16B16A16_SFLOAT. The storage-image format qualifier is baked
  into the precompiled SPIR-V, so a `rgba32f` UAV could only legally bind
  an R32G32B32A32 image (4× the bandwidth of the engine convention). The
  engine always uses a separate output image (no `FFX_DOF_OUTPUT_PRE_INIT`),
  so the `COMBINE_IN_PLACE` `imageLoad`-as-input path is unaffected in
  practice. The internal UAVs (bilateral color / near / far / radius) keep
  their `rgba32f`/`rg32f` qualifiers — the VK backend forces
  `fp16Supported = false`, so the 32-bit permutations are selected and the
  host allocations (R32G32B32A32 / R32G32) already match.

### `sdk/include/FidelityFX/host/ffx_dof.h` (DOF round)

- `FFX_DOF_CONTEXT_SIZE` 45674 → 88000 uint32s on non-Windows (Linux
  `wchar_t` inflation: `FfxDofContext_Private` with its 5
  `FfxPipelineState` members is 349096 B = 87274 uint32s there; the SDK's
  `FFX_STATIC_ASSERT` in `ffxDofContextCreate` catches regressions).

### `sdk/include/FidelityFX/gpu/lpm/ffx_lpm_callbacks_glsl.h` (LPM round)

- Output UAV `rw_output_color` qualifier `rgba32f` → `rgba8` (non-HALF
  branch; the VK backend forces `fp16Supported = false`, so the 32-bit
  permutations are selected and this is the qualifier that gets baked into
  the SPIR-V). LPM's LDR display mode gamma-encodes its output
  (`ApplyGamma`), so the engine runs it on a display-referred 8-bit image —
  the same convention as the LENS output patch above. The LPM pass writes
  the result to an `R8G8B8A8_UNORM` image and blits it into the SRGB
  swapchain / lens input (format-compatible, swizzle only).

### `sdk/include/FidelityFX/host/ffx_lpm.h` (LPM round)

- `FFX_LPM_CONTEXT_SIZE` 9300 → 18000 uint32s on non-Windows (Linux
  `wchar_t` inflation, same pattern as the LENS/SPD/DOF bumps: the private
  context's single `FfxPipelineState` name buffers dominate; the SDK's
  `FFX_STATIC_ASSERT` in `ffxLpmContextCreate` catches regressions).

### `sdk/include/FidelityFX/gpu/sssr/ffx_sssr_callbacks_glsl.h` (SSSR round — kept in the fork, SSSR currently not built)

- Fixed the storage-image format qualifiers to match the resource formats the
  SDK allocates in `ffx_sssr.cpp`: the callbacks declared `rw_radiance`
  `rgba32f`, `rw_variance` `r32f`, `rw_extracted_roughness` `r32f`, and
  `rw_blue_noise_texture` `rg32f`, but the host creates them as
  `R16G16B16A16_FLOAT`, `R16_FLOAT`, `R8_UNORM`, and `R8G8_UNORM`
  respectively. The storage-image format qualifier is baked into the
  precompiled SPIR-V, so a `rgba32f` UAV could only legally bind an
  R32G32B32A32 image (4× the bandwidth, and a validation error against the
  actual R16F image). Qualifiers changed to `rgba16f`/`r16f`/`r8`/`rg8`
  (restoring the lean formats, DX12 parity). `rw_output` (the
  engine-provided reflection buffer, R16G16B16A16_SFLOAT) was patched
  `rgba32f` → `rgba16f` for consistency, though no SSSR/denoiser compute pass
  binds it — the final result reaches the output via a `vkCmdCopyImage`
  blit, not a UAV write. `rw_depth_hierarchy` stays `r32f` (host R32_FLOAT,
  already matching).

### `sdk/include/FidelityFX/host/ffx_denoiser.h` + `ffx_sssr.h` (SSSR round — kept in the fork, SSSR currently not built)

- `FFX_DENOISER_CONTEXT_SIZE` 73098 → 140000 uint32s on non-Windows (Linux
  `wchar_t` inflation: `FfxDenoiserContext_Private` with its 8
  `FfxPipelineState` members is 558608 B = 139652 uint32s there).
- `FFX_SSSR_CONTEXT_SIZE` 118914 → 228000 uint32s on non-Windows. The SSSR
  private context **embeds a full `FfxDenoiserContext`** (SSSR owns its own
  denoiser), so its Linux size is the sum of 5 SSSR `FfxPipelineState`
  members + the (bumped) denoiser context = 909664 B = 227416 uint32s. Both
  are guarded by `#if defined(_WIN32)`; the SDK's `FFX_STATIC_ASSERT` in
  `ffxSssrContextCreate` / `ffxDenoiserContextCreate` catches regressions at
  compile time.
- `ffx_sssr.cpp` / `ffx_denoiser.cpp`: explicit `static_cast<uint32_t>` on the
  buffer-size fields of the internal-resource initializer lists
  (`numPixels * sizeof(uint32_t)`, `sizeof(uint32_t) * tileCount`) — the
  strict `-Wc++11-narrowing` build rejects the implicit `size_t` → `uint32_t`
  narrowing in brace-initializers.

The denoiser's **reflections** UAV qualifiers already matched the host
formats (`rgba16f`/`r16f`/`r11f_g11f_b10f`), so no GLSL patch was needed for
it — only the context-size bump.

### `sdk/src/backends/vk/ffx_vk.cpp` (descriptor-pool round)

- `bindlessDescriptorPool` created with `flags = 0`, but the backend
  destroys its descriptor sets with `vkFreeDescriptorSets` on context
  teardown — legal only on pools created with
  `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT`. Any context destroy
  that churned FFX-internal descriptor sets (e.g. a per-frame
  create/destroy self-test) hit a validation CRIT. Universal fix, no
  platform guard. Both FFX archives (Linux + Windows) were rebuilt after
  this patch.

## Compatibility Header

`ffx_compat.h` (force-included during C++ library compilation via `-include`) provides:

- `_countof` macro
- `wcscpy_s`, `strcpy_s`, `sprintf_s` shims (Linux only, MinGW has them)
- `FFX_UNUSED` macro
- Missing standard includes (`<cstring>`, `<cwchar>`, `<cmath>`, `<new>`, `<mutex>`)

## Usage

### Headers

```c
#include <vulkan/vulkan.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
```

Include path: `-I<path-to>/fsr3.1/git/sdk/include`

### Linking

```
-lffx_fsr3upscaler_vk -lstdc++
```

Library path: `-L<path-to>/fsr3.1/git/sdk/build-linux/` or `build-win/`

### API Overview

#### 1. Create the VK backend interface

```c
// Query scratch buffer size
size_t scratchSize = ffxGetScratchMemorySizeVK(physicalDevice, 1);
void* scratch = malloc(scratchSize);

// Wrap VK device
VkDeviceContext vkDevCtx = {
    .vkDevice         = device,
    .vkPhysicalDevice = physicalDevice,
    .vkDeviceProcAddr = vkGetDeviceProcAddr
};
FfxDevice ffxDevice = ffxGetDeviceVK(&vkDevCtx);

// Create backend interface
FfxInterface backendInterface = {0};
ffxGetInterfaceVK(&backendInterface, ffxDevice, scratch, scratchSize, 1);
```

#### 2. Create the upscaler context

```c
FfxFsr3UpscalerContext upscalerCtx;

FfxFsr3UpscalerContextDescription desc = {0};
desc.maxRenderSize.width  = renderWidth;
desc.maxRenderSize.height = renderHeight;
desc.maxUpscaleSize.width  = displayWidth;
desc.maxUpscaleSize.height = displayHeight;
desc.backendInterface = backendInterface;
desc.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE    // if HDR
           | FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED        // if reverse-Z
           | FFX_FSR3UPSCALER_ENABLE_DEPTH_INFINITE         // if infinite far
           | FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;

ffxFsr3UpscalerContextCreate(&upscalerCtx, &desc);
```

#### 3. Per-frame: compute jitter and dispatch

```c
// Jitter
int32_t phaseCount = ffxFsr3UpscalerGetJitterPhaseCount(renderWidth, displayWidth);
float jitterX, jitterY;
ffxFsr3UpscalerGetJitterOffset(&jitterX, &jitterY, frameIndex % phaseCount, phaseCount);
// Apply jitterX, jitterY to projection matrix before rendering

// After rendering, dispatch upscaler
FfxFsr3UpscalerDispatchDescription dispatchDesc = {0};
dispatchDesc.commandList       = ffxGetCommandListVK(cmdBuffer);
dispatchDesc.color             = ffxGetResourceVK(colorImage, colorDesc, L"color", FFX_RESOURCE_STATE_COMPUTE_READ);
dispatchDesc.depth             = ffxGetResourceVK(depthImage, depthDesc, L"depth", FFX_RESOURCE_STATE_COMPUTE_READ);
dispatchDesc.motionVectors     = ffxGetResourceVK(mvImage, mvDesc, L"mv", FFX_RESOURCE_STATE_COMPUTE_READ);
dispatchDesc.output            = ffxGetResourceVK(outputImage, outputDesc, L"output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
dispatchDesc.jitterOffset.x    = jitterX;
dispatchDesc.jitterOffset.y    = jitterY;
dispatchDesc.motionVectorScale.x = -(float)renderWidth;   // adjust to your MV convention
dispatchDesc.motionVectorScale.y = -(float)renderHeight;
dispatchDesc.renderSize.width  = renderWidth;
dispatchDesc.renderSize.height = renderHeight;
dispatchDesc.upscaleSize.width  = displayWidth;
dispatchDesc.upscaleSize.height = displayHeight;
dispatchDesc.frameTimeDelta    = deltaTimeMs;
dispatchDesc.preExposure       = 1.0f;
dispatchDesc.cameraNear        = nearPlane;
dispatchDesc.cameraFar         = farPlane;
dispatchDesc.cameraFovAngleVertical = fovY;
dispatchDesc.viewSpaceToMetersFactor = 1.0f;
dispatchDesc.reset             = cameraReset;  // true on teleport / cut

ffxFsr3UpscalerContextDispatch(&upscalerCtx, &dispatchDesc);
```

#### 4. Destroy

```c
ffxFsr3UpscalerContextDestroy(&upscalerCtx);
free(scratch);
```

### Quality Modes

| Mode        | Enum                                              | Scale |
| ----------- | ------------------------------------------------- | ----- |
| Native AA   | `FFX_FSR3UPSCALER_QUALITY_MODE_NATIVEAA`          | 1.0×  |
| Quality     | `FFX_FSR3UPSCALER_QUALITY_MODE_QUALITY`           | 1.5×  |
| Balanced    | `FFX_FSR3UPSCALER_QUALITY_MODE_BALANCED`          | 1.7×  |
| Performance | `FFX_FSR3UPSCALER_QUALITY_MODE_PERFORMANCE`       | 2.0×  |
| Ultra Perf  | `FFX_FSR3UPSCALER_QUALITY_MODE_ULTRA_PERFORMANCE` | 3.0×  |

For Native AA, render and display resolution are the same — you still apply
jitter and the full dispatch pipeline.

### Init Flags

| Flag                                                         | When to set                                           |
| ------------------------------------------------------------ | ----------------------------------------------------- |
| `FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE`                 | Color buffer is HDR (linear, not tonemapped)          |
| `FFX_FSR3UPSCALER_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS`  | Motion vectors are at display resolution              |
| `FFX_FSR3UPSCALER_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION` | Motion vectors already include jitter                 |
| `FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED`                     | Depth buffer uses reverse-Z (1=near, 0=far)           |
| `FFX_FSR3UPSCALER_ENABLE_DEPTH_INFINITE`                     | Infinite far plane                                    |
| `FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE`                      | Let FSR compute exposure (no exposure texture needed) |
| `FFX_FSR3UPSCALER_ENABLE_DYNAMIC_RESOLUTION`                 | Render resolution changes per-frame                   |
| `FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING`                     | Enable runtime API validation                         |

### Reactive Mask (engine-side, `reactive.comp`)

The SDK's `ffx_fsr3upscaler_autogen_reactive_pass` /
`_luma_instability_pass` are compiled (see shader list above) but the engine
feeds the upscaler its **own** per-pixel reactive + T&C masks instead:
`shaders/pass/fsr/reactive.comp` (pipe `fsr_reactive`, dispatched from
`VulkanFsrPass.cpp` just before the FSR dispatch whenever the upscaler is
active). It writes two render-res `R32F` images:

- `FsrReactiveMask`, exposed via `vulkanFsrPassGetReactiveMaskImage()` and
  dumpable with `ENGINE_DEBUG_DUMP_IMAGES=reactive` (jpg + `reactiveRaw`);
- `FsrTCMask` (transparency & composition), no dump token.

Mask terms (max-combined, sky pixels write 0):

1. **Planar-reflection reactivity** — smooth surfaces near the water plane
   (`planeDist < 0.40`), weight ≤ 0.015.
2. **Specular / view-dependent** — roughness band 0.20–0.40 with a grazing
   boost, metallic bonus, weight ≤ 0.08.
3. **Composite-difference fallback** — for `roughness ≥ 0.25`, relative
   luminance difference between opaque scene color and composite color,
   `smoothstep(0.10, 0.30) × 0.25`. Catches effects that only exist in the
   composite (SSR, AO). Also feeds the T&C mask at `× 0.3`.
4. Alpha-cutout edge detection — intentionally disabled (comment in the
   shader; FSR's internal shading-change tracking handles these edges
   better).
5. **Terrain at grazing angles** — up-facing, non-metal, rough ≥ 0.25,
   weight ≤ 0.4. Dominates vegetated/terrain-heavy frames.

The DOF pass max-blends its CoC mask into `FsrReactiveMask` after the
dispatch (`vulkanDofPassApplyReactiveMask`), and
`vulkanFsrPassSetReactiveMask(0|1)` toggles the whole mechanism
(`reactiveMaskEnabled`, default 1 — no env/settings wiring exists today).

**GI interaction** (plans/ssgi.md Phase 4): screen-space GI changes the
opaque/composite ambient that term 3 measures. Validated empirically
(parked vantage, FSR Native AA, GI on vs off): GI *reduces* the mean mask
(0.094 vs 0.110 — the GI-driven CACAO attenuation shrinks the
opaque-vs-composite gap) but reshuffles term-3 knee crossings (0.5% of
pixels newly >0.1). Final-frame boiling under FSR is unchanged within
noise (inter-frame mean|diff| 1.96 vs 1.91 /255), i.e. the GI temporal
filter + `ENGINE_GI_TLUMA` luminance clamp keep the signal stable enough
that the mask reshuffling does not destabilize the upscaler. If a future
change trips term 3 visibly, lower `ENGINE_GI_TLUMA` before touching
`reactive.comp`.

## Build Script

`build.sh` — run from the `fsr3.1/` directory. Sources `../exports.sh` for
toolchain (clang, llvm-mingw, ccache). Requires Wine for shader compilation.

### Component registry

The script is component-table driven: each SDK component gets a
`comp_<name>_` registry block (blob-dispatch define, C++ sources, shader
list, SC base/permutation args, includes — copy from the component's
`gpu/<name>/CMakeCompile<NAME>Shaders.txt`), and is built iff listed in
`ENABLED_COMPONENTS`. Adding a component = patch the SDK fork if needed,
write its block, add the name to the list, drop any `ffx_stubs.cpp` stub
of its symbols. The blob accessor
`src/backends/shared/blob_accessors/ffx_<name>_shaderblobs.cpp` and the
`-DFFX_<NAME>` routing define are wired up automatically; a missing
registry field fails fast via `validate_component`.

Everything lands in the single `libffx_fsr3upscaler_vk.a` (name kept for
CMake stability) for both platforms.

### Steps

1. Compiles GLSL shaders → SPIR-V permutation headers for every enabled
   component (via Wine + FidelityFX_SC.exe, bounded parallel pool,
   `MAX_SHADER_JOBS=N` to override)
2. Compiles shared sources + per-component sources + blob accessors +
   stubs → `libffx_fsr3upscaler_vk.a` for Linux
3. Same for Windows (MinGW); per-object verification guards against
   silently-incomplete archives

### Re-run note

Re-running the build reorders permutation entries inside the generated
`*_permutations.h` tables (the SC tool emits whichever parallel variant
finished first). The hash-named SPIR-V payload headers are unchanged and
the key→blob mapping stays equivalent; expect these wrapper files to show
as modified after every rebuild.

### Volk integration

The build force-includes `volk.h` (via `-include` flag) before any other
header. This defines `VK_NO_PROTOTYPES` and provides `extern PFN_vk…`
declarations so the compiler generates **indirect calls** through volk's
function-pointer variables. Without this, the compiler would emit direct
`call` instructions to Vulkan symbols, but at link time those symbols resolve
to volk's global variables (data, not code), causing a segfault when the CPU
tries to execute raw pointer data as instructions.
