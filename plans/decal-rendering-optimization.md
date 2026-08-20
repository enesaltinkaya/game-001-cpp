# Decal Rendering Optimization Plan

## Problem

The first screen-space decal implementation uses a fullscreen compute pass where every pixel loops over every submitted decal.

This is simple but scales poorly:

```txt
cost ~= screen_pixels * visible_decal_count
```

Azgaar road decals can produce thousands of persistent decals. Even after CPU frustum/distance culling, a fullscreen loop is not the right architecture for many decals.

## Goals

- Remove the fullscreen `for each pixel -> for each decal` bottleneck.
- Keep the reusable screen-space decal API.
- Support both gameplay decals and large persistent road/trail decals.
- Avoid projecting every decal over the whole screen.
- Preserve current features:
  - depth reconstruction
  - oriented projector boxes
  - procedural fallback decals
  - color/alpha/emissive overlay
  - ground-only normal filtering

## Recommended Architecture

Move from fullscreen compute to **rasterized decal volumes**.

Instead of dispatching all screen pixels, draw one box/cube per decal. The GPU rasterizer limits fragment shader work to pixels covered by each decal volume.

```txt
current:
  fullscreen pixels * decals

optimized:
  sum(screen pixels covered by each visible decal volume)
```

For roads/trails this is much cheaper because each segment covers a small rectangle on screen.

## Phase 1: Rasterized Decal Volume Pass

### Overview

Replace or supplement the current compute decal pass with a graphics pipeline:

```txt
VulkanDecalPass
  - uploads visible decals to GPU buffer
  - draws instanced unit cubes
  - vertex shader expands cube by decal model matrix
  - fragment shader reconstructs world position from depth
  - fragment shader transforms world position by decal inverse model
  - discard outside decal box
  - blend result into HDR scene color
```

### Pass placement

Keep current placement:

```txt
opaque/sky/OIT/SSR/volumetric -> decal -> composite
```

or, preferably for roads that should participate in fog/composite:

```txt
opaque/sky/OIT/SSR/volumetric -> decal -> composite
```

The existing placement before composite is fine.

### New / updated files

```txt
c-engine/renderer/vulkan/pass/decal/VulkanDecalPass.c
c-engine/data/pak_0_engine/shaders/pass/decal/decal.vert
c-engine/data/pak_0_engine/shaders/pass/decal/decal.frag
```

The current compute shader can be kept temporarily as a fallback/debug path:

```txt
c-engine/data/pak_0_engine/shaders/pass/decal/decal.comp
```

### GPU decal struct

Add both model and inverse model to avoid reconstructing model in shader:

```c
typedef struct DecalGpu {
    mat4 model;
    mat4 invModel;
    vec4 color;
    vec4 params0; // opacity, normalThreshold, edgeFeather, time
    vec4 params1; // uvScale.xy, flags, textureId
} DecalGpu;
```

### Vertex shader

Use hardcoded unit cube vertices from `gl_VertexIndex` or a small static cube vertex/index buffer.

Recommended first implementation: hardcoded 36 vertices, no vertex buffer.

Pseudo:

```glsl
vec3 cubePos = unitCubeVertex(gl_VertexIndex); // [-1, 1]
DecalGpu d = decals[gl_InstanceIndex];
vec4 world = d.model * vec4(cubePos, 1.0);
gl_Position = sceneBuffer.cameras[0].viewProjectionNoJitter * world;
```

Use normal front-face/back-face culling carefully. For camera-inside-decal cases, disabling cull is simplest.

### Fragment shader

Fragment shader is similar to current compute shader, except `coord` comes from `gl_FragCoord`:

```glsl
ivec2 coord = ivec2(gl_FragCoord.xy);
vec2 uv = (vec2(coord) + 0.5) / viewportSize;
float depth = texelFetch(depthTexture, coord, 0).r;
reconstruct world position;
local = invModel * vec4(worldPos, 1);
if outside [-1, 1], discard;
blend decal color;
```

### Blending

Use graphics color blending instead of storage image load/store where possible:

Initial alpha mode:

```txt
srcColorBlendFactor = SRC_ALPHA
 dstColorBlendFactor = ONE_MINUS_SRC_ALPHA
 colorBlendOp = ADD
srcAlphaBlendFactor = ONE
 dstAlphaBlendFactor = ONE_MINUS_SRC_ALPHA
 alphaBlendOp = ADD
```

For emissive/additive decals, either:

1. create a second additive pipeline and sort decals by blend mode, or
2. keep emissive approximated in alpha mode for first rasterized implementation.

Recommended Phase 1: alpha-only graphics pass, then add additive pipeline in Phase 2.

### Depth state

Do not write depth.

Depth test options:

- `depthTestEnable = false`: simplest; shader depth reconstruction/discard handles correctness.
- `depthTestEnable = true` with a conservative mode can reduce fragments, but decals are volumes and reconstructed depth is sampled separately, so start with depth test off.

### MSAA

Current scene color may have MSAA and resolved variants. For decal overlay, prefer writing to the resolved HDR scene color after MSAA opaque resolve, or ensure pass targets the same color image used by composite.

If writing into MSAA color directly, fragment shader depth sampling from resolved depth becomes complicated. Recommended:

```txt
write decals into resolved sceneColor after opaque/MSAA resolve
```

If the existing frame resources do not resolve before decal placement, add/confirm a resolve step or keep decal pass after passes that already use resolved `sceneColor`.

## Phase 2: CPU Visibility and Batching

Keep CPU culling, but make it more deliberate.

### Culling

For every decal:

1. sphere/frustum cull using decal OBB radius
2. distance cull by category
3. optional screen-size cull
4. optional receiver/layer cull later

Suggested category distances:

```txt
spell/gameplay decals: no aggressive distance cull, lifetime usually short
roads/trails: 800m-1500m depending on camera height
small stains/blood: 150m-300m
large world decals: custom maxDistance
```

Add fields later:

```c
float maxDrawDistance;
u32 category;
u32 sortKey;
```

### Batching

Sort visible decals by:

```txt
blend mode -> texture id -> flags
```

Then draw contiguous batches with the same pipeline/blend mode.

Phase 1 can draw all visible decals in one instanced draw if all alpha blended.

## Phase 3: Tile / Cluster Decal Lists for Very Many Decals

Rasterized volumes should be enough for thousands of road segments, but very dense decals can still overdraw heavily.

If needed, add tiled decal lists:

1. compute pass bins decals into screen tiles, e.g. 16x16 or 32x32 pixels
2. decal resolve pass loops only decals affecting the current tile
3. use atomic counters / prefix sums for tile list construction

This architecture is useful when decals are large or overlap heavily, but it is more complex than rasterized volumes.

Do not implement tile lists before measuring rasterized decals.

## Phase 4: Road-Specific LOD / Streaming

Azgaar roads are persistent and numerous. They should not all remain active decals forever.

### Tile buckets

Bucket generated road decals by Azgaar terrain tile:

```txt
key = floor(worldX / AZGAAR_TILE_SIZE_METERS), floor(worldZ / AZGAAR_TILE_SIZE_METERS)
```

Only submit/build decals for tiles near the active streaming center.

This avoids keeping 6000+ persistent decal handles in the generic decal system.

### Road LOD

Near:

```txt
individual screen-space road decals
```

Medium:

```txt
longer simplified road decals, fewer segments
```

Far:

```txt
no road decals, or map/vector overlay only
```

### Route simplification

Before decal generation, simplify route polylines with Ramer-Douglas-Peucker or angle/distance threshold.

Suggested thresholds:

```txt
roads: 10m-25m
trails: 15m-35m
```

This can reduce segment count without visible loss.

## Phase 5: Material/G-buffer Decals Optional

If roads look too much like UI overlays, add material decal mode.

Pre-lighting decals would write to G-buffer/material buffers:

- albedo
- roughness
- normal
- emissive

This improves integration but requires deeper renderer changes.

Keep color-overlay decal mode for gameplay telegraphs and magic effects.

## Implementation Details for Rasterized Pass

### Pipeline creation

Current `VulkanPipe` supports graphics pipelines like skybox/final passes. Create a decal graphics pipe with:

```c
vulkanCreatePipe(
    .name = "decal",
    .vs = "shaders/pass/decal/spv/decal.vert.spv",
    .fs = "shaders/pass/decal/spv/decal.frag.spv",
    .colorFormat1 = VK_FORMAT_R16G16B16A16_SFLOAT,
    .clearColor1Enabled = 0,
    .depthFormat = VK_FORMAT_D32_SFLOAT,
    .clearDepthEnabled = 0,
    .noCull = 1,
    .blend = 1 // or equivalent VulkanPipe field if available
);
```

If `VulkanPipe` does not expose blending controls yet, add minimal support for alpha/additive blend presets.

### Draw call

```c
vulkanBeginRender(... sceneColor ... depth optional ...);
vulkanBindPipe(cmd, &decalPipe);
vulkanPush(... DecalPushConstants ...);
vkCmdDraw(cmd->cmd, 36, visibleDecalCount, 0, 0);
vulkanEndRender(cmd);
```

### Push constants

```c
typedef struct DecalPushConstants {
    u64 decalsAddress;
    u32 decalCount;
    u32 depthIndex;
    u32 normalsIndex;
    u32 width;
    u32 height;
} DecalPushConstants;
```

No storage output index is needed if using framebuffer blending.

### Shader output

```glsl
layout(location = 0) out vec4 outColor;
outColor = vec4(decalRgb, alpha);
```

Let fixed-function blending combine with scene color.

## Validation Plan

### Build

```bash
./scripts/build.sh
```

### Runtime log

```bash
./scripts/run.sh play log 5000
```

### Screenshot

```bash
./scripts/run.sh play screenshot /tmp/decal-rasterized.png
```

### Metrics to compare

Before/after:

- decal pass GPU time
- visible decal count after CPU culling
- draw calls
- frame time in road-heavy areas

Expected result:

```txt
fullscreen compute road case: tens of ms
rasterized decal volume road case: low single-digit ms or less, depending on overdraw
```

## Risks / Edge Cases

- Camera inside a decal volume: disable culling or render backfaces correctly.
- Very large decals still cover much of the screen and can be expensive.
- Overlapping road intersections can cause overdraw; acceptable initially.
- Fixed-function blending makes per-decal blend mode require batching/pipelines.
- If sceneColor is not in color attachment layout at this point, transitions must be updated.
- MSAA/resolved scene color ownership needs careful verification.

## Recommended Next Step

Implement Phase 1: rasterized alpha-blended decal volumes.

Keep the existing compute decal path temporarily behind a debug/env switch:

```txt
ENGINE_DECAL_COMPUTE=1
```

After rasterized decals are stable and faster, remove or reserve compute path for future tiled decal experiments.
