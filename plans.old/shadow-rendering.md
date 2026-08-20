# Shadow Rendering Plan

Cascaded Shadow Maps (CSM) with 4 cascades, 2048×2048 per cascade, logarithmic/linear split. Uses the default sun from LightSystem. Supports both meshlet and triangle mesh rendering.

## Overview

1. Split the camera frustum into 4 cascades using a log/linear blend
2. For each cascade, compute a tight orthographic projection fitting the sub-frustum in light space
3. Render all geometry (meshlets + triangles) into each cascade layer of a depth-only shadow map array
4. In forward passes, select the appropriate cascade based on fragment view-space depth and sample for shadow

## Architecture

```
[Camera frustum] → split into 4 cascades
                         ↓
[LightSystem sun] → [VulkanShadowPass] → shadow depth array (4 × 2048×2048)
                                              ↓
                              [meshlet.frag / triangle.frag] select cascade + sample shadow
```

**Pass ordering** (in Vulkan.c): Shadow pass runs **before** depth prepass, after culling.
```
vulkanGeometryCullingPass
vulkanShadowPass
vulkanDepthPass
vulkanMeshletRenderPass
vulkanTriangleRenderPass
...
```

## Constants

| Parameter | Value |
|-----------|-------|
| Shadow map resolution | 2048 × 2048 per cascade |
| Cascade count | 4 |
| Lambda (log/linear blend) | 0.75 |
| Max shadow distance | min(camera zfar, 500m) |
| Depth clear | 1.0 (standard depth, NOT reverse-Z) |
| Depth compare | LESS_OR_EQUAL |
| Depth bias | 0.005 (scaled per cascade) |
| Normal bias | 0.05 (scaled per cascade) |
| PCF kernel | 3×3 |
| Depth format | D32_SFLOAT |

## Key Implementation Details

### Cascade Splitting
Uses a logarithmic/linear blend (lambda=0.75) to distribute cascades:
- Cascade 0: near objects (high detail)
- Cascade 3: far objects (low detail)

### Per-Cascade Rendering (Option A: Multi-pass)
Each cascade is rendered in a separate `vulkanBeginRender`/`vulkanEndRender` pair, targeting a specific layer of the depth image array via `depthLayer` parameter.

### Shadow Image
A single `VulkanImage` with `layers=4`, creating a `VK_IMAGE_VIEW_TYPE_2D_ARRAY`. Per-layer `VK_IMAGE_VIEW_TYPE_2D` views are registered individually in the texture pool for fragment shader sampling.

### Cascade Selection in Fragment Shader
Fragments compute their view-space depth (`-viewPos.z`) and compare against `cascadeSplits` to select the right cascade. Normal bias is scaled by cascade index to reduce acne on far cascades.

### Debug Modes (shadow.shader SHADOW_DEBUG)
- 0: Production shadows
- 1: Cascade visualization (R/G/B/Y)
- 2-6: UV, depth, difference, and raw shadow debug views

## Files Modified

- `src/ecs/system/light/Light.h` — `ShadowUbo` with per-cascade matrices and split distances
- `src/renderer/vulkan2/pass/shadow/VulkanShadowPass.c` — CSM computation and multi-pass rendering
- `src/renderer/vulkan2/pipeline/VulkanPipe.h` — `depthLayer` field in `VulkanBeginRenderInfo`
- `src/renderer/vulkan2/pipeline/VulkanPipe.c` — Per-layer depth view selection
- `src/renderer/vulkan2/resources/VulkanResourceManager.h/c` — `vulkanAddImageViewToPool`/`vulkanRemoveImageViewFromPool`
- `data/pak_0_engine/shaders/includes/globalset.shader` — `ShadowData` with cascade arrays
- `data/pak_0_engine/shaders/includes/shadow.shader` — Cascade selection + PCF sampling
- `data/pak_0_engine/shaders/pass/shadow/shadow_meshlet.vert` — `cascadeIndex` push constant
- `data/pak_0_engine/shaders/pass/shadow/shadow_triangle.vert` — `cascadeIndex` push constant
