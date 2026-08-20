# Order-Independent Transparency (OIT) Implementation Plan

## Approach: Weighted Blended OIT (McGuire & Bavoil 2013)

Chosen for its simplicity, single-pass design, and good integration with the existing GPU-driven indirect draw pipeline. No sorting, no per-pixel linked lists, no extra geometry passes. The tradeoff is approximate blending (not exact back-to-front), which is acceptable for most game transparency.

---

## Overview

Currently, both opaque and transparent triangle instances go through a single draw call (`vkCmdDrawIndexedIndirectCount`) in the triangle render pass with simple alpha blending on sceneColor. This is order-dependent and broken for overlapping transparent surfaces.

The plan splits rendering into:
1. **Opaque pass** (existing triangle render pass, no blending)
2. **OIT accumulation pass** (transparent objects → two render targets)
3. **OIT composite pass** (full-screen resolve blending OIT results onto sceneColor)

---

## Phase 1: Separate Opaque and Transparent Draw Streams

### 1.1 Culling shader split (`triangle_culling.comp`)

The culling shader currently writes all visible instances into a single indirect buffer + draw count. Modify it to produce **two streams**:

- Read `materialId` → fetch `featureMask` from material buffer
- If `MAT_ALPHA_BLEND` bit is set → write to **transparent** indirect/culled/drawCount buffers
- Otherwise → write to **opaque** indirect/culled/drawCount buffers

This requires two atomic draw count counters and two sets of indirect+culled buffers.

### 1.2 New GPU buffers (`VulkanScene`)

Add per-flight-frame buffers to `VulkanScene`:

```c
// Transparent triangle buffers (mirrors existing opaque ones)
VulkanBuffer triangleTransparentIndirectBuffer[FRAMES_IN_FLIGHT];
VulkanBuffer triangleTransparentDrawCountBuffer[FRAMES_IN_FLIGHT];
VulkanBuffer triangleTransparentCulledBuffer[FRAMES_IN_FLIGHT];
```

Size them identically to the opaque counterparts (worst case: all instances are transparent).

### 1.3 Culling dispatch (`VulkanGeometryCullingPass` or equivalent)

Update push constants to pass the additional buffer addresses. The culling dispatch itself doesn't change — same thread count, same invocation per instance.

### 1.4 Triangle render pass changes

Modify `VulkanTriangleRenderPass.c`:
- Remove `.blend = 1` from the pipeline — opaque pass should not blend.
- Draw only from the **opaque** indirect/drawCount buffers.
- This pass now only handles opaque + alpha-masked geometry.

---

## Phase 2: OIT Accumulation Pass

### 2.1 New pass: `VulkanOitAccumulatePass`

Create `c-engine/renderer/vulkan2/pass/oit/VulkanOitAccumulatePass.c/.h`

**Render targets** (new frame resources):
- `oitAccum`: `R16G16B16A16_SFLOAT` — weighted color accumulation (premultiplied RGB × weight, alpha × weight)
- `oitReveal`: `R8_UNORM` — revealage (product of `1 - alpha`)

Both same resolution as sceneColor.

**Pipeline configuration:**
- Depth test enabled (read existing depth from depth prepass), depth write **off**
- Blend on `oitAccum`: `ONE / ONE` additive
- Blend on `oitReveal`: `ZERO / ONE_MINUS_SRC_COLOR` (multiplicative)
- No G-buffer outputs (normals/material not needed for transparent surfaces)

**Shader** (`oit_accumulate.vert` / `oit_accumulate.frag`):
- Vertex shader: identical to `triangle.vert` (reuse or symlink)
- Fragment shader: full PBR lighting (copy from `triangle.frag`), then at the end:

```glsl
// Weighted blended OIT output
float z = gl_FragCoord.z;
float weight = alpha * max(1e-2, min(3e3, 
    10.0 / (1e-5 + pow(z / 200.0, 4.0))));

outAccum  = vec4(color * alpha * weight, alpha * weight);
outReveal = alpha; // blended multiplicatively via (ZERO, ONE_MINUS_SRC)
```

**Clear values:**
- `oitAccum` → `(0, 0, 0, 0)`
- `oitReveal` → `1.0` (fully revealed = no transparency yet)

**Draw call:**
- `vkCmdDrawIndexedIndirectCount` using **transparent** indirect/drawCount buffers

### 2.2 Frame resources

Add to `VulkanFrameResources`:
```c
VulkanImage oitAccum;
VulkanImage oitReveal;
```

Create/destroy alongside other frame resources. Expose via getter functions.

### 2.3 Pass ordering

Insert after the triangle render pass and skybox, before composite:
```
... → vulkanTriangleRenderPass → vulkanSkyboxPass → vulkanOitAccumulatePass → vulkanHiZPass → ...
```

---

## Phase 3: OIT Composite Pass

### 3.1 New pass: `VulkanOitCompositePass`

Create `c-engine/renderer/vulkan2/pass/oit/VulkanOitCompositePass.c/.h`

**Type:** Full-screen compute or full-screen triangle

**Operation:** Blend OIT results onto sceneColor in-place:

```glsl
vec4 accum   = texelFetch(oitAccumTex, coord, 0);
float reveal = texelFetch(oitRevealTex, coord, 0).r;

// Skip pixels with no transparency
if (reveal > 0.9999) return;

vec3 transparentColor = accum.rgb / max(accum.a, 1e-5);
vec3 opaqueColor      = imageLoad(sceneColor, coord).rgb;

vec3 result = transparentColor * (1.0 - reveal) + opaqueColor * reveal;
imageStore(sceneColor, coord, vec4(result, 1.0));
```

**Pass ordering:** Immediately after the OIT accumulation pass, before HiZ/SSPR/composite.

### 3.2 Push constants

```c
typedef struct OitCompositePushConstants {
    u32 oitAccumIndex;
    u32 oitRevealIndex;
    u32 sceneColorIndex;  // storage image for read-write
    u32 width;
    u32 height;
} OitCompositePushConstants;
```

---

## Phase 4: Material Buffer Access in Culling Shader

The culling shader currently doesn't access the material buffer. To classify opaque vs transparent:

### Option A: Material buffer in culling shader
- Bind the global material SSBO (already in the global descriptor set via `globalset.shader`)
- Read `materialBuffer.materials[inst.materialId].featureMask`
- Check `MAT_ALPHA_BLEND` bit

### Option B: Bake transparency flag into `GpuTriangleInstance`
- Add a `flags` field (or repurpose `_pad0`) in `GpuTriangleInstance`
- Set a `TRIANGLE_INSTANCE_TRANSPARENT` bit on CPU upload
- Culling shader checks the flag without needing material buffer access

**Recommendation:** Option B — simpler, avoids coupling culling to material buffer layout, negligible memory cost.

---

## Phase 5: Shader Files

New shader files under `c-engine/data/pak_0_engine/shaders/pass/oit/`:

| File | Description |
|------|-------------|
| `oit_accumulate.vert` | Same as `triangle.vert` (can `#include` shared code) |
| `oit_accumulate.frag` | Full PBR lighting → WBOIT weight output |
| `oit_composite.comp` | Full-screen resolve onto sceneColor |

Update `scripts/build.sh` or shader compilation scripts to compile these.

---

## Phase 6: Pass Registration

In `Vulkan.c`, update pass ordering:

```c
addPass(&vulkanTriangleRenderPass);   // opaque only now
addPass(&vulkanSkyboxPass);
addPass(&vulkanOitAccumulatePass);    // NEW: transparent accumulation
addPass(&vulkanOitCompositePass);     // NEW: resolve onto sceneColor
addPass(&vulkanHiZPass);
addPass(&vulkanSSPRPass);
addPass(&vulkanCompositePass);
// ... rest unchanged
```

---

## Summary of File Changes

| File | Change |
|------|--------|
| `c-engine/renderer/vulkan2/scene/VulkanScene.h` | Add transparent indirect/culled/drawCount buffers |
| `c-engine/renderer/vulkan2/scene/VulkanScene.c` | Allocate/destroy transparent buffers |
| `shaders/pass/triangle/triangle_culling.comp` | Split output into opaque/transparent streams |
| `c-engine/renderer/vulkan2/pass/triangle/VulkanTriangleRenderPass.c` | Remove `.blend`, draw opaque only |
| `c-engine/renderer/vulkan2/resources/VulkanFrameResources.c/.h` | Add `oitAccum` + `oitReveal` images |
| `c-engine/renderer/vulkan2/pass/oit/VulkanOitAccumulatePass.c/.h` | **NEW** — accumulation pass |
| `c-engine/renderer/vulkan2/pass/oit/VulkanOitCompositePass.c/.h` | **NEW** — composite pass |
| `shaders/pass/oit/oit_accumulate.vert` | **NEW** |
| `shaders/pass/oit/oit_accumulate.frag` | **NEW** |
| `shaders/pass/oit/oit_composite.comp` | **NEW** |
| `c-engine/renderer/vulkan2/Vulkan.c` | Register new passes |
| `CMakeLists.txt` | Add new `.c` files to `c-engine` sources |
| `scripts/build.sh` (or shader script) | Compile new shaders |
| `GpuTriangleInstance` | Repurpose `_pad0` as `flags` for transparency bit |
| Geometry culling pass (push constants) | Pass additional buffer addresses |

---

## Risks and Considerations

1. **WBOIT weight function tuning** — The depth weight function (`10 / (1e-5 + pow(z/200, 4))`) may need tuning for the project's depth range. If near/far planes differ significantly from typical values, colors can wash out or darken.

2. **Particle systems** — If particles exist later, they benefit from the same OIT path. No special handling needed since they'd just be transparent triangle instances.

3. **Refraction/transmission** — Materials with `MAT_HAS_TRANSMISSION` currently read sceneColor for transmitted light. With OIT, transmitted objects are rendered into the accumulation buffer and don't have access to the opaque sceneColor behind them. May need to sample the opaque-only sceneColor before OIT composite for transmission approximation.

4. **TAA interaction** — WBOIT is temporally stable by nature (no sorting flicker). Should integrate well with the existing TAA pass.

5. **Meshlet path** — The meshlet render pass (`VulkanMeshletRenderPass`) would also need an opaque/transparent split for full coverage. This plan focuses on the triangle path; meshlet OIT can follow the same pattern.

6. **Performance** — WBOIT adds one extra render pass for transparent geometry plus a full-screen composite. Cost is proportional to transparent fragment count. The extra frame resource images add ~2× pixel cost (RGBA16F + R8) at render resolution.
