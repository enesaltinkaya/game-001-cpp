# Two-Phase Occlusion Culling

## Problem
When the camera moves, meshlet objects show holes because Phase 1 culling uses the previous frame's HiZ depth buffer. Meshlets that were occluded last frame but are now visible get falsely culled. When the camera stops, prev frame ≈ current frame, so it looks correct.

## Solution: Two-Phase Occlusion Culling

After Phase 1 renders depth, build an **early HiZ** from it, then re-test Phase 1's occlusion rejects against the fresh depth. Anything that passes Phase 2 gets rendered too — no more holes.

## Current Pass Order
```
1.  geometry_culling   (Phase 1: frustum + backface + prev HiZ)
2.  shadow
3.  depth              (render Phase 1 survivors)
4.  light_culling
5.  SSS
6.  AO
7.  meshlet_render     (draw Phase 1 survivors)
8.  triangle_render    (draw Phase 1 survivors)
9.  skybox
10. OIT accumulate/composite
11. HiZ               (build from depth → becomes next frame's "prev HiZ")
12. SSPR, composite, TAA, bloom, final, rmlui
```

## New Pass Order
```
1.  geometry_culling   (Phase 1: frustum + backface + prev HiZ → also writes occlusion rejects)
2.  shadow
3.  depth              (render Phase 1 survivors)
4.  phase2_occlusion   ← NEW (build early HiZ → Phase 2 cull rejects → Phase 2 depth)
5.  light_culling
6.  SSS
7.  AO
8.  meshlet_render     (draw Phase 1 + Phase 2 survivors)
9.  triangle_render    (draw Phase 1 + Phase 2 survivors)
10. skybox
11. OIT accumulate/composite
12. HiZ               (unchanged — builds final HiZ for next frame)
13. SSPR, composite, TAA, bloom, final, rmlui
```

## Design: Visibility Flag Buffer

Instead of a separate "rejected list" buffer with atomic appends, use a **per-instance u32 flag buffer**:
- `0` = culled by frustum or backface (don't retry)
- `1` = visible (passed all tests, drawn in Phase 1)
- `2` = rejected by occlusion only (retry in Phase 2)

Phase 2 shader dispatches over ALL instances again but only processes flag == 2.

This avoids an extra atomic counter + indirection and keeps both shaders simple.

## Files to Change

### 1. VulkanScene.h — Add Phase 2 + visibility buffers
```c
// Per-instance visibility flags (0=frustum/backface culled, 1=visible, 2=occlusion rejected)
VulkanBuffer visibilityBuffer[FRAMES_IN_FLIGHT];

// Phase 2 meshlet output buffers
VulkanBuffer phase2IndirectBuffer[FRAMES_IN_FLIGHT];
VulkanBuffer phase2DrawCountBuffer[FRAMES_IN_FLIGHT];
VulkanBuffer phase2CulledMeshletsBuffer[FRAMES_IN_FLIGHT];

// Phase 2 triangle output buffers (opaque)
VulkanBuffer phase2TriangleIndirectBuffer[FRAMES_IN_FLIGHT];
VulkanBuffer phase2TriangleDrawCountBuffer[FRAMES_IN_FLIGHT];
VulkanBuffer phase2TriangleCulledBuffer[FRAMES_IN_FLIGHT];

// Phase 2 triangle output buffers (double-sided)
VulkanBuffer phase2TriangleDSIndirectBuffer[FRAMES_IN_FLIGHT];
VulkanBuffer phase2TriangleDSDrawCountBuffer[FRAMES_IN_FLIGHT];
VulkanBuffer phase2TriangleDSCulledBuffer[FRAMES_IN_FLIGHT];

// Per-instance triangle visibility flags
VulkanBuffer triangleVisibilityBuffer[FRAMES_IN_FLIGHT];
```

### 2. VulkanScene.c — Allocate/free Phase 2 buffers
- In `vulkanSceneCreate`: allocate all new buffers (same sizes as Phase 1 counterparts)
- In `vulkanSceneDestroy`: free them

### 3. Shaders — Phase 1 modifications

**shaders/pass/meshlet/culling.comp** — Add to push constants:
```glsl
uint64_t visibilityBufferAddress;
```
Push constant total: 96 + 8 = 104 bytes (under 128 limit).

Change occlusion reject from `return` to:
```glsl
visibilityBuf.flags[instanceIndex] = 2u;  // occlusion rejected
return;
```
Write `0` for frustum/backface culled, `1` for visible.

**shaders/pass/triangle/triangle_culling.comp** — Same pattern: add visibility buffer, write flags.

### 4. Shaders — Phase 2 (new)

**shaders/pass/meshlet/culling_phase2.comp**:
```glsl
// Push constants:
// meshletBufferAddress, meshletInstanceBufferAddress,
// visibilityBufferAddress, indirectBufferAddress,
// culledMeshletsBufferAddress, drawCountBufferAddress,
// transformBufferAddress, maxMeshletInstances, hizTextureIndex

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= maxMeshletInstances) return;
    if (visibilityBuf.flags[idx] != 2u) return;  // only retry occlusion rejects

    // HiZ test using CURRENT viewProjection + early HiZ
    // (no frustum/backface — already passed in Phase 1)
    if (hizCull(worldCenter, worldRadius)) return;  // still occluded

    // Passed — append to Phase 2 draw buffers
    uint visibleIndex = atomicAdd(drawCount, 1);
    culledBuf[visibleIndex] = idx;
    indirectBuf[visibleIndex] = DrawCommand(triCount*3, 1, 0, 0);
}
```

**shaders/pass/triangle/triangle_culling_phase2.comp** — Same pattern for triangles. Classifies into opaque / double-sided / transparent Phase 2 buckets.

### 5. VulkanMeshletCullingPass.c — Update push constants
- Add `visibilityBufferAddress` to `MeshletPushConstants` struct
- Pass `vs->visibilityBuffer[flightIndex].address` in the PC
- Same for `TriangleCullingPushConstants` + triangle visibility

### 6. New: VulkanPhase2OcclusionPass.c/.h
Single new pass that does three things in `update()`:

**Step A — Build early HiZ:**
- Transition depth to shader read
- Copy depth → early HiZ mip 0 (reuse existing `hiz_copy_depth.comp`)
- Downsample mip chain (reuse existing `hiz_downsample.comp`)
- Transition depth back to attachment
- Uses a dedicated `earlyHiZImage` (single image, not double-buffered)

**Step B — Phase 2 meshlet culling:**
- Reset Phase 2 draw counts to 0
- Barrier
- Dispatch `culling_phase2.comp` over all meshlet instances
- Same for `triangle_culling_phase2.comp`
- Barrier compute → indirect draw

**Step C — Phase 2 depth rendering:**
- Begin render pass on same depth image + velocity image (NO clear)
- Draw Phase 2 meshlet survivors via `vkCmdDrawIndirectCount` using Phase 2 buffers
- Draw Phase 2 triangle survivors (opaque + double-sided)
- End render pass
- Depth barrier

Needs its own pipes:
- `phase2_meshlet_depth` — same shaders as `meshlet_depth_prepass` but no clears
- `phase2_triangle_depth` — same shaders as `triangle_depth` but no clears
- `phase2_triangle_depth_double_sided` — same but noCull

### 7. VulkanHiZPass.c/.h — Expose helper or keep self-contained
Option A: Factor out HiZ generation into a reusable function.
Option B: Phase 2 pass has its own copy of the HiZ build logic with its own earlyHiZ image.

**Recommend Option B** — keeps passes independent, avoids coupling. The early HiZ image is simpler (single image, no double-buffering). The copy-depth + downsample shaders are already compiled; the Phase 2 pass just creates its own pipes pointing to the same SPV files.

### 8. VulkanMeshletRenderPass.c — Draw Phase 2 survivors
After drawing Phase 1 meshlets, also draw Phase 2:
```c
// Phase 2 meshlet draws
for (each scene) {
    // same push constants but with phase2 buffers
    vkCmdDrawIndirectCount(cmd, vs->phase2IndirectBuffer[fi], 0,
                           vs->phase2DrawCountBuffer[fi], 0,
                           vs->totalMeshletInstanceCount, sizeof(DrawCommand));
}
```

### 9. VulkanTriangleRenderPass.c — Draw Phase 2 survivors
Same pattern: after Phase 1 draws, also issue Phase 2 draws for opaque and double-sided triangles.

### 10. VulkanDepthPass.h — Expose depth/velocity images
Already exposed via `vulkanDepthPassGetDepthImage()` and `vulkanDepthPassGetVelocityImage()`. No changes needed.

### 11. Vulkan.c — Insert new pass
```c
addPass(&vulkanGeometryCullingPass);
addPass(&vulkanShadowPass);
addPass(&vulkanDepthPass);
addPass(&vulkanPhase2OcclusionPass);   // ← NEW
addPass(&vulkanLightCullingPass);
// ... rest unchanged ...
```

### 12. Shadow pass
**Verified**: Shadow pass uses Phase 1's `indirectBuffer`/`culledMeshletsBuffer`/`drawCountBuffer`. Phase 2 survivors will NOT cast shadows. This is acceptable for now — Phase 2 typically has very few meshlets (only newly-disoccluded ones near edges), so missing shadows are negligible. Can be addressed later by also drawing Phase 2 into shadow maps if needed.

## Push Constant Sizes (verify < 128 bytes)

| Struct | Current | +visibility | Total |
|--------|---------|-------------|-------|
| MeshletPushConstants | 96 B | +8 B | 104 B ✓ |
| TriangleCullingPushConstants | 104 B | +8 B | 112 B ✓ |
| Phase2 meshlet PC | — | — | 72 B ✓ |
| Phase2 triangle PC | — | — | ~96 B ✓ |

## Performance Notes
- Phase 2 dispatch covers ALL instances but early-outs for flag != 2 — minimal overhead when nothing is occlusion-rejected
- Early HiZ build is the main cost: same as final HiZ (copy + downsample chain)
- Phase 2 depth rendering is typically very few draws (only newly-disoccluded meshlets)
- Net GPU cost: ~1 extra HiZ build + 2 compute dispatches + small draw call per frame

## Validation
1. `./scripts/build.sh` (compiles code + shaders)
2. `./scripts/run.sh` — move camera rapidly, verify no holes
3. Check RenderDoc: Phase 2 draws should be non-zero when camera moves, near-zero when static
