# Next Renderer Plan

## Commit 1 — Fix the camera/velocity contract first

### Goal
Make the current jitter/velocity path match what the future TAA pass needs.

### Why first
The groundwork exists, but the contracts are still off:
- `prevViewProjection` is currently being used like previous non-jittered VP.
- We want both:
  - previous jittered VP
  - previous non-jittered VP
- velocity is currently written as NDC delta, but TAA and motion blur want pixel-space velocity.

### Files
- `src/ecs/system/camera/Camera.h`
- `src/ecs/system/camera/CameraSystem.c`
- `data/pak_0_engine/shaders/includes/globalset.shader`
- `data/pak_0_engine/shaders/pass/meshlet/depth_prepass.vert`
- `data/pak_0_engine/shaders/pass/triangle/triangle_depth.vert`

### Changes
- Add a second previous matrix to the camera UBO:
  - `prevViewProjection` = previous jittered
  - `prevViewProjectionNoJitter` = previous non-jittered
- In `CameraSystem.c`:
  - store both every frame
- In depth-prepass shaders:
  - use `viewProjectionNoJitter` and `prevViewProjectionNoJitter`
  - convert velocity to pixel units
  - clamp before storing to `RG16_SFLOAT`

Example target behavior:
```glsl
vec2 ndcVelocity = ndcCurrent - ndcPrev;
outVelocity = clamp(ndcVelocity * sceneBuffer.cameras[0].viewport,
                    vec2(-32767.0),
                    vec2(32767.0));
```

### Done when
- build passes
- no visual regression
- velocity buffer semantics are stable enough for TAA and motion blur later

---

## Commit 2 — Add a dedicated frame-resources module

### Goal
Stop treating the swapchain as the main scene target.

### Why
Everything after this depends on persistent offscreen images.

### Files
- new: `src/renderer/vulkan2/resources/VulkanFrameResources.h`
- new: `src/renderer/vulkan2/resources/VulkanFrameResources.c`
- `src/renderer/vulkan2/Vulkan.c`

### Resources to add now
Use simple safe formats first, not the roadmap’s packed formats yet.

Create:
- `SceneColor` → `VULKAN_RGBA16F`
- `Normals` → `VULKAN_RG16_SFLOAT`
- `Material` → `VULKAN_RG8_UNORM`
- `ResolvedColor` → `VULKAN_RGBA16F`
- `HistoryColor` → `VULKAN_RGBA16F`
- `PrevDepth` → `VULKAN_D32F` or `VULKAN_R32F` copy target depending on copy strategy

Do not worry yet about:
- SSR textures
- bloom chain
- AO texture
- packed `R11G11B10`
- packed bloom formats

### Module responsibilities
The new module should:
- create, destroy, and resize these images
- expose getters
- handle end-of-frame history swap
- be called from the Vulkan bootstrap and update flow

### Important repo-specific fix
`VulkanPipe.c` currently infers clear-enabled state from whether clear color values are non-zero.
That will break attachments that must clear to `0,0,0,0`.

Also patch:
- `src/renderer/vulkan2/pipeline/VulkanPipe.h`
- `src/renderer/vulkan2/pipeline/VulkanPipe.c`

Add explicit flags like:
- `clearColor1Enabled`
- `clearColor2Enabled`
- `clearColor3Enabled`

instead of using “non-zero means clear”.

### Done when
- frame resources resize with the window
- build passes
- nothing uses them yet, but lifecycle is solid

---

## Commit 3 — Move world rendering to HDR MRT, then add a final tonemap pass

### Goal
Render the scene offscreen, then present it properly.

### Files

#### Main passes
- `src/renderer/vulkan2/pass/grid/VulkanGridPass.c`
- `src/renderer/vulkan2/pass/meshlet/VulkanMeshletRenderPass.c`
- `src/renderer/vulkan2/pass/triangle/VulkanTriangleRenderPass.c`

#### Shaders
- `data/pak_0_engine/shaders/pass/grid/fragment.frag`
- `data/pak_0_engine/shaders/pass/meshlet/meshlet.frag`
- `data/pak_0_engine/shaders/pass/triangle/triangle.frag`
- maybe `data/pak_0_engine/shaders/includes/utils.shader`

#### New final pass
- new: `src/renderer/vulkan2/pass/final/VulkanFinalPass.h`
- new: `src/renderer/vulkan2/pass/final/VulkanFinalPass.c`
- new shaders:
  - `data/pak_0_engine/shaders/pass/final/final.vert`
  - `data/pak_0_engine/shaders/pass/final/final.frag`

#### Pass ordering
- `src/renderer/vulkan2/Vulkan.c`

### Changes
#### 1) Grid pass
Make grid render into `SceneColor`, not directly to swapchain.

#### 2) Meshlet / triangle passes
Change them to render to:
- RT0 = `SceneColor`
- RT1 = `Normals`
- RT2 = `Material`
- depth = existing prepass depth

#### 3) Fragment outputs
In both `meshlet.frag` and `triangle.frag`, write:
- `layout(location = 0)` HDR lit color
- `layout(location = 1)` oct-encoded normal
- `layout(location = 2)` roughness/metallic

For example:
- `Normals.rg` = oct-encoded world or view normal
- `Material.r` = roughness
- `Material.g` = metallic

#### 4) Final pass
Add a fullscreen pass that samples `SceneColor` and writes to swapchain:
- ACES tonemap
- manual gamma / `fromLinear()`
- no TAA yet
- no bloom yet

Useful helpers already exist in:
- `data/pak_0_engine/shaders/includes/utils.shader`

#### 5) Pass order
A good temporary order:
- grid
- culling
- shadow
- depth
- sss
- meshlet render
- triangle render
- hiz
- final
- rmlui

### Done when
- scene still renders
- swapchain only gets:
  - final post pass
  - UI
- world passes no longer write directly to swapchain

---

## Commit 4 — Add TAA MVP with history + prev-depth management

### Goal
Make the jitter actually pay off.

### Files

#### New pass
- new: `src/renderer/vulkan2/pass/taa/VulkanTaaPass.h`
- new: `src/renderer/vulkan2/pass/taa/VulkanTaaPass.c`
- new shader:
  - `data/pak_0_engine/shaders/pass/taa/taa.comp`

#### Resource/module changes
- `src/renderer/vulkan2/resources/VulkanFrameResources.c`
- `src/renderer/vulkan2/resources/VulkanImage.h`
- `src/renderer/vulkan2/resources/VulkanImage.c`
- `src/renderer/vulkan2/Vulkan.c`

### Changes
#### 1) Add real history flow
At end of frame:
- copy current depth to `PrevDepth`
- swap `ResolvedColor` and `HistoryColor`

#### 2) Add image copy helper
Add a dedicated helper for:
- color image copy
- depth image copy

`vulkanBlit()` is not enough as-is for this job.

#### 3) TAA MVP behavior
Keep it simple first:
- current = `SceneColor`
- history = `HistoryColor`
- velocity = depth-prepass velocity
- output = `ResolvedColor`

MVP algorithm:
- reproject history using velocity
- reject if out of bounds
- reject if depth mismatch against `PrevDepth`
- blend with a conservative alpha

Do not try to ship all of this in the first TAA commit:
- YCoCg
- variance clipping
- neighborhood min/max
- velocity dilation
- subgroup tricks

Those can come after the MVP is working.

#### 4) Final pass uses `ResolvedColor`
Once TAA exists, the final pass should sample `ResolvedColor`, not `SceneColor`.

### Done when
- camera motion is visibly less shimmery
- static geometry looks more stable
- no ghosting disasters
- build passes

---

## Commit 5 — Finish triangle frustum + Hi-Z culling

### Goal
Bring the triangle path up to the same baseline as meshlets.

### Why
This is currently one of the clearest roadmap gaps:
- meshlets: frustum + Hi-Z
- triangles: pass-through

### Files
- `src/renderer/vulkan2/scene/VulkanScene.h`
- `src/renderer/vulkan2/scene/VulkanScene.c`
- `src/renderer/vulkan2/pass/meshlet/VulkanMeshletCullingPass.c`
- `data/pak_0_engine/shaders/pass/triangle/triangle_culling.comp`

### Changes
#### 1) Add bounds to triangle instances
Extend `GpuTriangleInstance` with a bound:
- sphere is easiest
- AABB is fine too, but sphere is simpler

Likely fields:
```c
vec4 boundingSphere; // xyz center, w radius
```

#### 2) Populate bounds during scene upload
In `VulkanScene.c`, compute primitive bounds once on CPU and copy them into each triangle instance.

#### 3) Reuse meshlet culling logic
In `triangle_culling.comp`:
- frustum test
- previous-frame Hi-Z test
- only emit indirect draw if visible

### Done when
- triangle path no longer brute-forces everything
- draw counts drop when looking away or behind occluders
- build passes

---

## What not to do yet
Do not jump to these before the 5 commits above:
- SSR
- bloom
- motion blur
- clustered lights
- IBL precompute
- packed render-target optimizations

These all get easier once the renderer is already:
- offscreen
- HDR
- MRT-based
- history-aware

---

## After these 5 commits
The next milestone should be real Forward+ lighting:

1. light upload plumbing (`rendererUploadLights()` is currently empty)
2. point and spot light scene data in shaders
3. clustered light culling compute pass
4. forward shading consuming the light grid
5. then IBL

---

## Suggested order
1. camera/velocity contract cleanup
2. frame resource module
3. HDR MRT + final tonemap pass
4. TAA MVP
5. triangle frustum/Hi-Z culling
