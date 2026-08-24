# IBL Implementation Review

_Reviewed: March 9, 2026_

Files reviewed:
- `c-engine/renderer/vulkan2/resources/VulkanIbl.c`
- `c-engine/renderer/vulkan2/resources/VulkanIbl.h`
- `c-engine/data/pak_0_engine/shaders/pass/ibl/irradiance.frag`
- `c-engine/data/pak_0_engine/shaders/pass/ibl/prefilter.frag`
- `c-engine/data/pak_0_engine/shaders/pass/ibl/brdf_lut.frag`
- `c-engine/data/pak_0_engine/shaders/pass/ibl/fullscreen.vert`
- `c-engine/data/pak_0_engine/shaders/pass/meshlet/meshlet.frag` (IBL consumption)
- `c-engine/data/pak_0_engine/shaders/pass/triangle/triangle.frag` (IBL consumption)
- `c-engine/renderer/vulkan2/resources/VulkanResourceManager.c` (IblData struct, upload)
- `c-engine/data/pak_0_engine/shaders/includes/globalset.shader` (IblData GLSL struct)
- `c-engine/data/pak_0_engine/shaders/includes/utils.shader` (GeometrySmith)

---

## 🔴 Bug 1: `irradiance.frag` — Double cosine weighting

**Severity:** Significant — irradiance map is too dark.

The shader uses **cosine-weighted hemisphere sampling** (`cosTheta = sqrt(1.0 - xi.y)`), which has PDF = cos(θ)/π. With this sampling strategy the cosine term cancels in the Monte Carlo estimator:

```
E(N) = ∫ L(ω) cos(θ) dΩ
     ≈ (1/N) Σ L(ωi) · cos(θi) / (cos(θi)/π)
     = (π/N) Σ L(ωi)
```

But the code multiplies by `NdotL` again, effectively computing ∫ L·cos²(θ) dΩ instead of ∫ L·cos(θ) dΩ.

**Current code (`irradiance.frag` ~line 75):**
```glsl
irradiance += sampleEnvironment(sampleVec) * NdotL;   // WRONG
```

**Fix:**
```glsl
irradiance += sampleEnvironment(sampleVec);            // CORRECT
```

---

## 🔴 Bug 2: `brdf_lut.frag` — Wrong geometry function for IBL

**Severity:** Moderate — specular IBL subtly incorrect at high roughness.

The BRDF LUT calls `GeometrySmith()` from `utils.shader`, which uses the **direct-lighting** remapping:
```
k = (roughness + 1)² / 8
```

For the IBL split-sum BRDF integration (Epic's formulation), the correct remapping is:
```
k = roughness² / 2
```

This causes the LUT to over-estimate geometry attenuation at high roughness, making specular IBL too bright at grazing angles for rough surfaces.

**Fix:** Define IBL-specific geometry functions locally in `brdf_lut.frag`:
```glsl
float GeometrySchlickGGX_IBL(float NdotV, float roughness) {
    float k   = (roughness * roughness) / 2.0;
    float nom = NdotV;
    float den = NdotV * (1.0 - k) + k;
    return nom / den;
}

float GeometrySmith_IBL(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX_IBL(NdotV, roughness)
         * GeometrySchlickGGX_IBL(NdotL, roughness);
}
```
Then in `IntegrateBRDF()` replace:
```glsl
float G = GeometrySmith(N, V, L, roughness);
```
with:
```glsl
float G = GeometrySmith_IBL(N, V, L, roughness);
```

---

## 🟡 Minor 1: `irradiance.frag` uses `texture()` instead of `textureLod()`

The irradiance shader samples the environment with implicit LOD (`texture()`), relying on screen-space UV gradients from a nonlinear equirectangular mapping. The `prefilter.frag` correctly uses `textureLod(..., 0.0)`. The irradiance shader should do the same for consistency and to avoid accidentally sampling lower mips.

**Fix in `irradiance.frag`:**
```glsl
// Change sampleEnvironment to use textureLod:
vec3 sampleEnvironment(vec3 dir) {
    return textureLod(sampler2D(textures[nonuniformEXT(environmentMapIndex)],
                                samplers[SAMPLER_LINEAR]),
                      directionToEquirectUv(dir),
                      0.0)
        .rgb;
}
```

---

## 🟡 Minor 2: `hashNoise` generates white noise, not blue noise

The procedural hash in `VulkanIbl.c` produces uniformly-distributed **white noise**, not spatially-correlated **blue noise**. The descriptor slot is named `BlueNoise` but the content won't have blue noise properties (even nearby-sample distribution). For future SSR/dithering work, this should be replaced with a real precomputed blue noise texture (e.g., Christoph Peters' free blue noise set). Fine as a placeholder for now.

---

## 🟡 Minor 3: R32G32B32A32_SFLOAT for the environment map

128-bit per pixel is overkill for an HDR equirectangular map. `R16G16B16A16_SFLOAT` has more than enough precision for HDR environment data and halves the memory/bandwidth cost. Not a bug, but worth optimizing.

---

## ✅ Things that are correct

- **Cubemap face directions** — `cubemapDirection()` mapping with the UV Y-flip matches the OpenGL/Vulkan cubemap convention.
- **Descriptor pool routing** — Cube images (irradiance, prefilter) go to `cubeTextures[]` pool via `VK_IMAGE_VIEW_TYPE_CUBE`; 2D images (environment, BRDF LUT, blue noise) go to `textures[]` pool. Shader sampling matches.
- **Layout transitions** — `vulkanTransition()` uses `img->mipLevels` and `img->layers` for full-range barriers, so all 5 prefilter mips and all 6 cubemap layers are properly transitioned.
- **Split-sum IBL consumption** in `meshlet.frag`/`triangle.frag` — Fresnel-roughness, diffuse/specular split, BRDF LUT lookup are all standard and correct.
- **Prefilter convolution** — GGX importance sampling with NdotL weighting + totalWeight normalization matches Epic's split-sum approach.
- **C ↔ GLSL struct layout** — `VulkanIblData` (C) / `IblData` (GLSL) match in field order, types, and size (32 bytes, std430-safe).
- **Proxy attachment rendering** — Per-face/per-mip `VkImageView` creation and proxy approach is clean.
- **Resource lifecycle** — Init/destroy are symmetrical, temp image views are cleaned up after use.
- **Fullscreen triangle vertex shader** — Standard oversized-triangle approach, correct.
