# SDFGI Implementation Plan

## Reference Source

Godot 4 source code used as reference: `/home/enes/temp/godot`

Key files:

- **SDFGI C++ implementation:** `servers/rendering/renderer_rd/environment/gi.h` and `gi.cpp` (~4300 lines)
- **Shaders:** `servers/rendering/renderer_rd/shaders/environment/sdfgi_*.glsl`
- **Screen-space GI sampling:** `servers/rendering/renderer_rd/shaders/environment/gi.glsl`

---

## Overview

Implement **Signed Distance Field Global Illumination (SDFGI)** — a fully dynamic, real-time GI system
inspired by Godot 4's implementation. SDFGI provides multi-bounce indirect lighting, ambient light,
and reflections without pre-baking.

---

## How SDFGI Works (Godot Reference)

SDFGI is a cascaded voxel-based GI system with these core stages:

### 1. Voxelization (SDF Generation)

- Scene geometry is rendered into a 3D voxel grid (128³ per cascade, 4–8 cascades).
- Each cascade covers a progressively larger area (2× scale per level).
- A **jump-flood algorithm** converts solid voxels into a signed distance field.
- Albedo color is stored per voxel (R5G5B5 packed).

### 2. Direct Lighting

- For each solid voxel, direct light contributions are computed by **raymarching through the SDF**.
- Supports directional, omni, and spot lights.
- Light is stored anisotropically (6 directions) for accurate indirect bounce.

### 3. Probe Integration (Light Probes)

- Light probes are placed on a regular 3D grid (every 16 voxels → `PROBE_DIVISOR = 16`).
- Each probe traces rays through the SDF cascades using **Vogel hemisphere sampling**.
- Hits sample the anisotropic direct light volume; misses sample the sky.
- Results are accumulated into **spherical harmonics (L1 or L2)** with temporal history.
- SH is then converted to **octahedral lightprobe maps** for efficient GPU sampling.

### 4. Occlusion (Optional)

- Per-probe visibility/occlusion is computed for each of 8 octants.
- Used to prevent light bleeding through walls.

### 5. Screen-Space Sampling (GI Pass)

- A full-screen compute shader samples the lightprobe array using the pixel's world position and normal.
- Provides **diffuse ambient** and **specular reflection** via cone-traced SDF raymarching.
- Cascades blend smoothly; outer probes fall back to parent cascade data.

### 6. Dynamic Updates

- As the camera moves, cascades **scroll** to follow the player.
- Only dirty regions are re-voxelized (partial updates).
- Dynamic objects are re-lit every frame; static lighting is updated incrementally.

---

## Architecture Mapping: Godot → Our Engine

| Godot Concept                | Our Engine Equivalent                                                    |
| ---------------------------- | ------------------------------------------------------------------------ |
| `GI::SDFGI` class            | New `VulkanSdfgiPass` (System) + `Sdfgi` state struct                    |
| `gi.cpp` (4300 lines)        | Split into multiple C files under `c-engine/renderer/vulkan/pass/sdfgi/` |
| `sdfgi_preprocess.glsl`      | `shaders/pass/sdfgi/sdfgi_preprocess.comp`                               |
| `sdfgi_direct_light.glsl`    | `shaders/pass/sdfgi/sdfgi_direct_light.comp`                             |
| `sdfgi_integrate.glsl`       | `shaders/pass/sdfgi/sdfgi_integrate.comp`                                |
| `sdfgi_debug.glsl`           | `shaders/pass/sdfgi/sdfgi_debug.comp`                                    |
| `sdfgi_debug_probes.glsl`    | `shaders/pass/sdfgi/sdfgi_debug_probes.vert/frag`                        |
| `gi.glsl` (screen sampling)  | `shaders/pass/sdfgi/sdfgi_sample.comp`                                   |
| RenderingDevice (RD)         | Vulkan API directly (already used throughout engine)                     |
| RID-based texture management | `VulkanImage` + `VulkanResourceManager`                                  |

---

## Implementation Phases

### Phase 0: Infrastructure & Scaffolding

**Goal:** Create the pass skeleton, resource types, and shader compilation pipeline.

1. **Create directory structure:**

   ```
   c-engine/renderer/vulkan/pass/sdfgi/
       VulkanSdfgiPass.h
       VulkanSdfgiPass.c          (SDFGI lifecycle: create, update, destroy)
       VulkanSdfgiResources.h     (cascade/probe GPU resource structs)
       VulkanSdfgiVoxelization.c  (render-to-3D-texture voxelization)
       VulkanSdfgiLighting.c      (direct lighting dispatch)
       VulkanSdfgiProbes.c        (probe integration + SH → octahedral)
       VulkanSdfgiSampling.c      (full-screen GI sampling)
   ```

2. **Create shader directory:**

   ```
   c-engine/data/pak_0_engine/shaders/pass/sdfgi/
       sdfgi_preprocess.comp       (jump-flood SDF generation)
       sdfgi_direct_light.comp     (per-voxel direct lighting)
       sdfgi_integrate.comp        (probe ray-tracing + SH accumulation)
       sdfgi_store.comp            (SH → octahedral lightprobe conversion)
       sdfgi_scroll.comp           (cascade scrolling)
       sdfgi_sample.comp           (screen-space GI sampling)
       sdfgi_debug.comp            (debug visualization)
       spv/                        (compiled SPIR-V)
   ```

3. **Create resource structs:**

   ```c
   // VulkanSdfgiResources.h
   #define SDFGI_MAX_CASCADES    8
   #define SDFGI_CASCADE_SIZE    128
   #define SDFGI_PROBE_DIVISOR   16
   #define SDFGI_LIGHTPROBE_OCT  6
   #define SDFGI_SH_SIZE         16

   typedef struct SdfgiCascade {
       VulkanImage sdf;              // R8_UNORM 128³ — signed distance field
       VulkanImage light;            // R32_UINT (RGBE9995) 128³ — direct light
       VulkanImage lightAniso0;     // RGBA8_UNORM 128³ — anisotropic light 0-3
       VulkanImage lightAniso1;     // RG8_UNORM 128³ — anisotropic light 4-5

       VulkanBuffer solidCellBuffer; // SSBO of solid voxels for indirect dispatch
       VulkanBuffer dispatchBuffer;  // indirect dispatch args

       float cellSize;               // world-space size of one voxel
       vec3i position;               // world-space offset (in cells)
       vec3i dirtyRegions;           // which sub-regions need re-voxelization
   } SdfgiCascade;

   typedef struct Sdfgi {
       SdfgiCascade cascades[SDFGI_MAX_CASCADES];
       int cascadeCount;

       // Lightprobe volumes
       VulkanImage lightprobeData;    // R32_UINT 2DArray (octahedral irradiance)
       VulkanImage lightprobeHistory; // RGBA16_SINT 2DArray (SH history ring)
       VulkanImage lightprobeAverage; // RGBA32_SINT 2D (running SH average)

       // Occlusion
       VulkanImage occlusion;         // R16_UINT 3D

       // Render intermediates (voxelization)
       VulkanImage renderAlbedo;
       VulkanImage renderSdf[2];      // ping-pong for jump flood
       VulkanImage renderSdfHalf[2];

       // UBOs
       VulkanBuffer cascadesUbo;      // cascade transforms

       // Config
       float minCellSize;
       int probeAxisCount;
       uint32_t historySize;
       uint32_t solidCellCount;
       float yMult;
       char usesOcclusion;

       // Pipelines
       VulkanPipe preprocessPipe;     // jump flood variants
       VulkanPipe directLightPipe;    // static + dynamic modes
       VulkanPipe integratePipe;      // process + store + scroll
       VulkanPipe samplePipe;         // screen-space sampling
   } Sdfgi;
   ```

4. **Add CMake entries** for new source files in `c-engine/CMakeLists.txt`.

5. **Update shader build script** (`scripts/build.sh`) to compile `sdfgi/*.comp` → SPIR-V.

**Estimated effort:** 2–3 days

---

### Phase 1: SDF Voxelization & Jump Flood

**Goal:** Voxelize scene geometry into cascaded SDF volumes.

1. **Implement cascade creation** (`SdfgiCascade` init with 128³ textures).
2. **Voxelization pass:**
   - Render scene geometry into a 128³ 3D texture using **geometry shader or multi-layer rendering**.
   - Store albedo (R5G5B5) and solidity bit into `renderAlbedo` (R16_UINT).
   - Alternative (simpler): Use **software voxelization** in a compute shader — project each triangle into the 3D grid using axis-aligned rasterization.
3. **Jump-flood SDF generation** (`sdfgi_preprocess.comp`):
   - **Initialize:** mark solid voxels vs empty, write position to flood buffer.
   - **Jump flood:** iteratively propagate closest-solid position (1→2→4→...→64 steps).
   - **Half-resolution variant** for faster initial passes.
   - **Store final:** convert flood positions to distance values (R8 UNORM SDF).

4. **Cascade scrolling:**
   - When camera moves past a threshold, scroll cascade data by the delta.
   - Mark dirty regions for re-voxelization.

**Key design decisions:**

- Use compute-based voxelization (no geometry shader) to match the engine's compute-first approach.
- Cascade size 128³ is a good default (adjustable).

**Estimated effort:** 1–2 weeks

---

### Phase 2: Direct Lighting

**Goal:** Compute per-voxel direct lighting with SDF raymarching.

1. **Light upload:** pack directional/point/spot lights into a storage buffer (max 1024 static, 128 dynamic).
2. **`sdfgi_direct_light.comp` shader:**
   - For each solid voxel (via indirect dispatch using `solidCellBuffer`).
   - For each light: raymarch through SDF cascades to test visibility.
   - Accumulate light anisotropically (6 directions) based on normal facing bits.
   - Store as RGBE9995 in `light` texture + anisotropy in `lightAniso0/1`.
3. **Static vs dynamic split:**
   - Static lights update incrementally (spread across frames).
   - Dynamic lights update every frame (only for affected voxels).
4. **Bounce feedback:** read existing indirect light from probes and add to direct contribution.

**Estimated effort:** 1 week

---

### Phase 3: Light Probe Integration

**Goal:** Trace rays from probe positions through SDF, accumulate SH, convert to octahedral maps.

1. **Probe grid layout:** one probe every `SDFGI_PROBE_DIVISOR` (16) voxels → 9×9×9 probes per cascade.
2. **`sdfgi_integrate.comp` (PROCESS mode):**
   - For each probe, cast `rayCount` rays (Vogel hemisphere, 8–32 rays).
   - Raymarch through SDF cascades.
   - On hit: sample anisotropic direct light + normal.
   - On miss: sample sky color.
   - Accumulate into spherical harmonics (L2, 16 coefficients × 3 channels).
3. **Temporal accumulation:**
   - Store per-frame SH in a history ring buffer (`lightprobeHistory`, N frames).
   - Maintain running average (`lightprobeAverage`).
   - Converges over 10–60 frames.
4. **`sdfgi_integrate.comp` (STORE mode):**
   - Convert SH to octahedral irradiance map (6×6 texels per probe face).
   - Store as RGBE9995 in `lightprobeData` 2DArray.
5. **Probe scrolling:** when cascades scroll, scroll probe history data too; new probes interpolate from parent cascade.

**Estimated effort:** 1–2 weeks

---

### Phase 4: Occlusion (Optional Enhancement)

**Goal:** Compute per-probe directional occlusion to prevent light bleeding.

1. For each probe, compute occlusion in 8 octants by checking solid voxels in the surrounding region.
2. Store as R16_UINT packed 4-bit-per-octant values in `occlusion` 3D texture.
3. During screen-space sampling, use occlusion to weight probe contributions.

**Estimated effort:** 3–5 days

---

### Phase 5: Screen-Space GI Sampling

**Goal:** Full-screen compute pass that applies GI to the final image.

1. **`sdfgi_sample.comp`:**
   - Input: depth buffer, normal/roughness, world position.
   - Find which cascade + probe the pixel falls in.
   - Trilinear blend 8 surrounding probes.
   - Sample octahedral lightprobe at the pixel's normal direction → diffuse GI.
   - For specular: cone-trace through SDF or sample specular probe.
   - Apply occlusion weighting.
   - Output: ambient color + reflection color (written to composite pass inputs).

2. **Integration with composite pass:**
   - Add SDFGI output as input to `VulkanCompositePass`.
   - Blend with existing lighting (direct + SSR).
   - Fallback path when SDFGI is disabled.

3. **Cascade blending:**
   - Smooth blend zone between cascade edges.
   - Outermost cascade uses parent data for scrolled probes.

**Estimated effort:** 1 week

---

### Phase 6: Dynamic Updates & Runtime

**Goal:** Make it all run every frame efficiently.

1. **Update loop (per frame):**

   ```
   1. Check if camera moved → scroll cascades if needed
   2. For each cascade with dirty regions:
      a. Re-voxelize dirty region
      b. Jump-flood SDF for dirty region
   3. Update direct lighting (static: partial, dynamic: full)
   4. Integrate probes (spread across frames based on converge setting)
   5. Store probes → octahedral (when new SH data ready)
   6. Screen-space sampling (every frame)
   ```

2. **Frame budget management:**
   - Distribute work across frames (e.g., 1 cascade per frame).
   - Priority queue: closest cascades update first.
   - Configurable quality presets (ray count, converge speed).

3. **Configuration API:**
   ```c
   void rendererSetSdfgiEnabled(char enabled);
   void rendererSetSdfgiCascades(int cascades);       // 4–8
   void rendererSetSdfgiMinCellSize(float size);
   void rendererSetSdfgiRayCount(int rays);            // 8, 16, 32, 64
   void rendererSetSdfgiConvergeFrames(int frames);    // 5, 10, 15, 30, 60
   void rendererSetSdfgiYScale(float scale);           // 0.5, 0.75, 1.0
   void rendererSetSdfgiOcclusion(char enabled);
   ```

**Estimated effort:** 1 week

---

### Phase 7: Debug Visualization & Polish

**Goal:** Debug tools and quality tuning.

1. **Debug visualization:**
   - Visualize SDF voxels as colored cubes.
   - Visualize lightprobe spheres with irradiance colors.
   - Visualize occlusion per probe.
   - Overlay SDFGI-only lighting mode.

2. **Performance optimizations:**
   - Half-resolution GI sampling option.
   - Indirect compute dispatch (only process solid voxels).
   - Async compute overlap with scene rendering.

3. **Quality tuning:**
   - Normal bias / probe bias controls.
   - Energy multiplier.
   - Bounce feedback strength.

**Estimated effort:** 3–5 days

---

## Total Estimated Timeline

| Phase     | Duration       | Description                   |
| --------- | -------------- | ----------------------------- |
| 0         | 2–3 days       | Infrastructure & scaffolding  |
| 1         | 1–2 weeks      | SDF voxelization & jump flood |
| 2         | 1 week         | Direct lighting               |
| 3         | 1–2 weeks      | Light probe integration       |
| 4         | 3–5 days       | Occlusion (optional)          |
| 5         | 1 week         | Screen-space GI sampling      |
| 6         | 1 week         | Dynamic updates & runtime     |
| 7         | 3–5 days       | Debug viz & polish            |
| **Total** | **~6–8 weeks** |                               |

---

## Key Technical Risks

1. **Voxelization performance:** Software voxelization in compute is well-proven but needs careful optimization for 128³ per cascade. May need to start with a simpler approach (render from 3 axes) and optimize later.

2. **Memory:** 8 cascades × 128³ × multiple textures ≈ 500 MB–1 GB VRAM. Need configurable cascade count and half-res options.

3. **Temporal stability:** SH accumulation needs careful handling to avoid flickering. Godot uses a 10-bit fixed-point ring buffer — we should do the same.

4. **Integration with existing lighting:** Must blend with SSR and direct lighting without double-counting. The composite pass needs clear separation of GI vs direct contributions.

5. **Camera-relative rendering:** Our engine uses camera-relative rendering (`worldOrigin` offset). SDFGI world-space data must account for this — the SDFGI cascades should be positioned relative to the camera origin, and the `worldOrigin` offset applied during screen-space sampling.

---

## Recommended Implementation Order

**Start with a minimal viable version:**

1. Single cascade, 64³ (reduced for faster iteration)
2. No scrolling — static position
3. Direct lighting only (no bounce)
4. Simple probe integration (L1 SH, few rays)
5. Screen-space diffuse only (no specular)

**Then iterate:** 6. Add cascades (4) 7. Add scrolling 8. Add bounce feedback 9. Full L2 SH 10. Occlusion 11. Specular/reflections 12. Performance optimizations
