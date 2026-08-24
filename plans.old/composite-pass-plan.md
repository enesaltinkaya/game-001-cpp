# Composite Pass — Implementation Plan
_Step 6 of progress.md (Phase 7a of the roadmap)_

---

## Overview

The SSR pass produces `SSRTextureUpsampled` (full-res, RGBA16F) every frame but nothing consumes it — TAA reads `SceneColor` directly. This step inserts a lightweight compute pass between SSR and TAA that blends the SSR contribution into a new `CompositeScene` target using the Fresnel term, then redirects TAA to read `CompositeScene` instead of `SceneColor`.

```
Before:
  SSR → SSRTextureUpsampled (dead-end)
  TAA → reads SceneColor

After:
  SSR → SSRTextureUpsampled
  Composite → reads SceneColor + SSRTextureUpsampled + G-buffer → writes CompositeScene
  TAA → reads CompositeScene
```

---

## Pass position in `Vulkan.c`

```
addPass(&vulkanSSRPass);        // writes SSRTextureUpsampled; leaves SceneColor/Normals/Material as SHADER_READ_ONLY
addPass(&vulkanCompositePass);  // NEW — reads all of the above; writes CompositeScene
addPass(&vulkanTaaPass);        // now reads CompositeScene instead of SceneColor
addPass(&vulkanFinalPass);      // unchanged — reads ResolvedColor
```

---

## New frame resource — `CompositeScene`

| Field | Value |
|---|---|
| Name | `"CompositeScene"` |
| Format | `VK_FORMAT_R16G16B16A16_SFLOAT` |
| Size | Full-res (`window.width × window.height`) |
| Usage flags | `SAMPLED \| STORAGE \| TRANSFER_DST` |
| Initial layout | `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` |

This is a dedicated output buffer. In-place modification of `SceneColor` is avoided: reading from and writing to the same image in a single dispatch requires `VK_IMAGE_LAYOUT_GENERAL` and would lose the original for debugging, and `SceneColor` lacks `STORAGE_BIT`.

---

## New files

```
c-engine/data/pak_0_engine/shaders/pass/composite/composite.comp
c-engine/renderer/vulkan2/pass/composite/VulkanCompositePass.h
c-engine/renderer/vulkan2/pass/composite/VulkanCompositePass.c
```

---

## `composite.comp` — detailed design

### Dispatch
- `local_size_x = 8, local_size_y = 8`
- One thread per full-resolution pixel

### Push constants
```glsl
layout(push_constant) uniform PushConstants {
    uint sceneColorIndex;      // SceneColor (sampled, R16G16B16A16_SFLOAT)
    uint ssrUpsampledIndex;    // SSRTextureUpsampled (sampled, R16G16B16A16_SFLOAT)
    uint depthIndex;           // HiZ mip-0 full-res depth (sampled, D32 read as R32)
    uint normalsIndex;         // G-buffer Normals (sampled, R16G16_SFLOAT oct-encoded)
    uint materialIndex;        // G-buffer Material (sampled, R8G8_UNORM: r=roughness g=metallic)
    uint outputImageIndex;     // CompositeScene (storage, R16G16B16A16_SFLOAT)
    uint width;
    uint height;
} pc;
```

### Per-thread algorithm

```
coord = ivec2(gl_GlobalInvocationID.xy)
if (coord >= vec2(width, height)) return

uv = (vec2(coord) + 0.5) / vec2(width, height)

// ── 1. Sample SceneColor ─────────────────────────────────────────────────────
vec3 sceneCol = texture(sampler2D(SceneColor, SAMPLER_LINEAR), uv).rgb

// ── 2. Sample SSR (premultiplied: rgb = hitColor * confidence, a = confidence) ──
vec4 ssrSample = texture(sampler2D(SSRUpsampled, SAMPLER_LINEAR), uv)

// ── 3. Fast path: no SSR contribution at this pixel ─────────────────────────
// This covers sky pixels, high-roughness surfaces, and missed rays.
// Avoids the G-buffer reads and Fresnel math for the majority of the screen.
if (ssrSample.a < 0.001) {
    imageStore(output, coord, vec4(sceneCol, 1.0))
    return
}

// ── 4. G-buffer reads ───────────────────────────────────────────────────────
float depth     = texelFetch(HiZ mip0, coord, 0).r
vec2  normEnc   = texelFetch(Normals,  coord, 0).rg
vec2  matSample = texelFetch(Material, coord, 0).rg
float roughness = matSample.r
float metallic  = matSample.g

// Sky guard (should be caught by ssrSample.a check above, but defensive)
if (depth == 0.0) {
    imageStore(output, coord, vec4(sceneCol, 1.0))
    return
}

// ── 5. Reconstruct view-space position and normal ───────────────────────────
vec3 viewPos = reconstructViewPos(uv, depth)        // same helper as ssr.comp
vec3 viewDir = normalize(viewPos)
vec3 worldN  = OctDecode(normEnc)
vec3 viewN   = normalize(mat3(cameras[0].view) * worldN)
float NdotV  = max(dot(-viewDir, viewN), 0.0)

// ── 6. Fresnel (IBL variant, roughness-adjusted Schlick) ─────────────────────
// F0 approximation: dielectrics ≈ 0.04, metals ≈ 1.0 (greyscale, no albedo available)
// This is the same formula used by meshlet.frag for the IBL specular term.
vec3 F0 = mix(vec3(0.04), vec3(1.0), metallic)
vec3 F  = F0 + (max(vec3(1.0 - roughness), F0) - F0)
             * pow(clamp(1.0 - NdotV, 0.0, 1.0), 5.0)

// ── 7. Composite ─────────────────────────────────────────────────────────────
// ssrSample.rgb already contains hitColor * confidence (validity fade).
// Multiplying by F adds the physical reflectance fraction.
// Result: bright on mirrors/metals at grazing angles, zero for diffuse surfaces.
vec3 composite = sceneCol + F * ssrSample.rgb

imageStore(output, coord, vec4(composite, 1.0))
```

### Why `fresnelSchlickRoughness` and not plain `fresnelSchlick`

The same IBL Fresnel is used in `meshlet.frag` for the prefiltered specular term. SSR is replacing that term (or complementing it) for on-screen geometry. Using the same formula keeps the contribution physically consistent and prevents double-counting at smooth vs. rough transitions.

### Why we don't output alpha

`CompositeScene` is treated as an opaque HDR buffer. TAA and the final pass only read `.rgb`. Alpha is always `1.0`.

---

## `VulkanCompositePass.h`

```c
#pragma once
#include "ecs/system/System.h"
extern System vulkanCompositePass;
```

No extra getter needed — TAA will call `vulkanFrameResourcesGetCompositeScene()` directly.

---

## `VulkanCompositePass.c` — structure

### State
```c
static VulkanPipe pipeline;
static double elapsedCPU;
static double elapsedGPU;
```

No toggle. The composite pass always runs. When SSR is disabled (via Ctrl+R in `VulkanSSRPass`), `SSRTextureUpsampled` is all zeros, so the composite output equals `SceneColor` — a correct no-op at zero cost beyond the dispatch itself.

### `added()`
```c
pipeline = vulkanCreatePipe(.name = "composite",
                            .comp = "shaders/pass/composite/spv/composite.comp.spv");
```

### Push constant struct
```c
typedef struct CompositePushConstants {
    u32 sceneColorIndex;
    u32 ssrUpsampledIndex;
    u32 depthIndex;
    u32 normalsIndex;
    u32 materialIndex;
    u32 outputImageIndex;
    u32 width;
    u32 height;
} CompositePushConstants;
```

### `update()` — barrier flow

Image states at the start of this pass (guaranteed by SSR pass):
- `SceneColor`           → `SHADER_READ_ONLY_OPTIMAL`
- `SSRTextureUpsampled`  → `SHADER_READ_ONLY_OPTIMAL`
- `Normals`              → `SHADER_READ_ONLY_OPTIMAL`
- `Material`             → `SHADER_READ_ONLY_OPTIMAL`
- `HiZ image`            → `SHADER_READ_ONLY_OPTIMAL`
- `CompositeScene`       → `SHADER_READ_ONLY_OPTIMAL` (initial layout, or from last frame)

```c
// Only CompositeScene needs a transition — everything else is already readable.
vulkanTransition(cmd, compositeScene, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

vulkanBeginProfile(cmd, &pipeline.profile, 0);
vulkanBindPipe(cmd, &pipeline);

CompositePushConstants pc = {
    .sceneColorIndex    = (u32)sceneColor->sampledPoolIndex,
    .ssrUpsampledIndex  = (u32)ssrUpsampled->sampledPoolIndex,
    .depthIndex         = (u32)hiz->sampledPoolIndex,          // HiZ mip-0
    .normalsIndex       = (u32)normals->sampledPoolIndex,
    .materialIndex      = (u32)material->sampledPoolIndex,
    .outputImageIndex   = (u32)compositeScene->storagePoolIndex,
    .width              = compositeScene->extent.width,
    .height             = compositeScene->extent.height,
};
vulkanPush(cmd, &pipeline, sizeof(pc), &pc);

u32 groupsX = (compositeScene->extent.width  + 7) / 8;
u32 groupsY = (compositeScene->extent.height + 7) / 8;
vulkanDispatch(cmd, &pipeline, groupsX, groupsY, 1);
vulkanEndProfile(cmd, &pipeline.profile, 0);

// Leave CompositeScene readable for TAA.
vulkanTransition(cmd, compositeScene, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

// SceneColor, Normals, Material remain SHADER_READ_ONLY — TAA resets them at frame end.
```

### Image pointers to acquire

```c
VulkanImage* hiz          = vulkanHiZGetCurrentImage();
VulkanImage* sceneColor   = vulkanFrameResourcesGetSceneColor();
VulkanImage* ssrUpsampled = vulkanFrameResourcesGetSSRTextureUpsampled();
VulkanImage* normals      = vulkanFrameResourcesGetNormals();
VulkanImage* material     = vulkanFrameResourcesGetMaterial();
VulkanImage* composite    = vulkanFrameResourcesGetCompositeScene();

if (!hiz || !sceneColor || !ssrUpsampled || !normals || !material || !composite) return;
```

### `postUpdate()` / `removed()`
Standard pattern — mirror `VulkanSSRPass.c`.

---

## Changes to existing files

### `VulkanFrameResources.c`

**1. Add field to struct:**
```c
typedef struct VulkanFrameResources {
    ...
    VulkanImage compositeScene;   // ← add after ssrTextureUpsampled
    ...
} VulkanFrameResources;
```

**2. In `recreate()`:**
```c
frameResources.compositeScene =
    vulkanCreateImage(.name   = "CompositeScene",
                      .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                      .usage  = VK_IMAGE_USAGE_SAMPLED_BIT |
                               VK_IMAGE_USAGE_STORAGE_BIT  |
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      .width  = window.width,
                      .height = window.height);
```

**3. In `destroyAll()`:**
```c
destroyImage(&frameResources.compositeScene);
```

**4. In `transitionInitialLayouts()`:**
```c
vulkanTransition(cmd,
                 &frameResources.compositeScene,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 0, 1);
```

**5. Add getter:**
```c
VulkanImage* vulkanFrameResourcesGetCompositeScene(void) {
    return frameResources.compositeScene.img ? &frameResources.compositeScene : NULL;
}
```

---

### `VulkanFrameResources.h`

Add one getter declaration:
```c
VulkanImage* vulkanFrameResourcesGetCompositeScene(void);
```

---

### `VulkanTaaPass.c`

TAA currently reads `SceneColor`. Switch it to `CompositeScene`.

**1. Fetch CompositeScene:**
```c
// Add:
VulkanImage* compositeScene = vulkanFrameResourcesGetCompositeScene();
// Add to null guard:
if (!sceneColor || !resolvedColor || !compositeScene || ...) return;
```

**2. Remove the SHADER_READ_ONLY transition for sceneColor** (it arrives already in that state from the SSR pass, and the composite pass leaves it unchanged):
```c
// Remove:
vulkanTransition(cmd, sceneColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
// Add (defensive — composite pass already does this, but stays self-documenting):
vulkanTransition(cmd, compositeScene, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
```

**3. Swap TAA's color input:**
```c
// Before:
.currentColorTextureIndex = sceneColor->sampledPoolIndex,
// After:
.currentColorTextureIndex = compositeScene->sampledPoolIndex,
```

**4. Keep the end-of-pass sceneColor reset unchanged** — next frame's rendering pass writes to SceneColor:
```c
// Unchanged:
vulkanTransition(cmd, sceneColor, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
```

`CompositeScene` does not need a reset at end-of-TAA — the composite pass will transition it from SHADER_READ_ONLY to GENERAL at the top of next frame's dispatch before writing.

---

### `Vulkan.c`

**1. Include:**
```c
#include "renderer/vulkan2/pass/composite/VulkanCompositePass.h"
```

**2. Insert pass:**
```c
addPass(&vulkanSSRPass);        // unchanged
addPass(&vulkanCompositePass);  // ← new, between SSR and TAA
addPass(&vulkanTaaPass);        // unchanged
```

---

## Image layout flow — full frame (after this step)

| Pass | SceneColor | SSRTextureUpsampled | CompositeScene | Notes |
|---|---|---|---|---|
| MeshletRender / TriangleRender | `COLOR_ATTACHMENT` | — | — | G-buffer writes |
| SkyboxPass | `COLOR_ATTACHMENT` | — | — | Sky fill |
| HiZPass | `SHADER_READ_ONLY` (end) | — | — | HiZ generates, leaves depth copy readable |
| SSRPass | `SHADER_READ_ONLY` | `SHADER_READ_ONLY` | — | Reads SceneColor; outputs SSR |
| **CompositePass** | `SHADER_READ_ONLY` | `SHADER_READ_ONLY` | `GENERAL` → `SHADER_READ_ONLY` | Reads both; writes CompositeScene |
| TAAPass | `SHADER_READ_ONLY` | — | `SHADER_READ_ONLY` | Reads CompositeScene; writes ResolvedColor |
| TAAPass (end) | `COLOR_ATTACHMENT` | — | — | Resets SceneColor for next frame |
| FinalPass | — | — | — | Reads ResolvedColor; tonemaps to swapchain |

---

## Build steps

1. **Frame resources**: add `compositeScene` to `VulkanFrameResources.c/.h` — build to verify struct/getter compile.
2. **Shader**: write `composite.comp` — compile with `scripts/build.sh` (shaders only) — fix GLSL errors.
3. **Pass**: write `VulkanCompositePass.h/.c` — build C — fix errors.
4. **Wire up**: update `Vulkan.c` (include + addPass) and `VulkanTaaPass.c` (swap input) — full build.
5. **Run**: verify in RenderDoc:
   - `CompositeScene` shows SceneColor plus SSR highlights on reflective surfaces.
   - `ResolvedColor` (TAA output) has stable reflections with no ghosting regression.
   - No Vulkan validation layer errors (especially no layout transition mismatches).
6. **Tune**: if Fresnel contribution is too strong or too weak, adjust the F0 dielectric constant (`0.04`) or clamp the final `F * ssrSample.rgb` term.
7. **Update `plans/progress.md`** — mark Step 6 done.

---

## Out of scope for this step

- Removing IBL specular at SSR-hit pixels to avoid double-counting: requires packing a "SSR replaces IBL" flag in the G-buffer. Deferred.
- Blending SSR into the translucent pass: SSR only covers opaque G-buffer pixels. Translucent SSR is a separate feature.
- Motion Blur (Step 7) and Bloom (Step 8): next in queue after this.
