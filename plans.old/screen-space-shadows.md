# Screen Space Shadows (Contact Shadows) — Implementation Plan

## Overview

Re-add screen-space shadows (SSS) to the engine as a compute pass that reads the depth buffer and writes a single-channel shadow texture. The forward pass shaders (meshlet.frag, triangle.frag) will sample this texture to multiply with the existing shadow map result, adding fine contact shadow detail.

**Approach chosen:** h3r2tic-style raymarching (simple single-dispatch compute shader). The Bend Studio wavefront approach is more performant at high sample counts but far more complex (CPU dispatch list builder, 1D wavefront mapping, LDS). The raymarching approach is simpler, proven, and can be upgraded later.

**All shaders will be GLSL** (`.comp` / `.frag`), compiled by `glslc` via the existing `shaders.sh` build system.

---

## Architecture

```
Depth Prepass → [depthImage] → SSS Compute Pass → [sssImage] → Forward Pass (sample sssImage)
                                                         ↓
                                              (optional) Blur Pass
```

The SSS pass runs **after** the depth prepass and **before** the forward rendering passes (meshlet render, triangle render). It reads the depth buffer and the light direction, and outputs a single-channel R8_UNORM texture where 1.0 = fully lit, 0.0 = fully shadowed.

---

## Files to Create

### 1. `src/renderer/vulkan2/pass/sss/VulkanSSSPass.h`

```c
#pragma once
#include "ecs/system/System.h"

extern System vulkanSSSPass;
```

Minimal header. Exposes the System so `Vulkan.c` can register it as a render pass.

---

### 2. `src/renderer/vulkan2/pass/sss/VulkanSSSPass.c`

The CPU-side pass implementation. Follows the exact same pattern as `VulkanShadowPass.c`, `VulkanDepthPass.c`, etc.

#### Responsibilities:
- Create/destroy the SSS output image (`sssImage`, R8_UNORM, STORAGE|SAMPLED)
- Create/destroy an optional blur temp image (`sssTempImage`, R8_UNORM, STORAGE|SAMPLED)
- Create the compute pipeline from the compiled `.comp.spv`
- Each frame: bind pipeline, push constants, dispatch, transition layouts
- Optionally run a blur pass (reuse existing `vulkanBlur()` from `VulkanBlur.c`)
- Expose `vulkanSSSPassGetSSSImage()` so forward shaders can read the pool index

#### Lifecycle:

| Callback     | Work                                                                 |
|-------------|----------------------------------------------------------------------|
| `added()`   | Create compute pipeline. Subscribe to swapchain resize events.       |
| `preUpdate()` | Reset GPU profile. Recreate images if window size changed.         |
| `update()`  | Transition depth → SHADER_READ. Transition sssImage → GENERAL. Bind pipeline, push constants, dispatch compute. Transition sssImage → SHADER_READ. Optional blur pass. |
| `postUpdate()` | Report timing to `vulkanSSSPass.gpuElapsed`.                     |
| `removed()` | Destroy pipeline, images.                                            |

#### Push Constants struct:

```c
typedef struct SSSPushConstants {
    vec4 lightDirViewSpace;     // xyz: light direction in VIEW space, w: sign flag
    vec2 invDepthTextureSize;   // 1.0/width, 1.0/height
    float surfaceThickness;     // ~0.005 (tunable)
    float maxRayLength;         // max ray length in UV space (~0.05-0.1)
    u32 stepCount;              // number of linear march steps (16-32)
    float jitter;               // temporal jitter offset (0-1), from frame counter
    int depthImageIndex;        // sampledPoolIndex of depthImage
    int sssImageIndex;          // storagePoolIndex of sssImage
} SSSPushConstants;
```

#### Key implementation details:

- **Light direction**: Get from `sceneBuffer.directionalLight.direction`. Transform to view space by multiplying with the camera view matrix. Pass as push constant.
- **Jitter**: Use `interleavedGradientNoise()` keyed on frame counter for temporal stability. Can start with a simple constant (0.5) and add proper jitter later.
- **Dispatch**: Standard `(width+7)/8, (height+7)/8, 1` for an 8×8 workgroup.
- **Image creation**: Only create when `!sssImage.img` or window size changed. Use `vulkanCreateImage(.format = VULKAN_R8_UNORM, .usage = VULKAN_IMG_STORAGE | VULKAN_IMG_SAMPLED, ...)`.
- **Pool registration**: Images are auto-added to the global descriptor pool via `vulkanCreateImage` (no `noPool` flag), giving us `sampledPoolIndex` and `storagePoolIndex`.

#### Expose for forward shaders:

```c
VulkanImage* vulkanSSSPassGetSSSImage(void);  // returns NULL if not ready
```

The forward pass will read `vulkanSSSPassGetSSSImage()->sampledPoolIndex` to get the texture index to pass as a push constant or read from the scene buffer.

---

### 3. `data/pak_0_engine/shaders/pass/sss/sss.comp` — GLSL Compute Shader

The core raymarching shader. **Pure GLSL**, no HLSL.

#### Design:

```glsl
#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_ARB_shading_language_include : enable

#include "../../includes/globalset.shader"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform PushConstants {
    vec4 lightDirViewSpace;     // xyz: view-space light dir, w: unused
    vec2 invDepthTextureSize;
    float surfaceThickness;
    float maxRayLength;
    uint stepCount;
    float jitter;
    int depthImageIndex;
    int sssImageIndex;
} pc;
```

#### Algorithm (per pixel):

1. **Read depth** at current pixel from `depthImageIndex` using `SAMPLER_NEAREST`.
2. **Skip sky pixels**: if depth ≈ 0.0 (reverse-Z far plane), write 1.0 and return.
3. **Reconstruct view-space position** from depth + UV + inverse projection matrix.
4. **Compute ray direction** in screen space:
   - Start position: current pixel UV + depth.
   - End position: offset the view-space position along the light direction, then project back to screen space. The difference gives the screen-space ray direction.
5. **Linear march** along the ray in screen space (UV + depth interpolation):
   - For each step, sample depth buffer at the interpolated UV.
   - Compare interpolated ray depth with sampled scene depth.
   - If the ray goes behind a surface (ray depth > scene depth) and the penetration is less than `surfaceThickness`, record a hit.
6. **Accumulate shadow**: On hit, compute shadow strength based on penetration fraction. Use `smoothstep` for soft falloff.
7. **Write result** to `sssImageIndex` via `storageImages[]`.

#### Key shader functions:

```glsl
// Reconstruct view-space position from depth + UV
vec3 viewPosFromDepth(vec2 uv, float depth) {
    Camera cam = sceneBuffer.cameras[0];
    vec2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y; // Vulkan
    vec4 clipPos = vec4(ndc, depth, 1.0);
    vec4 viewPos = cam.invProjection * clipPos;
    return viewPos.xyz / viewPos.w;
}

// Interleaved gradient noise for jitter
float interleavedGradientNoise(vec2 px) {
    return fract(52.9829189 * fract(0.06711056 * px.x + 0.00583715 * px.y));
}
```

#### Why view-space raymarching?

Screen-space raymarching (march in UV + depth) is efficient but the depth comparison needs care with reverse-Z. By reconstructing view-space positions and projecting back to screen space, we get correct depth interpolation. The march itself still happens in screen-space UV coordinates for cache efficiency.

---

## Files to Modify

### 4. `src/renderer/vulkan2/Vulkan.c` — Register the pass

Add the SSS pass between the depth pass and the meshlet/triangle render passes.

```c
// Add include
#include "renderer/vulkan2/pass/sss/VulkanSSSPass.h"

// In vulkanInit(), add after vulkanDepthPass:
addPass(&vulkanSSSPass);     // <-- NEW: screen-space shadows
```

Current pass order:
```
gridPass → geometryCullingPass → shadowPass → depthPass → meshletRenderPass → triangleRenderPass → hiZPass → rmluiPass
```

New order:
```
gridPass → geometryCullingPass → shadowPass → depthPass → vulkanSSSPass → meshletRenderPass → triangleRenderPass → hiZPass → rmluiPass
```

The SSS pass reads from `depthImage` (written by depthPass) and writes `sssImage`. The forward passes then read `sssImage`.

---

### 5. Forward Shaders — Consume the SSS texture

#### `data/pak_0_engine/shaders/pass/meshlet/meshlet.frag`

Add the SSS image index. Two options:

**Option A: Via push constants** — Add `int sssImageIndex` to the meshlet push constants. This requires modifying both the C struct and the shader. Clean but touches more files.

**Option B: Via SceneBuffer** — Add `uint sssImageIndex` to the `ShadowData` struct in `globalset.shader` and upload it from C each frame. This is the cleanest approach because all forward shaders automatically get access without modifying their individual push constants.

**Recommended: Option B.**

#### Changes to `data/pak_0_engine/shaders/includes/globalset.shader`:

Add to the `ShadowData` struct:
```glsl
struct ShadowData {
    // ... existing fields ...
    uint sssImageIndex;     // NEW: pool index for screen-space shadow texture
    uint pad[2];            // adjust padding (was pad[3])
};
```

#### Changes to `src/ecs/system/light/Light.h` (C-side ShadowUbo):

```c
typedef struct ShadowUbo {
    // ... existing fields ...
    u32 sssImageIndex;          // NEW
    u32 pad[2];                 // adjust padding (was pad[3])
} ShadowUbo;
```

#### Changes to `data/pak_0_engine/shaders/includes/shadow.shader`:

Add to `sampleShadow()`:
```glsl
float sampleShadow(vec3 worldPos, vec3 normal) {
    // ... existing cascade shadow code ...
    float shadow = /* existing cascade result */;

    // Multiply with screen-space shadow (contact shadows)
    ShadowData sd = sceneBuffer.shadow;
    if (sd.sssImageIndex != 0u) {
        Camera cam = sceneBuffer.cameras[0];
        vec4 clipPos = cam.viewProjection * vec4(worldPos, 1.0);
        vec2 screenUV = clipPos.xy / clipPos.w * vec2(0.5, -0.5) + 0.5;
        float sss = texture(
            sampler2D(textures[nonuniformEXT(sd.sssImageIndex)],
                      samplers[SAMPLER_LINEAR]),
            screenUV
        ).r;
        shadow = min(shadow, sss);
    }

    return shadow;
}
```

Using `min(shadow, sss)` ensures screen-space shadows only add MORE shadow, never remove the cascade shadow map's contribution.

---

### 6. `src/renderer/vulkan2/pass/sss/VulkanSSSPass.c` — Upload SSS index

After transitioning the sssImage to SHADER_READ, update the shadow UBO with the pool index:

```c
// In the SSS pass postUpdate or at end of update:
// This requires access to the shadowUbo. Two approaches:
//   a) Have VulkanShadowPass expose a setter function
//   b) Upload the sssImageIndex separately via VulkanResourceManager

// Simplest: modify VulkanShadowPass to accept an SSS index
vulkanShadowPassSetSSSIndex(sssImage.sampledPoolIndex);
```

OR — even simpler — have the SSS pass directly write `sceneBuffer.shadow.sssImageIndex` via the resource manager. Looking at the existing code, `vulkanResourceUploadShadow()` uploads the entire `ShadowUbo`. The SSS pass should set `shadowUbo.sssImageIndex` before the shadow upload happens, or do a separate upload.

**Cleanest approach**: Have `VulkanSSSPass` expose `vulkanSSSPassGetPoolIndex()` and have `VulkanShadowPass.c` call it during `preUpdate()` when filling the `ShadowUbo`:

```c
// In VulkanShadowPass.c preUpdate():
VulkanImage* sssImg = vulkanSSSPassGetSSSImage();
shadowUbo.sssImageIndex = sssImg ? sssImg->sampledPoolIndex : 0;
```

This keeps data flow clean: shadow pass owns the UBO upload, SSS pass just provides its image.

---

## Build Integration

### Shader compilation

The existing `shaders.sh` script auto-discovers `.comp` files under `data/pak_*/shaders/` and compiles them with `glslc`. No changes needed — just placing `sss.comp` in the right directory is sufficient.

Output SPV path: `data/pak_0_engine/shaders/pass/sss/spv/sss.comp.spv.debug` / `.release`

### C compilation

The new `.c` file needs to be picked up by CMake. Check CMakeLists.txt — if it uses `file(GLOB_RECURSE ...)` on `src/`, it's automatic. If not, add the file.

---

## Implementation Steps (Ordered)

### Phase 1: Infrastructure (no visual change yet)

1. **Create directory**: `src/renderer/vulkan2/pass/sss/`
2. **Create `VulkanSSSPass.h`**: Declare `extern System vulkanSSSPass` and `vulkanSSSPassGetSSSImage()`.
3. **Create `VulkanSSSPass.c`**: Stub implementation:
   - `added()`: Create compute pipeline (pointing to the shader SPV path), create sssImage.
   - `preUpdate()`: Reset profile. Handle resize.
   - `update()`: Transition depth → SHADER_READ, sssImage → GENERAL. Dispatch compute. Transition sssImage → SHADER_READ.
   - `postUpdate()`: Report GPU timing.
   - `removed()`: Cleanup.
4. **Create shader directory**: `data/pak_0_engine/shaders/pass/sss/`
5. **Create `sss.comp`**: Minimal GLSL compute shader that just writes 1.0 (fully lit) everywhere. Validates the pipeline works.
6. **Register pass** in `Vulkan.c`: Add `#include` and `addPass(&vulkanSSSPass)` after `vulkanDepthPass`.
7. **Build and verify**: `scripts/build.sh`. Confirm no crashes, no visual change (all white = no shadows).

### Phase 2: Raymarching shader

8. **Implement `sss.comp`** with the full raymarching algorithm:
   - Read depth, reconstruct view-space position.
   - March toward light in screen space.
   - Depth comparison with thickness test.
   - Write shadow result.
9. **Tune push constants** in `VulkanSSSPass.c`:
   - Light direction from directional light, transformed to view space.
   - Surface thickness, step count, max ray length.
10. **Build and verify**: The sssImage should now contain contact shadow data. Use RenderDoc to inspect.

### Phase 3: Integration with forward shaders

11. **Modify `ShadowUbo`** in `Light.h`: Add `sssImageIndex`, adjust padding.
12. **Modify `ShadowData`** in `globalset.shader`: Mirror the C struct change.
13. **Modify `VulkanShadowPass.c`**: In `preUpdate()`, set `shadowUbo.sssImageIndex` from `vulkanSSSPassGetSSSImage()->sampledPoolIndex`.
14. **Modify `shadow.shader`**: In `sampleShadow()`, sample the SSS texture and combine with cascade shadow via `min()`.
15. **Build and verify**: Contact shadows should now appear in the final render.

### Phase 4: Polish (optional)

16. **Add blur pass**: Use the existing `vulkanBlur()` utility to soften the SSS output. Create a temp image for the ping-pong blur. This softens single-pixel noise.
17. **Temporal jitter**: Use interleaved gradient noise keyed on frame index for per-pixel jitter. Combined with TAA (if present), this gives smooth temporal results.
18. **Tuning**: Expose `surfaceThickness`, `stepCount`, `maxRayLength` as settings (via the Settings system) for runtime tweaking.
19. **Depth bias pass** (optional, from Bend Studio's technique): A pre-pass that adds material displacement to the depth buffer for enhanced micro-shadow detail. This is a separate compute shader and a separate depth image. Skip for initial implementation.

---

## Tuning Parameters

| Parameter          | Starting Value | Description                                    |
|-------------------|---------------|------------------------------------------------|
| `surfaceThickness` | 0.005         | Assumed pixel thickness (fraction of depth range). Too large = thick blobs. Too small = no shadows. |
| `maxRayLength`     | 0.1           | Max ray length in UV space. Controls shadow reach distance. |
| `stepCount`        | 16            | Linear march steps. More = better quality, more cost. 16-32 recommended. |
| `jitter`           | 0.5           | Start fixed, then switch to IGN for temporal stability. |

---

## Performance Estimate

- **Single compute dispatch**: `(1920/8) × (1080/8) = 240 × 135 = 32,400 workgroups`
- **Per-thread**: ~16 depth texture reads (one per step) + arithmetic
- **Expected cost**: ~0.2-0.5ms at 1080p on mid-range GPU
- **Optional blur**: ~0.1ms additional

---

## Risk Mitigation

| Risk | Mitigation |
|------|-----------|
| Depth comparison issues with reverse-Z | The engine uses reverse-Z (near=1, far=0). The shader must account for this: "behind surface" = ray depth < scene depth in reverse-Z. Test early with debug output. |
| SSS image not ready on first frame | `vulkanSSSPassGetSSSImage()` returns NULL. Shadow pass checks for NULL and sets `sssImageIndex = 0`. Shader checks `sssImageIndex != 0` before sampling. |
| Window resize race condition | Follow the same resize pattern as `VulkanDepthPass.c`: check extent in `preUpdate()`, destroy + recreate if changed. |
| Push constant size limit | Current struct is ~48 bytes. Vulkan guarantees at least 128 bytes. No issue. |

---

## File Summary

| File | Action |
|------|--------|
| `src/renderer/vulkan2/pass/sss/VulkanSSSPass.h` | **CREATE** |
| `src/renderer/vulkan2/pass/sss/VulkanSSSPass.c` | **CREATE** |
| `data/pak_0_engine/shaders/pass/sss/sss.comp` | **CREATE** |
| `src/renderer/vulkan2/Vulkan.c` | **MODIFY** — add include + addPass |
| `src/ecs/system/light/Light.h` | **MODIFY** — add sssImageIndex to ShadowUbo |
| `data/pak_0_engine/shaders/includes/globalset.shader` | **MODIFY** — add sssImageIndex to ShadowData |
| `data/pak_0_engine/shaders/includes/shadow.shader` | **MODIFY** — sample SSS in sampleShadow() |
| `src/renderer/vulkan2/pass/shadow/VulkanShadowPass.c` | **MODIFY** — set shadowUbo.sssImageIndex |
