# IBL System Improvements (Inspired by Blender EEVEE)

## Status: IMPLEMENTED ✅

### 1. Sun Extraction from Environment Map ✅
**Files changed:**
- `c-engine/renderer/vulkan2/resources/VulkanIbl.c` — CPU-side sun extraction during HDR load
- `c-engine/renderer/vulkan2/resources/VulkanIbl.h` — `IblSunLight` struct, `vulkanIblGetExtractedSun()`
- `c-engine/ecs/system/light/LightSystem.c` — uses extracted sun for directional light direction/color/intensity

**How it works:**
- During environment map loading, pixels above `SUN_THRESHOLD` (10.0) are separated
- Direction is computed as weighted average of texel directions (weighted by sun radiance × solid angle)
- Angular size derived from directional coherence (EEVEE approach)
- Color and intensity extracted from total sun irradiance
- The irradiance/prefilter shaders now use luminance-preserving threshold clamping instead of flat per-channel clamp — no energy loss since extracted sun provides the excess as a shadow-casting directional light

### 2. L1 Spherical Harmonics for Diffuse Irradiance ✅
**Files changed:**
- `c-engine/renderer/vulkan2/resources/VulkanIbl.c` — CPU-side L1 SH extraction
- `c-engine/renderer/vulkan2/resources/VulkanResourceManager.h` — `VulkanIblSetInfo` with SH coefficients
- `c-engine/renderer/vulkan2/resources/VulkanResourceManager.c` — updated `VulkanIblData` struct
- `c-engine/data/pak_0_engine/shaders/includes/globalset.shader` — SH fields in `IblData`
- `c-engine/data/pak_0_engine/shaders/pass/meshlet/meshlet.frag` — `evaluateSHIrradiance()`
- `c-engine/data/pak_0_engine/shaders/pass/triangle/triangle.frag` — `evaluateSHIrradiance()`
- `c-engine/data/pak_0_engine/shaders/pass/oit/oit_accumulate.frag` — `evaluateSHIrradiance()`

**How it works:**
- L1 SH (4 coefficients × RGB) extracted on CPU with proper solid-angle weighting for equirectangular projection
- SH coefficients stored in scene buffer (64 bytes vs 6×32×32 irradiance cubemap)
- At runtime, Lambert-convolved SH evaluation: `A0*Y00*L0 + A1*Y1x*(L1_x*Nx + L1_y*Ny + L1_z*Nz)`
- Falls back to irradiance cubemap or environment map if SH not available
- Irradiance cubemap still generated for the specular→diffuse blend at high roughness

### 3. Improved Prefilter Convolution ✅ (partial)
**Files changed:**
- `c-engine/data/pak_0_engine/shaders/pass/ibl/prefilter.frag` — improved clamping, early-out for roughness 0
- `c-engine/data/pak_0_engine/shaders/pass/ibl/irradiance.frag` — luminance-preserving threshold clamping
- `c-engine/data/pak_0_engine/shaders/pass/ibl/brdf_lut.frag` — increased samples from 256 to 1024

**What was improved:**
- Sun-threshold-based luminance-preserving clamping (instead of flat per-channel MAX_SOURCE_HDR)
- Early-out for roughness 0 (skip convolution, direct sample)
- BRDF LUT quality: 256→1024 samples (eliminates grazing angle noise)
- Reduced prefilter samples from 1024→512 (PDF-based mip selection makes this sufficient)

**Cascaded convolution deferred:**
Full cascaded mip-chain convolution (convolving each mip from the previous one, 196 samples) requires per-mip-level image layout transitions, which the current `vulkanTransition` abstraction doesn't support (it transitions all mip levels atomically). This could be implemented later by adding per-subresource transition support.

### 4. Octahedral Mapping — SKIPPED (by request)

## Architecture Overview

```
HDR Environment Load (CPU)
    ├── extractSHAndSun()
    │   ├── L1 SH coefficients → scene buffer → evaluateSHIrradiance() in shaders
    │   └── Sun light → LightSystem → directional light with shadows
    │
    ├── Upload equirectangular texture (with mips)
    │
    └── GPU Precompute (one-time)
        ├── Irradiance cubemap (32², 2048 samples, sun-threshold clamped)
        ├── Prefilter cubemap (256², 6 mips, 512 samples, sun-threshold clamped)
        ├── BRDF LUT (512², 1024 samples)
        └── Blue noise texture (128²)
```
