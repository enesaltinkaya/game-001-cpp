# Vegetation System Review

## Architecture Summary

The system has a clean multi-layer design:

1. **VegetationMap** — CPU-side terrain triangle bucketing + grass tile mask (load time)
2. **Terrain fragment shader** → writes a `vegInfoMap` (R32 storage image, packs height + eligibility)
3. **Compute scatter/cull** (`vegetation_scatter_cull.comp`) — per-1m-tile, 120 blades/tile, reads vegInfoMap, writes surviving instances + atomic indirect count
4. **Depth prepass** → grass depth + velocity + view normals (for GTAO, motion blur, contact shadows)
5. **Color pass** → full shading with shadows, forward+ lights, IBL, subsurface, wind, player interaction
6. **Shadow pass** → separate vertex shader for cascaded shadow maps

The GPU-driven indirect approach is solid. The vegInfoMap written by terrain is a clever way to get pixel-accurate height + eligibility without uploading triangle data to the compute shader.

---

## Bugs

### 1. `postUpdate` CPU timing is broken
```c
static void postUpdate(void) {
    vulkanVegetationPass.cpuElapsed = elapsedCPU;
    vulkanVegetationPass.gpuElapsed = elapsedGPU;
    elapsedCPU = nanos();
    elapsedCPU = nanos() - elapsedCPU;  // ← measures ~0ns (delta of two consecutive calls)
}
```
This reports the time between two back-to-back `nanos()` calls (~tens of nanoseconds), not the actual CPU time of the pass. You'd need to capture a start timestamp at the beginning of `preUpdate`/`update` and subtract here.

### 2. Tile cache doesn't invalidate on camera rotation
```c
if (!visibleTileCacheValid || cachedCameraTileX != cameraTileX || cachedCameraTileZ != cameraTileZ) {
```
The cache only rebuilds when the camera crosses an integer tile boundary. If the player stands still and rotates the camera, the cached tile list is stale — tiles entering the frustum from the side won't appear, and tiles now behind the camera continue to be processed. This can cause visible popping and wasted GPU work. You should also invalidate on camera direction change (e.g. store + compare a quantized forward direction, or just rebuild every frame since the CPU cost of iterating ~280K offsets with cheap checks is low).

### 3. Shadow shader missing player interaction
`shadow_vegetation.vert` doesn't apply the player push displacement, but `vegetation.vert` and `vegetation_depth.vert` do. When the player walks through grass, the blades bend in the color/depth passes but their shadows remain straight — creating visually incorrect shadow artifacts.

### 4. `terrainSubmitCount` is never used
`VegetationMap` has `terrainSubmitCount` documented as "detect accidental silent overwrites" but it's never incremented or checked in `vegetationMapSubmit()`.

---

## Dead / Obsolete Code

### 5. The entire `VegTriangleGPU` tile bucketing system is dead code
`VegetationMap` stores `VegTileBucket tiles[100]` with `VegTriangleGPU` data (positions, normals, areas, global indices), built during `vegetationMapSubmit()`. However the compute scatter shader **never reads these**. It reads only the `vegInfoMap` (written by the terrain fragment shader) and per-1m-tile coordinates. The triangle bucketing was apparently the original design but has been entirely superseded by the vegInfoMap approach. This is a significant amount of dead code and wasted load-time memory:

- `VegTriangleGPU` struct (64 bytes/tri)
- `VegTileBucket` with `gpuTriangles`, `gpuTriangleCount`, `globalTriOffset`
- The entire Phase 1-2-3 bucketing in `vegetationMapSubmit()` (the temp TempTri arrays, triangle processing, etc.)

Only the **grass tile mask** part of `vegetationMapSubmit()` is actually consumed at runtime (by `vegetationMapTileMayContainGrass()`). The triangle data could be stripped.

---

## Performance Concerns

### 6. Massive GPU buffer allocations (~576 MB)
```c
#define VEG_MAX_VISIBLE_INSTANCES (4 * 1024 * 1024)
```
- `culledInstanceBuffer`: 4M × 36 bytes = **144 MB** × 2 flight frames = 288 MB
- `instanceStagingBuffer`: same = 288 MB (used only for baked instances, which are usually sparse)
- **Total: ~576 MB** just for vegetation instance buffers

At 300m distance with 120 blades/tile and π×300² ≈ 282K tiles, the theoretical max is ~33M instances. The 4M cap means <13% utilization at full density, so the cap is reasonable — but the staging buffers could be much smaller since baked instance counts are typically far below 4M.

### 7. Redundant image load in compute shader
The early path loads `packedNearest` for eligibility, then later loads all 4 bilinear corners including the same nearest texel again. This is 5 image loads where 4 suffice. You could reuse the nearest-texel load as one of the bilinear corners.

### 8. Compute shader: `cos()`/`sin()` per blade for clumpDensity noise
`noiseCell()` is called twice (two octaves) per surviving blade, each doing 4 hash evaluations. This is cheap relative to the image loads, but the value noise `floor`/`fract`/hash pattern could be replaced by a small precomputed noise texture for better cache behavior at very high instance counts.

---

## Code Quality / Maintainability

### 9. Vertex shader logic duplicated 3×
The V-fold, rotation, curvature, wind, and distance-fade logic is copied verbatim across `vegetation.vert`, `vegetation_depth.vert`, and `shadow_vegetation.vert`. This is the root cause of bug #3 (shadow missing player interaction) and makes any future change error-prone. Consider extracting the common blade positioning into an `#include`d function.

### 10. Hardcoded wind parameters
Wind direction `vec2(0.8, 0.3)`, speeds `1.5`/`0.4`, and interaction radius `1.2` are hardcoded across all shaders and C push constants. These should be uniforms for artist control and to avoid inconsistency.

### 11. `channelType` in fragment shader is unused
`inChannelType` is passed through as a flat varying but never read in `vegetation.frag`. Makes sense since only grass is implemented, but it's dead plumbing.

---

## Correctness (Verified OK)

- **Camera-relative rendering** is handled correctly across all passes (subtract `worldOrigin`, use `prevWorldOrigin` for velocity).
- **Frustum plane extraction** in the compute shader uses Gribb-Hartmann correctly on the camera-relative VP matrix.
- **Normal derivation** from bilinear height differences is mathematically correct.
- **VegInstance struct layout** (36 bytes) matches between C and GLSL std430 — the `u8 channel/type/pad[2]` maps to `uint channelType` correctly on little-endian.
- **Transfer → compute → draw barriers** are correct in terms of stage and access masks.
- **vegInfoMap race condition** is properly guarded: the barrier before compute dispatch waits on `FRAGMENT_SHADER_BIT → COMPUTE_SHADER_BIT`.
- **Double-sided lighting** with normal flip + subsurface is a nice touch for thin grass blades.

---

## Summary of Recommended Actions

| Priority | Issue | Action |
|----------|-------|--------|
| **High** | #2 Tile cache stale on rotation | Invalidate on direction change or rebuild every frame |
| **High** | #3 Shadow missing player push | Add player interaction to shadow shader (or extract shared include) |
| **High** | #9 Duplicated vertex shader code | Extract common blade positioning into `vegetation_common.glsl` |
| **Medium** | #1 Broken CPU timing | Fix `postUpdate` to measure actual pass duration |
| **Medium** | #5 Dead triangle bucketing | Remove `VegTriangleGPU`/`VegTileBucket` and related code |
| **Medium** | #6 Oversized staging buffers | Size `instanceStagingBuffer` based on actual baked counts |
| **Low** | #7 Redundant image load | Reuse nearest texel in bilinear sampling |
| **Low** | #10 Hardcoded wind params | Move to push constants / scene buffer |
| **Low** | #4 Unused `terrainSubmitCount` | Either implement or remove |

Overall this is a well-engineered system with a clean GPU-driven architecture. The main concerns are the shader duplication leading to inconsistencies and the dead triangle bucketing code that should be cleaned up.
