# Renderer Progress & Remaining Work

_Last updated: March 9, 2026_

---

## What Is Done

| Roadmap Section | Notes |
|---|---|
| Frame resource management (SceneColor RT0, Normals RT1, Material RT2, ResolvedColor, HistoryColor, PrevDepth) | `VulkanFrameResources.c` |
| Phase 1a: Jitter — Halton 8-frame cycle, jittered + no-jitter matrices, prev variants | `CameraSystem.c`, `globalset.shader` |
| Phase 1b: Frustum + Hi-Z culling — meshlets **and** triangles | `triangle_culling.comp` has full frustum + Hi-Z; wired in `VulkanMeshletCullingPass.c` |
| Phase 2: Depth pre-pass — depth + pixel-space velocity, correct jittered/non-jittered split | `VulkanDepthPass`, `depth_prepass.vert`, `triangle_depth.vert` |
| Phase 3: Shadow Atlas — CSM 4 cascades, PCF 3×3, D32_SFLOAT, log/linear split | `VulkanShadowPass.c`, `shadow.shader` |
| Phase 4a: Hi-Z generation with current/previous swap | `VulkanHiZPass.c` |
| Phase 4 (bonus): Screen Space Shadows (Bend Studio algorithm) | `VulkanSSSPass.c` |
| Phase 5a (partial): Opaque PBR shading — GGX, Cook-Torrance, CSM shadow sampling, SSS, clearcoat / sheen / transmission / anisotropy extensions | `meshlet.frag`, `triangle.frag` |
| Phase 7b: TSSAA — YCoCg, variance clipping, Catmull-Rom history, depth rejection, edge locking | `taa.comp` |
| Phase 9a: CAS (Contrast Adaptive Sharpening) + Blue Noise Dither in final pass | `final.frag`, `VulkanFinalPass.c` |
| Phase 9b: ACES tonemap (sRGB swapchain attachment handles gamma implicitly) | `final.frag` |
| Phase 9c: UI overlay | `VulkanRmluiPass.c` |
| Phase 4c: Forward+ Lighting — tile culling compute + point/spot PBR in fragment shaders | `VulkanLightCullingPass.c`, `light_culling.comp`, `forwardplus.shader`, `meshlet.frag`, `triangle.frag` |
| Phase 0 / Phase 5a: IBL pre-computation — startup irradiance cubemap, prefiltered specular cubemap, BRDF LUT, split-sum IBL in PBR shaders | `VulkanIbl.c`, `fullscreen.vert`, `irradiance.frag`, `prefilter.frag`, `brdf_lut.frag`, `meshlet.frag`, `triangle.frag` |
| Phase 6: Screen Space Reflections — half-res linear raymarch with binary-search refinement + bilateral upsample | `VulkanSSRPass.c`, `ssr.comp`, `ssr_upsample.comp` |
| Phase 7a: Composite Pass — Fresnel-Schlick SSR blend into CompositeScene; TAA reads CompositeScene | `VulkanCompositePass.c`, `composite.comp` |

---

## What Is Missing (in priority order)

### ~~1. Forward+ Lighting — Phase 4c + Phase 5a update~~ ✅ DONE

---

### ~~2. IBL Pre-Computation — Phase 0 + Phase 5a update~~ ✅ DONE
Status: HDR environment loading from pak is wired, startup GPU precomputation now generates the irradiance cubemap, prefiltered specular cubemap, and BRDF LUT, and `meshlet.frag` / `triangle.frag` now use the split-sum IBL term. A procedural 128×128 noise texture is also uploaded into the `BlueNoise` slot for later SSR / dither work.

---

### ~~3. Skybox — Phase 5b~~ ✅ DONE
Status: Added `VulkanSkyboxPass.c` after opaque geometry. It draws a fullscreen triangle into `SceneColor`, reconstructs the view direction from the camera inverse matrices, samples the HDR environment map, and writes clip-space depth `0.0` so the pass only shades untouched reverse-Z background pixels.

---

### ~~4. VB-AO — Phase 4b~~ ✅ DONE
Status: Added half-resolution `AOTexture` plus a read-only full-resolution AO depth copy to frame resources, wired a new `VulkanAOPass.c` compute pass that reconstructs view-space positions from the depth buffer and writes AO into the half-res target, and updated `meshlet.frag` / `triangle.frag` to do a depth-aware 3×3 bilateral upsample and apply the result to the ambient term.

---

### ~~5. Screen Space Reflections — Phase 6~~ ✅ DONE
Status: Added `SSRTexture` (R16G16B16A16_SFLOAT half-res) and `SSRTextureUpsampled` (full-res) to frame resources. `VulkanSSRPass` dispatches two compute passes after `VulkanHiZPass`: `ssr.comp` (half-res linear march with binary-search refinement, blue-noise temporal jitter, Schlick confidence) and `ssr_upsample.comp` (depth + normal bilateral 4-tap upsample to full-res). Toggle with `Ctrl+R`. Output `SSRTextureUpsampled` is ready for the Composite pass (step 6).

---

### ~~6. Composite Pass — Phase 7a~~ ✅ DONE
Status: Added `CompositeScene` (R16G16B16A16_SFLOAT, full-res, SAMPLED | STORAGE | TRANSFER_DST) to frame resources. `VulkanCompositePass` runs after `VulkanSSRPass` and before `VulkanTaaPass`. The compute shader (`composite.comp`) blends `SSRTextureUpsampled` into `SceneColor` using a roughness-adjusted Fresnel-Schlick term (matching the IBL Fresnel in `meshlet.frag`), and writes the result into `CompositeScene`. TAA now reads `CompositeScene` instead of `SceneColor`. Fast path skips all G-buffer reads for sky/non-reflective pixels (SSR confidence < 0.001).

---

### ~~7. Motion Blur — Phase 8a + 8b~~ ⏭️ SKIPPED
Status: Intentionally skipped. Motion blur tends to be more distracting than beneficial during gameplay and is one of the most commonly disabled settings. The velocity buffer is already written in the depth prepass, so this can be revisited later if needed. Bloom (#8) reads directly from TAA's `ResolvedColor` instead of a `BlurredScene` intermediate.

---

### 8. Bloom — Phase 8c
Kawase dual-filter bloom with soft-knee threshold.

Steps:
- Add **BloomTexture** (B10G11R11_UFLOAT_PACK32, half-res + 5-mip chain) and **BloomTempBuffer** (ping-pong) to frame resources
- Prefilter pass (full-res → half-res): luminance threshold with soft knee
- Downsample chain: 4 passes (half → quarter → eighth → sixteenth → 32nd)
- Upsample chain: 4 passes with 9-tap tent filter + additive blend back to half-res
- Combine with `BlurredScene` in the final pass

---

### ~~9. CAS + Blue Noise Dither — Phase 9a + 9b~~ ✅ DONE
Status: Implemented both in `final.frag`. **CAS** (AMD FidelityFX-style Contrast Adaptive Sharpening) runs in linear HDR space before tonemapping, using a 3×3 neighbourhood with contrast-adaptive weights (default strength 0.5, configurable via `vulkanFinalPassSetCasStrength()`). **Blue noise dither** applies a frame-rotated 128×128 blue noise tile as ±0.5/255 offset after ACES tonemap to eliminate 8-bit banding on the sRGB swapchain. Push constants extended with `casStrength`; no new render passes or frame resources needed.

---

## Frame Resource Gaps (to add as needed)

| Resource | Format | When Needed |
|---|---|---|
| `IrradianceMap` | R11G11B10_FLOAT cubemap 32×32 | IBL (#2) |
| `PrefilterMap` | R11G11B10_FLOAT cubemap 128×128, 5 mips | IBL (#2) |
| `BRDF_LUT` | R16G16_SFLOAT 512×512 | IBL (#2) |
| `BlueNoise` | R8_UNORM 128×128 | IBL (#2), Dither (#9) |
| `LightGrid` | StructuredBuffer uint | Clustered lights (#1) |
| `AOTexture` | R8_UNORM half-res | VB-AO (#4) |
| `SSRTexture` | R16G16B16A16_SFLOAT half-res | SSR (#5) |
| `SSRTextureUpsampled` | R16G16B16A16_SFLOAT full-res | SSR (#5) |
| `CompositeScene` | R16G16B16A16_SFLOAT full-res | Composite (#6) |
| ~~`MotionTileBuffer`~~ | ~~R8_UINT 240×135 tiles~~ | ~~Motion Blur (#7) — skipped~~ |
| ~~`BlurredScene`~~ | ~~B10G11R11_UFLOAT_PACK32 full-res~~ | ~~Motion Blur (#7) — skipped~~ |
| `BloomTexture` + mip chain | B10G11R11_UFLOAT_PACK32 half-res | Bloom (#8) |
| `BloomTempBuffer` | B10G11R11_UFLOAT_PACK32 half-res | Bloom (#8) |
