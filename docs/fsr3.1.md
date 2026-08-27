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

| File                                                                      | Purpose                                                                                                                                |
| ------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------- |
| `src/components/fsr3upscaler/ffx_fsr3upscaler.cpp`                        | FSR3 upscaler core logic                                                                                                               |
| `src/components/cacao/ffx_cacao.cpp`                                      | CACAO AO core logic (`ffxCacaoContextCreate/Dispatch/Destroy`, `ffxCacaoUpdateSettings`)                                               |
| `src/components/dof/ffx_dof.cpp`                                          | DOF core logic (`ffxDofContextCreate/Dispatch/Destroy`, `ffxDofCalculateCoc{Scale,Bias}`)                                              |
| `src/components/lpm/ffx_lpm.cpp`                                          | LPM core logic (`ffxLpmContextCreate/Dispatch/Destroy`, `FfxPopulateLpmConsts`)                                                       |
| `src/components/brixelizer/ffx_brixelizer.cpp` + `ffx_brixelizer_raw.cpp` | Brixelizer SDF voxelizer (high-level + raw contexts: `ffxBrixelizer{ContextCreate,BakeUpdate,Update,CreateInstances,DeleteInstances}`) |
| `src/components/brixelizergi/ffx_brixelizergi.cpp`                        | Brixelizer GI (`ffxBrixelizerGIContext{Create,Dispatch,Destroy,DebugVisualization}`)                                                   |
| `src/shared/ffx_assert.cpp`                                               | Assert/debug utilities                                                                                                                 |
| `src/shared/ffx_message.cpp`                                              | Message/logging utilities                                                                                                              |
| `src/shared/ffx_object_management.cpp`                                    | Internal object management                                                                                                             |
| `src/backends/vk/ffx_vk.cpp`                                              | Vulkan backend implementation                                                                                                          |
| `src/backends/shared/ffx_shader_blobs.cpp`                                | Shader blob dispatch (routes by `FFX_FSR3UPSCALER` / `FFX_CACAO` defines)                                                              |
| `src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp`     | Precompiled SPIR-V shader permutations (FSR3)                                                                                          |
| `src/backends/shared/blob_accessors/ffx_cacao_shaderblobs.cpp`            | Precompiled SPIR-V shader permutations (CACAO)                                                                                         |
| `src/backends/shared/blob_accessors/ffx_dof_shaderblobs.cpp`              | Precompiled SPIR-V shader permutations (DOF)                                                                                           |
| `src/backends/shared/blob_accessors/ffx_lpm_shaderblobs.cpp`              | Precompiled SPIR-V shader permutations (LPM)                                                                                           |
| `src/backends/shared/blob_accessors/ffx_brixelizer_shaderblobs.cpp`       | Precompiled SPIR-V shader permutations (Brixelizer)                                                                                    |
| `src/backends/shared/blob_accessors/ffx_brixelizergi_shaderblobs.cpp`     | Precompiled SPIR-V shader permutations (Brixelizer GI)                                                                                 |
| `../../ffx_stubs.cpp`                                                     | Stubs for unbuilt components (breadcrumbs, frame interpolation swapchain)                                                              |

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

Brixelizer: 31 shaders, no permutation axes → 31 × 4 variants = 124
permutation headers (18 cascade-ops, 10 context-ops, 3 debug).

Brixelizer GI: 19 shaders × 8 permutations (DEPTH_INVERTED ×
DISABLE_SPECULAR × DISABLE_DENOISER) × 4 variants = 608 permutation
headers — the slowest pool in the build on both platforms.

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

Brixelizer shaders (31): 18 `ffx_brixelizer_cascade_ops_*` passes (build
AABB tree, coarse culling, voxelize, E… job/reference scanning + counters,
cascade scroll/reset/invalidate), 10 `ffx_brixelizer_context_ops_*` passes
(brick clear/merge, Eikonal, merge cascades, args preparation) and 3 debug
passes (`debug_draw_aabb_tree`, `debug_draw_instance_aabbs`,
`debug_visualization_pass`). No permutation axes.

Brixelizer GI shaders (19): `ffx_brixelizergi_{blur_x, blur_y, clear_cache,
debug_visualization, downsample, emit_irradiance_cache,
emit_primary_ray_radiance, fill_screen_probes, generate_disocclusion_mask,
interpolate_screen_probes, prepare_clear_cache, project_screen_probes,
propagate_sh, reproject_gi, reproject_screen_probes, spawn_screen_probes,
specular_pre_trace, specular_trace, upsample}`.

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
- **All other shader blob accessors** — only the accessors of the enabled components (`ffx_fsr3upscaler`, `ffx_cacao`, `ffx_spd`, `ffx_lens`, `ffx_dof`, `ffx_lpm`, `ffx_brixelizer`, `ffx_brixelizergi`) are compiled

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
- Added `#include <bit>` (Brixelizer 6.4 win round): `std::popcount` lives in
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

### `sdk/src/components/brixelizergi/ffx_brixelizergi_private.h` + `sdk/include/FidelityFX/host/ffx_brixelizergi.h` (Brixelizer GI round, 6.3)

- **Upstream bug:** `FfxBrixelizerGIContext_Private.constantBuffers` was
  declared `[3]` while four constant-buffer identifiers exist
  (`FFX_BRIXELIZER_GI_CONSTANTBUFFER_IDENTIFIER_GI_CONSTANTS / PASS_CONSTANTS
/ SCALING_CONSTANTS / CONTEXT_INFO` = 0..3). `updateConstantBuffer(...
, CONTEXT_INFO, ...)` staged into `constantBuffers[3]` — one
  `FfxConstantBuffer` (16 B) **past the end of the private struct**, i.e.
  past the host's `FfxBrixelizerGIContext` allocation. In the engine this
  silently corrupted `.bss` statics of the consumer pass every frame
  (caught with a gdb watchpoint inside `StageConstantBufferDataVK`).
- Fork patch: `constantBuffers[3]` → `[4]`, and
  `FFX_BRIXELIZER_GI_CONTEXT_SIZE` 349680 → **349684** uint32s on
  non-Windows (the +16 B slot).

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

### `sdk/include/FidelityFX/host/ffx_brixelizer.h` + `ffx_brixelizer_raw.h` (Brixelizer round)

Linux `wchar_t` inflation (4 vs 2 bytes), same `#if defined(_WIN32)`
pattern as the CACAO/SPD/DOF bumps. Four constants:

- `FFX_BRIXELIZER_CONTEXT_SIZE` 5938838 → 6196886 uint32s (24787544 B):
  the high-level context embeds the raw context + the baked update
  description, so it absorbs both bumps below.
- `FFX_BRIXELIZER_RAW_CONTEXT_SIZE` 2924058 → 3182106 uint32s (12728424 B):
  the raw context's 24 cascade × FfxPipelineState name arrays.
- `FFX_BRIXELIZER_UPDATE_DESCRIPTION_SIZE` 2099376 → 2100976 uint32s
  (8403904 B): the baked update description's per-instance name arrays.
- `FFX_BRIXELIZER_GI_CONTEXT_SIZE` 210000 → 349680 uint32s (1398720 B,
  `ffx_brixelizergi.h`): 19 GI FfxPipelineState members.

The SDK's `FFX_STATIC_ASSERT`s in `ffxBrixelizerContextCreate`,
`ffxBrixelizerRawContextCreate`, `ffxBrixelizerBakeUpdate`'s description
check, and `ffxBrixelizerGIContextCreate` catch regressions at compile
time.

### `sdk/src/components/brixelizer/ffx_brixelizer_raw.cpp` + `ffx_brixelizergi.cpp` (Brixelizer round)

- Explicit `static_cast<uint32_t>` on the buffer-size fields of the
  internal-resource initializer lists (6 × `context->totalBricks *
sizeof(uint32_t)`, 5 × `width * height * sizeof(FfxUInt32x{2,4}...)`) —
  the strict `-Wc++11-narrowing` build rejects the implicit `size_t` →
  `uint32_t` narrowing in brace-initializers (SSSR precedent).

No GLSL patches: the GI internal UAV qualifiers already match the host
formats (probe images rgba16f, radiance cache r11f_g11f_b10f), and the
engine-provided GI outputs are rgba16f UAVs — matching the engine's
R16G16B16A16_SFLOAT HDR convention out of the box.

### `sdk/include/FidelityFX/gpu/brixelizer/ffx_brixelizer_cascade_ops.h` (Brixelizer 6.2 round)

- **Per-voxel reference clamp** (`FFX_BRIXELIZER_MAX_REFS_PER_VOXEL 128`):
  over-quota reference increments roll back instead of `MarkFailed`-ing the
  voxel. Without it, dense scenes (87k–225k-tri authored tree variants) blew
  the reference/triangle budgets on ~15k voxels per bake, and
  `FfxBrixelizerScanReferences` refuses to heal or allocate failed voxels —
  the brick map wedged with permanent UNINIT holes.
- **2D→3D distance-cull port**: the 2D voxelize path's "drop refs > 2 voxels
  from the triangle" cull applied to the 3D voxelize branch (120M → 532k
  references on the pathological scene).
- Together with the engine-side GI-LOD swap (voxelize simplified `_far`
  geometry for near trees), the bake triangle sum dropped 29.3M → 1.5M.
  Defensive depth for future dense scenes.

### `sdk/src/backends/vk/ffx_vk.cpp` (Brixelizer round)

- `bindlessDescriptorPool` created with `flags = 0`, but the backend
  destroys its descriptor sets with `vkFreeDescriptorSets` on context
  teardown — legal only on pools created with
  `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT`. Any brixelizer
  context destroy (including the self-test's per-frame churn of the
  FFX-internal views) hit a validation CRIT. Universal fix, no platform
  guard. Both FFX archives (Linux + Windows) were rebuilt after this
  patch.

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

## Brixelizer GI sample cross-build (Wine reference)

`build-brixgi-sample.sh` (in `fsr3.1/`) cross-compiles the Cauldron-based
Brixelizer GI sample (`git/samples/brixelizergi`) for Windows x64 + Vulkan
with llvm-mingw, links it against `sdk/build-win/libffx_fsr3upscaler_vk.a`
(run `./build.sh` first), and assembles a runnable tree in `git/bin/`
(exe + `dxcompiler.dll`/`dxil.dll` + `configs/` + `shaders/`). Run it with
Wine against the host's radeon ICD:

```bash
cd /home/enes/Projects/c/cpp-thirdparty/fsr3.1 && ./build-brixgi-sample.sh
cd git/bin && VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json \
  wine FFX_BrixelizerGI_VK.exe
```

Scene/IBL/noise media lives in `git/media/` (fetched via
`sdk/tools/media_delivery/MediaDelivery.exe`, see the brixelizer-gi plan).

### Test hooks (fork additions in `brixelizergirendermodule.cpp`)

- `BRIXGI_OUTPUT_MODE` — force the Output Mode: `diffusegi` / `speculargi` /
  `radiance` / `irradiance` / `debugvis` / `example` / `none` (otherwise the
  ImGui combo drives it).
- `BRIXGI_EXIT_FRAMES=N` — `PostQuitMessage` after N frames so the run is
  headless; with a screenshot-enabled config the framework's `PostRun` dumps
  the last swapchain frame to `git/bin/screenshots/`.
- FFX messages (`ffxSetPrintMessageCallback`) are mirrored to stdout as
  `[FFX ERROR|WARNING|INFO] …` — a run is "FFX-clean" when the log has no
  ERROR/WARNING lines.

### Test hooks for engine A/B comparisons (2026-08-27, ghost-cube hunt)

Added while hunting the engine's Step-2 "two SDF regions / 27→2 brick
collapse" artifact; they reproduce the engine's voxelizer conditions inside
the reference sample:

- `BRIXGI_STATIC_ONLY=1` — cascade layout parity with the engine's Step 1:
  8 STATIC-only cascades (raw cascade == level), base voxel 2 m doubling per
  level, trace range moved down to cascades 0..7.
- `BRIXGI_SINGLE_CUBE=<frame>` — at frame N, drop every instance and submit
  exactly one static instance (`maxCascade = 0`) 10 m ahead of the SDF center
  — the sparse-object regime of the engine's smoke test.
- `BRIXGI_CUBE_MIN_EXTENT=<m>` / `BRIXGI_CUBE_SOLID=1` /
  `BRIXGI_CUBE_SCALE=<m>` — object picker filters: minimum largest extent,
  require extent on every axis (planar decals legitimately free their
  adjacent voxels and hide solid-object failure modes), and per-axis scale to
  a cube-like target size (the toyshop's meshes are sub-voxel at 2 m).
- `BRIXGI_CUBE_FRESH=1` — destroy + recreate both FFX contexts before the
  single-instance submission (pristine voxelizer state, no deletion
  invalidations).
- `BRIXGI_SDF_CENTER=x,y,z` — pin the clipmap center to a fixed world
  position instead of the camera's (engine-parity large/negative coords).
  NOTE: this also moves the grid away from the toyshop (world ~0..20 m), so a
  pinned run has NO scene geometry — only the single instance.
- `BRIXGI_LOG_STATS=<stride>` — print the lagged `FfxBrixelizerStats` every
  N frames (`free=… sTris=… sBricks=…`); `freeBricks` is the collapse signal.
- `BRIXGI_DIAG_PATH` — where to write the `FFX_BRIX_DIAG` fork dump.

Result of the A/B that found the bug: with a SOLID 4 m object at the engine's
exact coordinates, the reference sample bakes ~24-34 bricks and keeps them
(retention ~100%), while the engine kept 2 of 27 — proving the failure was in
the engine integration, not FFX: the row-major instance transform had its
identity diagonal at `[0]/[4]/[8]` instead of `[0]/[5]/[10]`, projecting the
cube onto its main diagonal (degenerate line-segment triangles; `CompressBrick`
freed every brick without near-surface samples, leaving the diagonal's two
endpoint voxels — the "two regions").

The FFX fork's `FFX_BRIX_DIAG` dump was also extended (2026-08-27):
`FFX_BRIX_DIAG_AT=<frame>` + `FFX_BRIX_DIAG_COUNT=<n>` capture up to 4
consecutive cascade-0 bakes (each region: 40 B counters, 4 KB compression
list, and the 1 MB cascade-0 brick map) instead of one first-bake snapshot;
scratch source offsets are computed per bake via `getScratchMemorySize`
(the job-counter partitions scale with `numJobs + numInstances`); and the
capture state resets in `ffxBrixelizerRawContextCreate` so a recreated
context can be captured cleanly.

`enable-brixgi-screenshot.sh` (in `fsr3.1/`) flips
`configs/brixelizergiconfig.json` → `"FidelityFX Brixelizer GI" →
"Screenshot": true` (the sample loads that file, NOT cauldronconfig.json;
re-run it after every build — the build re-copies stock configs). Reference
captures (2558×1413, RADV, 1200 frames each, 2026-08-26) are in
`git/bin/screenshots/ref-{diffusegi,speculargi,radiance,irradiance,debugvis}.jpg`.

### Fork patches the sample build relies on (clang-on-MinGW, in `git/framework/…`)

MSVC-tolerant idioms that clang rejects — all patched with `[clang patch]`
comments in-tree:

- **Volk dispatch for the whole sample** — same root cause as the library
  build (Volk integration above): all sources compile with
  `VK_NO_PROTOTYPES` + force-included `volk.h` (+ `VK_USE_PLATFORM_WIN32_KHR`
  for the Win32 surface entry points, which volk only generates under that
  define), and the link **drops `vulkan-1.lib`** — every `vk*` symbol
  resolves to a volk function-pointer variable, and `volkInitialize()` /
  `volkLoadInstance()` / `volkLoadDevice()` (hooks in `device_vk.cpp`)
  populate them. `VMA_STATIC_VULKAN_FUNCTIONS=1` is also defined so
  vk_mem_alloc copies the volk variables instead of its dynamic-import path
  (which would dereference unpopulated proc-addr members).
- `volkInitialize()` runs **before** the `InstanceCreator` ctor in
  `device_vk.cpp` (its ctor calls volk-dispatched `vkEnumerate*`).
- Copy-queue fallback: RADV exposes no dedicated transfer-only family
  (family 0 is GRAPHICS|COMPUTE|TRANSFER, family 1 COMPUTE|TRANSFER); the
  copy-queue search falls back to any family with the transfer flag.
- `RenderModuleInfo::InitOptions` is null for name-only config entries and
  the modules call `.value()` on it (nlohmann throws `type_error.306`);
  the framework's init loop now hands them an empty object.
- MSVC-isms fixed in-tree: `L#shift` wide-string stringification macro and
  `x##/` include-path pasting (sample.cpp), `##` in `CHECK_FEATURE_SUPPORT`
  feature macros, `UISlider` out-of-class virtual specializations needing
  `template<>`, `AddShaderDesc`/`AddTask` by value (temp bindings),
  address-of-temporary `GetResourceView()`/`Barrier::Transition` call
  sites, `auto&` iterator temporaries in for-loops, non-const
  `operator float()` used by `std::sort`, `__uuidof` for the DXC COM
  interfaces (generated `dxc_uuids` shim in `git/build/brixgi-compat/`),
  missing `<share.h>`/`<cfloat>`/`<experimental/filesystem>` includes,
  `_WIN32_WINNT=0x0A00` (DPI + shellscaling APIs), a compat `Xinput.h`
  (MinGW lacks it; fields are `BYTE bLeftTrigger/bRightTrigger`, not WORD),
  and a lowercase include for `lightingrendermodule.h` (case-sensitive fs).
- The FFX lib build has no frame-interpolation component, so the five
  `ffx*Frameinterpolation*VK` entry points the VK backend references at
  static-init time are stubbed in
  `samples/brixelizergi/ffx_frameinterpolation_stubs.cpp` (they fail loudly
  if ever called; the GI sample never enables FG).

The build is incremental (`src -nt obj`); changes to the script's defines
need a `rm -rf git/build/brixgi-win` first.
