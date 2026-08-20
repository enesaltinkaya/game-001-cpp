# EEVEE IBL (Image-Based Lighting) Pipeline

EEVEE's IBL system is a multi-layered architecture with two complementary probe systems working together: **Sphere Probes** (specular/reflection) and **Volume Probes** (diffuse irradiance), plus a **sun extraction** system for sharp directional lights from the environment.

## 1. High-Level Architecture

The IBL pipeline has these major stages:

### Stage A: World/Environment Capture → Cubemap
### Stage B: Cubemap → Octahedral Atlas (with mip-chain convolution)
### Stage C: Spherical Harmonics & Sun Extraction
### Stage D: Irradiance Grid (Volume Probes) Baking
### Stage E: Runtime Evaluation (per-pixel shading)

---

## 2. Stage A: Environment Capture

**File:** `eevee_view.cc` → `CaptureView::render_world()`

The world shader (`eevee_surf_world_frag.glsl`) is rendered into a **cubemap** (`cubemap_tx_`) by rendering all 6 faces. The world material's node tree is evaluated to produce HDR radiance.

Key detail: If the world uses a **light path node** (different response for diffuse vs glossy rays), EEVEE renders the cubemap **twice**:
- Once with `RAY_TYPE_DIFFUSE` → extracts SH + diffuse sun
- Once with `RAY_TYPE_GLOSSY` → stores in atlas + extracts glossy sun

Otherwise, a single `RAY_TYPE_GLOSSY` render is done for both.

For **local reflection probes** (`CaptureView::render_probes()`), the scene is fully rendered from the probe's position (depth prepass → gbuffer → deferred lighting) into a cubemap per probe.

---

## 3. Stage B: Cubemap → Octahedral Atlas with Convolution

**Files:** `eevee_lightprobe_sphere_remap_comp.glsl`, `eevee_lightprobe_sphere_convolve_comp.glsl`, `eevee_lightprobe_sphere_mapping_lib.glsl`

### Octahedral Mapping
Instead of keeping cubemaps, EEVEE remaps them into an **octahedral projection** stored in a 2D texture atlas (`probes_tx_`). This is more memory-efficient and GPU-friendly than cubemap arrays.

The mapping (`eevee_octahedron_lib.glsl`):
- Projects a 3D direction onto the octahedron (L1 norm projection: `co /= dot(vec3(1), abs(co))`)
- Unfolds the bottom hemisphere
- Maps to [0,1]² UV space

Texel centers are placed **on the edges** of the octahedron to avoid interpolation artifacts across seams.

### Roughness Convolution (Mip Chain)
**File:** `eevee_lightprobe_sphere_convolve_comp.glsl`

The atlas has `SPHERE_PROBE_MIPMAP_LEVELS = 5` mip levels. Each successive mip is convolved with a wider filter to represent rougher reflections. The convolution uses:

1. **Cascaded convolution** — each mip is convolved from the *previous* mip (not from mip 0), reducing sample count
2. **Spherical Gaussian weighting** — `exp(2.0 * (NH - 1.0) / roughness²)`, mapping GGX roughness to a spherical gaussian approximation
3. **Cone sampling** — samples are distributed in a uniform cone whose aperture is derived from roughness
4. **196 samples per texel** via Hammersley quasi-random sequence
5. **Roughness subtraction** for cascaded convolution: uses the Gaussian sum property `G(a) * G(b) = G(a+b)` to derive the *relative* roughness between successive mips

The roughness-to-LOD mapping (from Frostbite's PBR 3.0, eq 53):
```glsl
float ratio = saturate(roughness / SPHERE_PROBE_MIP_MAX_ROUGHNESS); // max roughness = 0.7
float mip = mix(ratio, sqrt(ratio), 0.4) * (MIPMAP_LEVELS - 1);
```

---

## 4. Stage C: Spherical Harmonics & Sun Extraction

**Files:** `eevee_lightprobe_sphere_remap_comp.glsl` (extraction), `eevee_lightprobe_sphere_irradiance_comp.glsl` (SH sum), `eevee_lightprobe_sphere_sunlight_comp.glsl` (sun sum)

During the remap pass, three things can be optionally extracted in parallel:

### Spherical Harmonics (L1)
- Each texel's radiance is weighted by its **solid angle** on the sphere (computed analytically for the octahedral projection, accounting for the non-uniform distortion)
- Encoded into L1 SH (4 coefficients: 1 for L0, 3 for L1) per RGBA channel
- A parallel reduction (shared memory + workgroup barriers) sums across the workgroup, then `eevee_lightprobe_sphere_irradiance_comp.glsl` sums across all workgroups
- Stored in `spherical_harmonics_` buffer for the world probe

### Sun Extraction
- Radiance above `sun_threshold` is separated: `radiance_sun = radiance - clamp(radiance, sun_threshold)`
- Direction is computed as a **weighted average** of texel directions (weighted by sun radiance magnitude × solid angle)
- The length of the averaged direction vector encodes the **angular size** of the sun
- This creates a virtual directional **sun light** that casts proper shadows — something the environment map alone can't do
- Up to 2 suns supported (diffuse + glossy when using light path node)

---

## 5. Stage D: Volume Probes (Irradiance Grids)

**File:** `eevee_lightprobe_volume_eval_lib.glsl`

For diffuse irradiance, EEVEE uses a **3D irradiance grid** system (inspired by Unity's Enemies GDC talk, Quantum Break's DDGI, and McGuire's irradiance fields):

- Grids are stored in a 3D texture atlas with L1 SH per probe (4 layers: L0.M0, L1.Mn1, L1.M0, L1.Mp1)
- Organized in **bricks** of 4×4×4 probes
- **Visibility data** stored as 8-bit bitmask per cell (one bit per corner of the interpolation cell)

### Sampling with Bias
The sampling uses a sophisticated trilinear interpolation with three types of weighting:
1. **Positional weight** — standard trilinear interpolation
2. **Geometry weight** — `(cos_theta * 0.5 + 0.5)²` where theta is angle between normal and direction to probe corner — prevents light leaking through walls
3. **Validity weight** — binary, from the pre-baked visibility bitmask — prevents sampling invalid (occluded) probes

A `normal_bias` and `view_bias` shift the sampling point to avoid self-shadowing.

---

## 6. Stage E: Runtime Evaluation

**File:** `eevee_lightprobe_eval_lib.glsl`, `eevee_deferred_light_frag.glsl`

### Per-Pixel Probe Loading
```glsl
LightProbeSample lightprobe_load(texel, P, Ng, V) {
    result.volume_irradiance = lightprobe_volume_sample(P, V, Ng);  // SH from irradiance grid
    result.spherical_id = lightprobe_spheres_select(P, noise);       // best sphere probe
    return result;
}
```

### Sphere Probe Selection
`lightprobe_spheres_select()` iterates through probes sorted smallest-to-largest by volume. For each probe, it computes a gradient based on the influence shape (ellipsoid or cuboid) and compares against a random threshold for **stochastic** probe selection (avoiding hard boundaries).

### Per-Closure Evaluation
The key function is `lightprobe_eval()` in `eevee_lightprobe_eval_lib.glsl`:

```glsl
float3 lightprobe_eval(samp, closure, P, V, thickness) {
    LightProbeRay ray = bxdf_lightprobe_ray(closure, P, V, thickness);
    float lod = sphere_probe_roughness_to_lod(ray.perceptual_roughness);
    float fac = sphere_probe_roughness_to_mix_fac(ray.perceptual_roughness);

    float3 radiance_cube = lightprobe_spherical_sample_normalized_with_parallax(
        samp, P, ray.dominant_direction, lod);
    float3 radiance_sh = spherical_harmonics_evaluate_lambert(
        ray.dominant_direction, samp.volume_irradiance);

    return mix(radiance_cube, radiance_sh, fac);
}
```

This **blends between two representations** based on roughness:
- **Low roughness (< 0.7)**: Use the octahedral sphere probe atlas (sharp reflections)
- **High roughness (0.7–0.9 transition)**: Blend to L1 SH from volume probes (diffuse)
- **Full diffuse (> 0.9)**: Pure SH evaluation with Lambert convolution

### Parallax Correction
`lightprobe_sphere_parallax()` adjusts the reflection ray direction using the probe's influence volume shape (ellipsoid or cuboid). It finds the intersection of the reflection ray with the volume boundary and uses that to compute a corrected lookup direction, giving more accurate reflections for non-infinite environments.

### Normalization
To reduce light leaking, the sphere probe sample is **normalized** by comparing the irradiance at the shading point (from volume probes) to the irradiance baked at the sphere probe's location. This ratio scales the reflection to match local lighting conditions:

```glsl
float normalization = numerator.ambient / denominator.ambient;
```

### BxDF-specific dominant direction
Each BxDF type computes its own probe ray:
- **Diffuse**: dominant direction = N, roughness = 1.0
- **GGX Reflection**: dominant direction from `bxdf_ggx_dominant_direction_reflection()` (shifted from mirror direction toward N based on roughness), roughness = material roughness
- **GGX Transmission**: dominant direction through the surface, perceived roughness adjusted for IOR
- **Translucent**: dominant direction = -N, roughness = 1.0

---

## 7. Summary of Key Design Decisions

| Aspect | EEVEE's Approach |
|--------|-----------------|
| **Probe storage** | Octahedral mapping in 2D array atlas (not cubemap arrays) |
| **Specular convolution** | Cascaded mip-chain, spherical gaussian approximation of GGX |
| **Diffuse irradiance** | Separate 3D irradiance grid with L1 SH, visibility-biased trilinear interpolation |
| **Roughness blending** | Smooth transition from sphere probe (specular) to SH (diffuse) at roughness 0.7–0.9 |
| **Sun lights** | Extracted from environment above threshold, creates real shadow-casting directional light |
| **Light leaking prevention** | Normalization of probe samples by local vs probe-location irradiance ratio; visibility bitmasks in volume probes; geometry-aware trilinear weighting |
| **Probe selection** | Stochastic selection with blue noise, sorted by volume (smallest first) |
| **LUT** | Split-sum approximation (Karis/UE4 style) for Fresnel integration |

---

## 8. Key Source Files Reference

| File | Purpose |
|------|---------|
| `eevee_lightprobe_sphere.hh/.cc` | Sphere probe module (C++ host side) |
| `eevee_world.hh/.cc` | World environment handling |
| `eevee_view.cc` | CaptureView: renders world cubemap + scene probes |
| `eevee_lightprobe_sphere_remap_comp.glsl` | Cubemap → octahedral remap + SH/sun extraction |
| `eevee_lightprobe_sphere_convolve_comp.glsl` | Roughness mip-chain convolution |
| `eevee_lightprobe_sphere_irradiance_comp.glsl` | Parallel SH coefficient summation |
| `eevee_lightprobe_sphere_sunlight_comp.glsl` | Parallel sun radiance/direction summation |
| `eevee_lightprobe_sphere_select_comp.glsl` | Copies volume probe irradiance to sphere probe locations |
| `eevee_lightprobe_sphere_lib.glsl` | Sphere probe sampling + normalization |
| `eevee_lightprobe_sphere_eval_lib.glsl` | Sphere probe selection by position |
| `eevee_lightprobe_sphere_mapping_lib.glsl` | Octahedral UV mapping, roughness↔LOD conversion |
| `eevee_lightprobe_eval_lib.glsl` | Combined probe evaluation (sphere + volume) |
| `eevee_lightprobe_volume_eval_lib.glsl` | Volume probe (irradiance grid) sampling with bias |
| `eevee_octahedron_lib.glsl` | Octahedral projection math |
| `eevee_spherical_harmonics_lib.glsl` | SH encode/decode/convolve/rotate/compress |
| `eevee_closure_lib.glsl` | BxDF dispatch for lightprobe rays |
| `eevee_bxdf_microfacet_lib.glsl` | GGX dominant direction + perceived roughness |
| `eevee_bxdf_diffuse_lib.glsl` | Diffuse/translucent lightprobe rays |
| `eevee_deferred_light_frag.glsl` | Deferred lighting pass (combines direct + IBL) |
| `eevee_surf_world_frag.glsl` | World background shader |
| `eevee_lut_comp.glsl` | Split-sum LUT generation |
| `eevee_defines.hh` | All probe constants and resource binding slots |
| `eevee_lightprobe_shared.hh` | Shared GPU/CPU data structures |
