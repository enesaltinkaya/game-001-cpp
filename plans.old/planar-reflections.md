# Plan: Planar Reflections via Mirrored Camera

## Problem

SSPR (Screen-Space Planar Reflections) cannot reflect geometry that is occluded
or off-screen from the main camera's point of view. The underside of objects
sitting on the reflective plane is never visible, creating large holes and
jagged edges that no amount of hole-filling can fix.

## Solution

Replace SSPR with a **mirrored-camera render pass** that renders the scene from
below the reflection plane (y=0), producing a correct and complete reflection
image at half resolution.

## Approach: Swap `cameras[0]` Mid-Frame

All shaders hardcode `cameras[0]`. Rather than adding a camera index push
constant to every shader, we:

1. Upload the **mirrored camera** into `cameras[0]` of the scene buffer
2. Run a simplified reflection render (culling → depth → lit render → skybox)
3. Overwrite `cameras[0]` with the **real camera**
4. Run the normal pipeline as before

The scene buffer is persistently mapped CPU memory, so writes are instant and
visible to subsequent GPU commands on the same command buffer (after appropriate
barriers).

## Execution Order

### Current pipeline

```
GridPass → GeometryCulling → ShadowPass → DepthPass → Phase2Occlusion →
LightCulling → SSS → AO → MeshletRender → TriangleRender → Skybox →
OitAccumulate → OitComposite → HiZ → SSPR → Composite → TAA → Bloom →
Final → RmlUI
```

### New pipeline

```
─── REFLECTION PASS (mirrored camera in cameras[0]) ──────────────
ReflectionPass:
  1. Write mirrored camera to cameras[0]
  2. Culling (reuse culling shaders, write to reflection indirect buffers)
  3. Depth prepass (half-res reflection depth image)
  4. Meshlet + Triangle render (half-res reflection color, no normals/material)
  5. Skybox render (into reflection color)
  6. Write real camera back to cameras[0]

─── MAIN PIPELINE (unchanged, real camera in cameras[0]) ─────────
GridPass → GeometryCulling → ShadowPass → DepthPass → Phase2Occlusion →
LightCulling → SSS → AO → MeshletRender → TriangleRender → Skybox →
OitAccumulate → OitComposite → HiZ → Composite → TAA → Bloom →
Final → RmlUI
```

The SSPR pass is **removed entirely**. The Composite pass reads the half-res
reflection color texture instead of the SSPR output.

## Mirrored Camera Construction

Reflect the main camera across the plane `y = 0` (normal = (0,1,0), d = 0):

```c
// Mirror camera position: negate Y
vec3 mirrorPos = { pos[0], -pos[1], pos[2] };

// Mirror camera direction: negate Y component of forward vector
vec3 mirrorDir = { dir[0], -dir[1], dir[2] };

// Build view matrix looking from mirrorPos along mirrorDir, with UP = (0,-1,0)
// (mirroring flips handedness, so we negate the up vector)
glm_look(mirrorPos, mirrorDir, (vec3){0, -1, 0}, mirrorView);
```

Then build the same reverse-Z projection as the main camera (same FOV, aspect,
near/far), but modify it with an **oblique near clip plane** so that nothing
below y=0 is rendered:

```c
// Oblique clip plane in view space: transform world-space plane (0,1,0,0)
// into view space, then replace the near plane row of the projection matrix.
// See "Oblique View Frustum Depth Projection and Clipping" (Eric Lengyel).
vec4 clipPlaneWorld = { 0, 1, 0, 0 }; // y > 0 half-space
vec4 clipPlaneView;
// clipPlaneView = transpose(inverse(mirrorView)) * clipPlaneWorld
// = inverse(transpose(mirrorView))^T ... simplify using glm
mat4 invTransView;
glm_mat4_inv(mirrorView, invTransView);
glm_mat4_transpose(invTransView);
glm_mat4_mulv(invTransView, clipPlaneWorld, clipPlaneView);

// Modify projection to use oblique near plane
glm_frustum_plane_oblique(projection, clipPlaneView);
```

**No jitter** is applied to the reflection camera (TAA is not run on the
reflection image).

**Frustum planes** are extracted from the mirrored VP matrix for culling.

## Half-Resolution Rendering

All reflection render targets are `width/2 × height/2`:

- **Reflection color**: `R16G16B16A16_SFLOAT` (same as SceneColor)
- **Reflection depth**: `D32_SFLOAT`

These are allocated in `VulkanFrameResources` replacing `ssprTemp` and
`ssprOutput`:

- `ssprTemp` → `reflectionColor` (half-res)
- `ssprOutput` → removed (or repurposed)
- New: `reflectionDepth` (half-res)

## Simplified Reflection Render

The reflection pass skips expensive effects:

- ✅ **Geometry culling** — must run with mirrored frustum planes
- ✅ **Depth prepass** — needed for correct depth testing
- ✅ **Meshlet render** — lit geometry (uses existing directional light + IBL)
- ✅ **Triangle render** — lit geometry
- ✅ **Skybox** — needed for sky reflections
- ❌ **Shadows** — skip (use unshadowed lighting or fixed ambient)
- ❌ **AO** — skip
- ❌ **SSS** — skip
- ❌ **Light culling** — skip (no forward+ point/spot lights in reflection)
- ❌ **OIT** — skip (no transparent objects in reflection)
- ❌ **Phase2 occlusion** — skip (single-pass culling only)
- ❌ **TAA** — skip
- ❌ **HiZ for occlusion culling** — skip (frustum-only culling for reflection)

### Shader implications for simplified lighting

The meshlet.frag and triangle.frag shaders currently do full PBR with shadow
sampling, forward+ light culling, AO, etc. For the reflection pass we need
either:

**Option A (recommended): Preprocessor variant**

- Add `#define REFLECTION_PASS` when compiling the reflection fragment shaders
- Guard shadow sampling, AO reads, and forward+ light loop behind
  `#ifndef REFLECTION_PASS`
- Use only directional light + IBL ambient in reflection mode
- Compile separate `.spv` files: `meshlet_reflection.frag.spv`, etc.

**Option B: Runtime branch via push constant**

- Add a `uint flags` push constant, check `flags & REFLECTION_BIT`
- Simpler but adds branching to every fragment

Option A is cleaner and has zero runtime overhead.

## Culling for Reflection

The culling compute shaders (`culling.comp`, `triangle_culling.comp`) read
frustum planes from `sceneBuffer.cameras[0].frustumPlanes`. Since we write the
mirrored camera to `cameras[0]` before dispatching reflection culling, the
existing shaders work unmodified.

However, the culling pass writes to the **same indirect/drawCount/culled
buffers** used by the main render. We need **separate buffers** for the
reflection pass to avoid clobbering the main pass data.

### Approach: Reflection-specific indirect buffers

Add to `VulkanScene`:

```c
VulkanBuffer reflIndirectBuffer[FRAMES_IN_FLIGHT];
VulkanBuffer reflDrawCountBuffer[FRAMES_IN_FLIGHT];
VulkanBuffer reflCulledMeshletsBuffer[FRAMES_IN_FLIGHT];
VulkanBuffer reflTriangleIndirectBuffer[FRAMES_IN_FLIGHT];
VulkanBuffer reflTriangleDrawCountBuffer[FRAMES_IN_FLIGHT];
VulkanBuffer reflTriangleCulledBuffer[FRAMES_IN_FLIGHT];
VulkanBuffer reflTriangleDSIndirectBuffer[FRAMES_IN_FLIGHT];
VulkanBuffer reflTriangleDSDrawCountBuffer[FRAMES_IN_FLIGHT];
VulkanBuffer reflTriangleDSCulledBuffer[FRAMES_IN_FLIGHT];
```

These are the same size/format as the main buffers. The reflection culling
dispatches write to these, and the reflection render draws read from them.

**Alternative (simpler, less memory):** Since the reflection pass runs
_before_ the main culling pass, we could reuse the same buffers — reflection
culling writes them, reflection render reads them, then main culling overwrites
them, main render reads them. This works because the reflection pass fully
completes (with barriers) before the main culling starts.

→ **Recommended: Reuse the same indirect buffers.** This saves ~9 buffer
allocations per scene per flight frame and is safe because of the serial
execution order. We just need barriers between reflection render → main culling.

## New Pass: `VulkanReflectionPass`

**File:** `c-engine/renderer/vulkan2/pass/reflection/VulkanReflectionPass.c`
**Header:** `c-engine/renderer/vulkan2/pass/reflection/VulkanReflectionPass.h`

This is a single `System` pass that internally orchestrates:

```c
System vulkanReflectionPass = {
    .name = "reflection",
    .added = added,        // create pipelines, allocate reflection targets
    .preUpdate = preUpdate,
    .update = update,      // the main work
    .postUpdate = postUpdate,
    .removed = removed,
};
```

### `update()` pseudocode

```c
void update(void) {
    if (vulkan.skipFrame || reflectionDisabled) return;

    VulkanCommand* cmd = vulkan.currentCmd;
    VulkanSceneBuffer* scene = getSceneBuffer(); // persistently mapped

    // ─── 1. Save real camera, write mirrored camera ──────────────
    CameraUbo realCamera;
    memcpy(&realCamera, &scene->cameras[0], sizeof(CameraUbo));

    CameraUbo mirrorCamera;
    buildMirroredCamera(&realCamera, &mirrorCamera); // see construction above
    memcpy(&scene->cameras[0], &mirrorCamera, sizeof(CameraUbo));

    // Pipeline barrier: ensure camera write is visible to GPU
    // (CPU writes to coherent mapped memory — no barrier needed for
    //  persistently-mapped coherent buffers on most drivers, but
    //  we need a pipeline barrier between the previous frame's reads
    //  and our new dispatches)

    // ─── 2. Reflection culling ───────────────────────────────────
    // Reset draw counts, dispatch culling (same shaders, same buffers)
    reflectionCull(cmd);

    // Barrier: compute → indirect draw
    computeToDrawBarrier(cmd);

    // ─── 3. Reflection depth prepass ─────────────────────────────
    reflectionDepthPrepass(cmd);

    // Barrier: depth write → depth read
    depthWriteToReadBarrier(cmd);

    // ─── 4. Reflection meshlet + triangle render ─────────────────
    reflectionMeshletRender(cmd);
    reflectionTriangleRender(cmd);

    // ─── 5. Reflection skybox ────────────────────────────────────
    reflectionSkyboxRender(cmd);

    // ─── 6. Restore real camera ──────────────────────────────────
    memcpy(&scene->cameras[0], &realCamera, sizeof(CameraUbo));

    // Barrier: ensure real camera is visible before main pipeline
    // (Actually automatic — serialized command buffer recording)
}
```

### Pipeline objects needed

The reflection pass creates its own `VulkanPipe` instances:

- `reflCullingPipe` — same shaders as `culling.comp`, `triangle_culling.comp`
- `reflDepthPipe` — same shaders as `depth_prepass.vert/frag`
- `reflMeshletPipe` — `meshlet.vert` + `meshlet_reflection.frag` (new)
- `reflTrianglePipe` — `triangle.vert` + `triangle_reflection.frag` (new)
- `reflTriangleDSPipe` — same as above but `noCull = 1`
- `reflSkyboxPipe` — same shaders as `skybox.vert/frag`

The culling and depth pipelines can use the exact same SPV files. Only the
fragment shaders need reflection variants.

## Reflection Fragment Shaders

Create simplified versions of `meshlet.frag` and `triangle.frag`:

**`meshlet_reflection.frag`** / **`triangle_reflection.frag`**:

- Copy from original
- Add `#define REFLECTION_PASS 1` at top
- Remove shadow sampling code
- Remove AO texture read
- Remove forward+ light loop (point/spot lights)
- Keep: directional light, IBL ambient/specular, emissive
- Output only to color attachment (no normals, no material G-buffer)
- Single color attachment: `layout(location = 0) out vec4 outColor;`

Alternatively, modify the originals with `#ifdef REFLECTION_PASS` guards and
compile two variants. This is better for maintenance.

## Composite Pass Changes

The composite shader currently reads `ssprOutputIndex` (premultiplied RGBA) and
applies BRDF-weighted blending. With the new approach:

- The reflection texture is a **fully lit half-res color image** (not premultiplied)
- Composite needs to:
  1. Sample the reflection color at the current pixel's reflected UV
  2. Only apply to floor-like surfaces (same normal/plane check as before)
  3. Blend using the same BRDF weight (F0 \* brdf.x + brdf.y)

### Key change in composite.comp

```glsl
// Old: read SSPR scatter result (premultiplied, same resolution)
vec4 reflSample = texelFetch(...ssprOutput..., coord, 0);

// New: sample half-res reflection texture at reflected UV
//   - Reconstruct world pos from depth
//   - Mirror through plane
//   - Project mirrored pos with real camera VP → get UV into reflection texture
//   OR simply: since the reflection texture is rendered from below, the
//   floor pixel at (u,v) sees the same content that the mirrored camera
//   rendered at roughly (u, 1-v) — but not exactly due to perspective.
//
// Actually, the simplest correct approach:
//   The reflection texture already contains what the floor should show.
//   For a floor pixel at world position P on the y=0 plane, the reflection
//   camera sees P at some screen coordinate. We need to find that coordinate.
//
//   reflUV = project(worldPos, mirrorViewProjection)
//
//   This requires passing the mirror VP matrix to the composite shader.

// Better approach: just use the floor pixel's screen UV directly.
//   The mirrored camera is symmetric about y=0, so for a pixel on the y=0
//   floor plane, its screen-space position in the main camera corresponds
//   to the same screen-space position in the mirrored camera (assuming
//   symmetric reflection). This is only exactly true for a perfectly
//   horizontal plane with the camera looking straight ahead — in general
//   we need the mirror VP projection.

// → Pass mirror VP matrix via push constant or scene buffer (cameras[1]).
```

**Recommended approach for composite:**

- After the reflection pass restores the real camera to `cameras[0]`, also
  write the **mirrored camera VP** to `cameras[1]` (there are 4 camera slots)
- In composite.comp:
  1. Reconstruct world position from depth
  2. Check if pixel is on the floor (normal alignment + plane distance)
  3. Project world position using `cameras[1].viewProjectionNoJitter` → get UV
     into the half-res reflection texture
  4. Sample reflection color with bilinear filtering
  5. Blend with BRDF weight, replacing IBL specular

This gives correct perspective-accurate reflection lookup regardless of camera
angle.

## Scene Buffer Access

The reflection pass needs to write to `cameras[0]` mid-frame. Currently,
`VulkanResourceManager` does not expose the mapped scene buffer pointer.

**Add to VulkanResourceManager.h:**

```c
/// Returns the persistently-mapped scene buffer for the current flight frame.
/// Used by the reflection pass to swap camera data mid-frame.
void* vulkanResourceGetSceneBufferPtr(void);
```

**Implementation:**

```c
void* vulkanResourceGetSceneBufferPtr(void) {
    return sceneBuffer[renderer.flightIndex].vmaInfo.pMappedData;
}
```

## Frame Resources Changes

In `VulkanFrameResources.c`:

**Replace:**

- `ssprTemp` → `reflectionColor` (half-res: `width/2 × height/2`, `R16G16B16A16_SFLOAT`)
- `ssprOutput` → keep name but repurpose, OR just rename to `reflectionColor`

**Add:**

- `reflectionDepth` (half-res: `width/2 × height/2`, `D32_SFLOAT`)

**Update getters:**

- `vulkanFrameResourcesGetSSPRTemp()` → remove
- `vulkanFrameResourcesGetSSPROutput()` → replace with `vulkanFrameResourcesGetReflectionColor()`
- Add `vulkanFrameResourcesGetReflectionDepth()`

## Files to Create

| File                                                                          | Purpose                      |
| ----------------------------------------------------------------------------- | ---------------------------- |
| `c-engine/renderer/vulkan2/pass/reflection/VulkanReflectionPass.h`            | Header                       |
| `c-engine/renderer/vulkan2/pass/reflection/VulkanReflectionPass.c`            | Implementation               |
| `c-engine/data/pak_0_engine/shaders/pass/reflection/meshlet_reflection.frag`  | Simplified meshlet fragment  |
| `c-engine/data/pak_0_engine/shaders/pass/reflection/triangle_reflection.frag` | Simplified triangle fragment |

## Files to Modify

| File                                                               | Change                                                                                                                                                     |
| ------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `c-engine/renderer/vulkan2/Vulkan.c`                               | Replace `addPass(&vulkanSSPRPass)` with `addPass(&vulkanReflectionPass)` at the **beginning** of the pass list (before GeometryCulling). Remove SSPR pass. |
| `c-engine/renderer/vulkan2/resources/VulkanResourceManager.h`      | Add `vulkanResourceGetSceneBufferPtr()`                                                                                                                    |
| `c-engine/renderer/vulkan2/resources/VulkanResourceManager.c`      | Implement `vulkanResourceGetSceneBufferPtr()`                                                                                                              |
| `c-engine/renderer/vulkan2/resources/VulkanFrameResources.h`       | Replace SSPR getters with reflection getters                                                                                                               |
| `c-engine/renderer/vulkan2/resources/VulkanFrameResources.c`       | Replace SSPR images with half-res reflection images                                                                                                        |
| `c-engine/renderer/vulkan2/pass/composite/VulkanCompositePass.c`   | Update push constants for reflection texture                                                                                                               |
| `c-engine/data/pak_0_engine/shaders/pass/composite/composite.comp` | Replace SSPR lookup with mirror-VP-based reflection texture sampling                                                                                       |
| `c-engine/data/pak_0_engine/shaders/pass/meshlet/meshlet.frag`     | Add `#ifdef REFLECTION_PASS` guards around shadow/AO/forward+ code                                                                                         |
| `c-engine/data/pak_0_engine/shaders/pass/triangle/triangle.frag`   | Same guards                                                                                                                                                |
| `CMakeLists.txt`                                                   | Add new source files if not auto-globbed                                                                                                                   |
| `scripts/build.sh` or shader compilation                           | Add new SPV compilation targets                                                                                                                            |

## Files to Delete

| File                                                              | Reason           |
| ----------------------------------------------------------------- | ---------------- |
| `c-engine/renderer/vulkan2/pass/sspr/VulkanSSPRPass.h`            | Replaced         |
| `c-engine/renderer/vulkan2/pass/sspr/VulkanSSPRPass.c`            | Replaced         |
| `c-engine/data/pak_0_engine/shaders/pass/sspr/sspr_project.comp`  | No longer needed |
| `c-engine/data/pak_0_engine/shaders/pass/sspr/sspr_resolve.comp`  | No longer needed |
| `c-engine/data/pak_0_engine/shaders/pass/sspr/sspr_holefill.comp` | No longer needed |

## Memory Impact

**Removed:**

- SSPR atomic buffer: `width * height * 4` bytes
- SSPRTemp image: `width * height * 8` bytes (RGBA16F)
- SSPROutput image: `width * height * 8` bytes (RGBA16F)
- Total removed: `~20 bytes/pixel` at full res

**Added:**

- Reflection color: `(w/2) * (h/2) * 8` bytes (RGBA16F) = 1/4 of full-res
- Reflection depth: `(w/2) * (h/2) * 4` bytes (D32F) = 1/4 of full-res
- Total added: `~3 bytes/pixel` at full res equivalent

**Net: significant memory savings** (~17 bytes/pixel less at 1080p ≈ 35 MB saved).

## Performance Impact

**Removed:** 3 full-screen compute dispatches (project + resolve + holefill)

**Added:** Half-res scene render:

- Culling compute dispatch (1/4 pixel count, but same instance count)
- Depth prepass at half res (1/4 fragments)
- Lit render at half res with simplified shading (1/4 fragments, no shadows/AO)
- Skybox at half res (1/4 fragments)

Expected cost: roughly **30-50% of main scene render cost** due to half-res +
simplified shading. For scenes with many draw calls, the culling overhead is
similar to the main pass. For simple scenes this should be faster than SSPR.

## GPU Synchronization Notes

The reflection pass runs on the **same command buffer** as the main pipeline.
Commands are serialized within a command buffer, so:

1. Reflection culling reads `cameras[0]` (mirrored) → writes indirect buffers
2. Barrier: compute → indirect draw
3. Reflection depth/render reads `cameras[0]` (mirrored) + indirect buffers
4. Reflection render completes, all GPU work for reflection is submitted
5. CPU writes real camera to `cameras[0]` (mapped coherent memory)
6. Main culling reads `cameras[0]` (real) → overwrites indirect buffers

**Key concern:** Step 5 happens during command buffer recording (CPU-side),
but the GPU hasn't executed steps 1-4 yet. The write to mapped memory is
immediately visible to the GPU because the buffer is `HOST_COHERENT`. This
means the GPU might see the real camera when executing reflection commands.

**Solution:** Use a **second scene buffer** or write the mirrored camera via a
staging `vkCmdUpdateBuffer` command instead of a direct CPU write. This ensures
the mirrored camera data is part of the command stream.

### Revised approach: `vkCmdUpdateBuffer`

```c
// Before reflection culling:
CameraUbo mirrorCam = buildMirroredCamera(...);
vkCmdUpdateBuffer(cmd, sceneBuffer.buf,
                  offsetof(VulkanSceneBuffer, cameras[0]),
                  sizeof(CameraUbo), &mirrorCam);
// Barrier: transfer → compute/vertex
...
// Reflection passes run with mirrored camera on GPU

// After reflection render, before main culling:
vkCmdUpdateBuffer(cmd, sceneBuffer.buf,
                  offsetof(VulkanSceneBuffer, cameras[0]),
                  sizeof(CameraUbo), &realCamera);
// Barrier: transfer → compute/vertex
...
// Main passes run with real camera on GPU
```

This is the **correct** approach. `vkCmdUpdateBuffer` inserts the data into the
command stream, so it executes in order with the GPU commands. The buffer must
have `VK_BUFFER_USAGE_TRANSFER_DST_BIT` (check and add if missing).

**Important:** `vkCmdUpdateBuffer` has a max size of 65536 bytes.
`sizeof(CameraUbo)` is ~752 bytes, well within the limit.

## Implementation Steps

1. **Add `VK_BUFFER_USAGE_TRANSFER_DST_BIT`** to scene buffer creation in
   `VulkanResourceManager.c`

2. **Create reflection frame resources** — replace SSPR images with half-res
   reflection color + depth in `VulkanFrameResources.c`

3. **Create reflection fragment shaders** — copy `meshlet.frag`/`triangle.frag`,
   add `#ifdef REFLECTION_PASS` guards, compile as separate SPVs

4. **Create `VulkanReflectionPass`** — the main new pass that orchestrates
   mirrored camera, culling, depth, render, skybox

5. **Modify composite shader** — replace SSPR lookup with projection into
   reflection texture via `cameras[1]` (mirrored VP)

6. **Update `Vulkan.c`** — register reflection pass first, remove SSPR pass

7. **Delete SSPR files** — shaders and C source

8. **Update CMakeLists.txt** — add new source files

9. **Update shader compilation** — add new SPV targets

10. **Test and iterate**

## Open Questions

1. **Should the reflection include point/spot lights?** Initially no (simplify).
   Can add later by running light culling for the mirrored frustum.

2. **Roughness-based blur?** A polished floor should have sharp reflections,
   a rough floor should have blurry ones. We could mipmap the reflection texture
   and sample a higher mip for rougher surfaces. Future enhancement.

3. **Multiple reflection planes?** Currently hardcoded to y=0. The system
   generalizes to any plane, but each plane costs a full reflection render.
   Keep single plane for now.

4. **Reflection of transparent objects?** Skipped initially (no OIT in
   reflection pass). Can add later.
