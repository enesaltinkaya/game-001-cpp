# SDSM (Sample Distribution Shadow Maps) Implementation Plan

## Goal

Replace the fixed logarithmic/linear cascade split scheme with depth-distribution-aware splits computed from the actual scene depth buffer, entirely on the GPU. This concentrates shadow map resolution where geometry actually exists.

## Current State

- **CSM**: 4 cascades (`SHADOW_CASCADE_COUNT`), 3072² each, `SHADOW_LAMBDA = 0.92` log/linear blend
- **Fixed splits**: `computeCascadeSplits()` in `VulkanShadowPass.c`
- **Shadow matrices**: computed on CPU in `computeCascadeMatrix()` using cglm, uploaded via `vulkanResourceUploadShadow()` which memcpys into persistently-mapped `VulkanSceneBuffer`
- **Depth prepass**: `VulkanDepthPass` produces depth image, accessed via `vulkanDepthPassGetDepthImage()`
- **Scene buffer**: `VulkanSceneBuffer` (persistently mapped) contains `ShadowUbo shadow` with `cascadeSplits[4]` and `shadowViewProjection[4]`
- **Pipeline order**: depth prepass → HiZ → geometry culling (calls `vulkanShadowPassPrepareShadowMatrices()`) → shadow pass → forward render
- **Compute precedent**: meshlet culling pass uses compute with push constants containing buffer addresses

## Architecture: Full GPU Pipeline

All three stages run as compute shaders. No CPU readback. Shadow matrices are computed on GPU and written directly into the scene buffer.

### Why GPU-only

- Eliminates 2-frame readback latency — splits are always current
- Scene buffer is already GPU-accessible (persistently mapped, but also has a device address)
- Matches the engine's GPU-driven philosophy (indirect draws, compute culling)
- The matrix math (ortho projection, lookat, frustum corners) is straightforward in GLSL

## Pipeline Order (Modified)

```
depth prepass → HiZ → [SDSM reduce] → [SDSM splits] → [SDSM matrices] → geometry culling → shadow pass → forward
```

The three SDSM dispatches replace the CPU-side `computeShadowMatrices()` call that currently happens in `vulkanShadowPassPrepareShadowMatrices()`.

---

## Phase 0: Fix Temporal Flag (Prerequisite)

The `taaEnabled` field in `ShadowUbo` was orphaned when TAA was removed in favor of FSR. The C side renamed it to `pad0` and never writes it, but the shader (`shadow.shader`, `globalset.shader`) still reads it as `taaEnabled` to gate temporal PCF jitter. Result: temporal shadow noise is **dead code** even when FSR is active — shadows are noisier than they should be.

**Fix:**

1. **`Light.h`**: Rename `pad0` back to `temporalActive` (or `temporalAccumulation`):
   ```c
   u32  temporalActive;  /* 1 when FSR or any temporal upscaler is active */
   ```

2. **`VulkanShadowPass.c`**: In `computeShadowMatrices()`, set based on upscaler state:
   ```c
   shadowUbo.temporalActive = rendererIsUpscalerEnabled() ? 1 : 0;
   ```

3. **`globalset.shader`**: Rename field:
   ```glsl
   uint temporalActive;  // 1 when temporal upscaler (FSR) is active
   ```

4. **`shadow.shader`**: Update the check:
   ```glsl
   if (sceneBuffer.shadow.temporalActive != 0u) {
   ```

This fixes existing shadow quality with FSR (temporal PCF jitter was silently disabled) and gives the SDSM shader the signal it needs for adaptive smoothing.

---

## Phase 1: Depth Min/Max Reduction

**New shader**: `shaders/pass/shadow/sdsm_reduce.comp`

Single compute dispatch over the depth buffer. Each workgroup (16×16 = 256 threads) processes a tile:
- Linearize reverse-Z depth to view-space distance
- Compute local min/max via shared memory reduction
- `atomicMin`/`atomicMax` on a global buffer (using `floatBitsToUint` trick for float atomics)

**Output buffer** (`SDSMBuffer`, new small SSBO):
```c
typedef struct SDSMBuffer {
    uint32_t minDepthBits;  // float as uint for atomics
    uint32_t maxDepthBits;
    uint32_t histogram[256];
    // --- Outputs from Phase 3 ---
    float    cascadeSplits[4];
    mat4     shadowViewProjection[4];
} SDSMBuffer;
```

Double-buffered per `FRAMES_IN_FLIGHT`. Cleared to `{UINT_MAX, 0, {0...}}` via `vkCmdFillBuffer` before dispatch.

**Push constants**: depth texture bindless index, zNear, zFar, resolution, scene buffer address.

## Phase 2: Depth Histogram

**New shader**: `shaders/pass/shadow/sdsm_histogram.comp`

Runs after a pipeline barrier on the min/max result. Each thread:
1. Reads global min/max from `SDSMBuffer`
2. Linearizes its depth sample
3. Computes bin index: `bin = clamp(int((depth - minDepth) / (maxDepth - minDepth) * 256.0), 0, 255)`
4. `atomicAdd(histogram[bin], 1)`

Can be merged into Phase 1 as a two-phase single shader (phase selected by push constant), or kept separate for clarity. **Recommendation: separate shaders** for debuggability, merge later if the barrier cost matters.

## Phase 3: Split Computation + Shadow Matrices

**New shader**: `shaders/pass/shadow/sdsm_matrices.comp`

Single workgroup dispatch (e.g., 4 threads, one per cascade). Each thread:

1. **Compute split** (thread 0 does prefix-sum over histogram):
   - Total samples = sum of histogram bins
   - Walk bins, accumulate, emit split distance when `accumulated >= totalSamples * (cascade+1) / CASCADE_COUNT`
   - Apply temporal smoothing: `split = mix(prevSplit, newSplit, smoothingFactor)`
   - Store previous splits in the SDSM buffer for next frame

2. **Compute cascade matrix** (all 4 threads, one per cascade):
   - Reconstruct 8 frustum corners from camera inverse VP (read camera from scene buffer)
   - Compute bounding sphere radius analytically (same formula as current CPU code)
   - Build light view matrix from sun direction (read from scene buffer)
   - Compute ortho projection with texel snapping
   - Write result to `SDSMBuffer.shadowViewProjection[cascadeIndex]`

3. **Write to scene buffer**: Copy splits and matrices into `VulkanSceneBuffer.shadow` via buffer device address (the address buffer already provides `sceneBufferAddress`).

**Push constants**: SDSM buffer address, scene buffer address, sun direction, camera index, smoothing factor.

### Temporal Smoothing Details

The SDSM buffer retains `prevCascadeSplits[4]` across frames (persisted in the double-buffered SSBO). The compute shader blends:

```glsl
// Adaptive smoothing: FSR/TAA accumulates temporal noise, so we can
// afford faster convergence.  Without temporal accumulation, split
// changes are immediately visible — use conservative smoothing.
bool temporalActive = (sceneBuffer.shadow.taaEnabled != 0u);
float smoothing  = temporalActive ? 0.15 : 0.05;
float quantStep  = temporalActive ? 0.5  : 1.0;  // world units
float moveThresh = temporalActive ? 0.005 : 0.015; // fraction of cascade range

float smoothedSplit = mix(prevSplit, newSplit, smoothing);

// Skip micro-updates to avoid shimmer
if (abs(smoothedSplit - prevSplit) < moveThresh * cascadeRange)
    smoothedSplit = prevSplit;

// Quantize to discrete steps for texel stability
smoothedSplit = round(smoothedSplit / quantStep) * quantStep;
```

The `taaEnabled` field in `ShadowUbo` is already set based on whether FSR or any temporal upscaler is active — the SDSM shader reads it from the scene buffer to adapt automatically.

---

## Phase 4: CPU-Side Changes

### `VulkanShadowPass.c`

- When SDSM is enabled, `vulkanShadowPassPrepareShadowMatrices()` **skips** `computeShadowMatrices()` entirely
- The SDSM compute passes have already written matrices and splits to the scene buffer
- CPU still uploads non-matrix shadow params (bias, normal bias, map size, PCSS light size, cascade count, bindless indices) — these are cheap and don't benefit from GPU computation
- Alternative: have the SDSM shader preserve these fields (they're already in the scene buffer from previous frame)

### `VulkanShadowPass.h`

```c
void vulkanShadowPassSetSDSM(char enabled);
char vulkanShadowPassIsSDSM(void);
```

### New System Registration

Register `vulkanSDSMPass` in the renderer pass list, ordered after depth/HiZ and before geometry culling.

---

## Phase 5: Integration

### Debug GUI (`DebugGui.c`)

- Checkbox: "SDSM" (on/off toggle, falls back to log/linear)
- Optional: visualize cascade splits as horizontal lines in a depth histogram overlay

### Settings (`Settings.c`)

- Persist `sdsmEnabled` boolean

### Shader Compilation

Add to shader build script:
- `sdsm_reduce.comp`
- `sdsm_histogram.comp`
- `sdsm_matrices.comp`

---

## File Changes Summary

| File | Change |
|------|--------|
| `c-engine/renderer/vulkan2/pass/shadow/VulkanShadowSDSM.h` | **New** — SDSM compute pass system header |
| `c-engine/renderer/vulkan2/pass/shadow/VulkanShadowSDSM.c` | **New** — Buffer creation, 3 compute dispatches, barriers |
| `c-engine/renderer/vulkan2/pass/shadow/VulkanShadowPass.h` | Add SDSM toggle API |
| `c-engine/renderer/vulkan2/pass/shadow/VulkanShadowPass.c` | Skip CPU matrix computation when SDSM active |
| `c-engine/data/.../shaders/pass/shadow/sdsm_reduce.comp` | **New** — Depth min/max reduction |
| `c-engine/data/.../shaders/pass/shadow/sdsm_histogram.comp` | **New** — Depth histogram |
| `c-engine/data/.../shaders/pass/shadow/sdsm_matrices.comp` | **New** — Split computation + cascade matrices |
| `c-engine/renderer/Renderer.c` | Register SDSM pass in pass list |
| `c-engine/renderer/gui/rmlui/guis/debugGui/DebugGui.c` | Add SDSM toggle |
| `c-utils/settings/Settings.c` | Persist SDSM state |
| `CMakeLists.txt` | Add new .c source files |
| Shader build script | Add 3 new .comp shaders |

## Synchronization

```
[vkCmdFillBuffer: clear SDSM buffer]
    ↓ (transfer → compute barrier)
[sdsm_reduce.comp: depth → min/max atomics]
    ↓ (compute → compute barrier on SDSM buffer)
[sdsm_histogram.comp: depth → histogram atomics]
    ↓ (compute → compute barrier on SDSM buffer)
[sdsm_matrices.comp: histogram → splits + matrices → scene buffer write]
    ↓ (compute → compute barrier on scene buffer)
[geometry culling: reads scene buffer shadow data]
```

## Risk Assessment

| Risk | Severity | Mitigation |
|------|----------|------------|
| Temporal instability / shadow shimmer | Medium | Exponential smoothing + split quantization in compute shader |
| GPU matrix math precision vs CPU cglm | Low | Use same formulas; float32 is sufficient for ortho shadow matrices |
| Complexity of matrix computation in GLSL | Medium | Port `computeCascadeMatrix()` faithfully; test with SDSM off/on comparison screenshots |
| Barrier overhead from 3 dispatches | Low | Tiny dispatches; can merge reduce+histogram into one shader later |
| Edge case: no geometry in depth buffer | Low | Fallback: if minDepth == maxDepth, use log/linear splits |
| Camera teleport → stale smoothed splits | Low | Detect large camera movement, reset smoothing (set blend factor to 1.0) |

## Estimated Effort

- Phase 1 (depth reduction shader + dispatch): 1 day
- Phase 2 (histogram shader): 0.5 day
- Phase 3 (split + matrix compute shader): 1.5 days
- Phase 4 (CPU integration, skip paths): 0.5 day
- Phase 5 (UI, settings, build): 0.5 day
- Testing, tuning, screenshot comparison: 1 day

**Total: ~5 days**
