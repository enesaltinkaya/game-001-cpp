# Screen Space GI — Cheap Methods

Reference notes on cheap screen-space global illumination methods, ranked by cost.
Context: we use a software Vulkan pipeline (no RT cores assumed; see the
AMD-DCC note in `oit-amd-dcc.md`), so methods that need hardware tracing are
listed for completeness only.

## 1. Diffusion-based screen-space GI — cheapest

Karl Petersen, "Simplified Diffusion for Real-Time GI" (2017).

- Not true light transport: a blur of the direct-lighting buffer that is
  clamped at occluders, so it reads as "soft AO + ambient with light leaking
  around corners".
- Pipeline:
  1. Render direct lighting into a buffer; add a low-frequency ambient/IBL term.
  2. Iterate 2–3 times: small blur (3×3 Gaussian) + clamp contribution where
     the depth-normal discontinuity is large (`dot(n1,n2) < ~0.5` or a
     depth/normal edge mask) so light does not bleed across surfaces.
- Cost: a few 8-tap passes, ~0.2–0.4 ms at 1080p. No ray tracing, very robust.
- Best when: lowest-effort win; hides well behind a slider; only needs the
  direct-lighting buffer + normals/depth. Roughly ~100 lines of shader per pass.

### Implemented (this engine)

`c-engine/renderer/vulkan/pass/diffuse_gi/` — `VulkanDiffuseGIPass` +
`shaders/pass/diffuse_gi/diffuse_gi.comp`. Registered between `decal` and
`composite` in `Vulkan.cpp`:

- **Input:** the directly-lit `sceneColor` (post-OIT) + depth + oct-encoded
  normals.
- **Diffusion:** N iterations (default 2) of a separable edge-aware blur —
  one horizontal and one vertical 17-tap Gaussian (±8) per iteration —
  ping-ponged between two **quarter-resolution** buffers (linear upsampling
  in the composite). Per-tap weight = gaussian falloff (default σ 20
  render-res px, so colour travels tens of px) × relative inverse-depth
  edge (occlusion). An optional normal-dot gate exists but is **off by
  default** — grass colour climbing a vertical wall is the intended
  colour-bleed look. Sky pixels are copied verbatim and sky taps
  contribute nothing (no horizon smear).
- **Output:** the composite pass adds `(diffused - direct) × strength` —
  sign-preserving, so the bleed both tints/darkens bright surfaces (grass
  green onto a white wall) and light-leaks into dark ones. Strength is a
  blend factor (default 0.4; 1 = fully diffused). Added before AO so
  occlusion attenuates the bleed, before fog so distance still erases it.
- **Toggles:** settings → graphics → "Diffusion GI" (persisted as
  `giDisabled`), debug GUI (Ctrl+B) → "Diffusion GI" button.
- **Env vars** (see the pass header for details):
  `ENGINE_GI_DISABLED`, `ENGINE_GI_ITER` (default 2, 1..8),
  `ENGINE_GI_STRENGTH` (default 0.4, 0..1), `ENGINE_GI_RES` (default 0.25),
  `ENGINE_GI_RADIUS` (σ in render-res px, default 20),
  `ENGINE_GI_DEPTH_EDGE` (default 0.05), `ENGINE_GI_NDOT_MIN` / `_MAX`
  (default −1 / 0.9 = gate off).
- **Tuning notes:** if the bleed looks too strong, lower `ENGINE_GI_STRENGTH`
  before touching the edge thresholds; `ENGINE_GI_RADIUS` controls how far
  the colour travels.

## 2. SSGI / screen-space ray-march

Geometer et al. (2016); used by Unreal Engine 5.1.

- Per pixel, march a hemisphere in screen space through the G-buffer; at the
  hit point sample a **diffuse radiance buffer** (baked radiance, or a
  runtime radiance cache).
- Cost: 16–32 taps, ~0.5–1 ms at 1080p.
- Classic artifact: depth-reconstruction bias (banding/holes). Mitigate with
  an SDF (see SDFGI) or a radiance cache.

## 3. SDFGI

NVIDIA (2021).

- Rebuild a 512³ SDF from depth in screen space; 32-tap bracket probe for
  single-bounce diffuse irradiance.
- Cost: ~0.3–0.5 ms at 1080p.
- Limitations: single bounce; quality bounded by SDF/depth-reconstruction
  quality.

## 4. SVOGI (screen-space voxel GI)

- Screen-space voxelization (96–256³ grid), 2–3 bounces.
- Cost: ~1–2 ms at 1080p — not absolute-cheap, but the usual quality-per-ms
  sweet spot for visible software GI; handles indirect diffuse far better
  than the methods above.

## 5. ReSTIR-GI (requires RT cores)

- ReSTIR DI + spatial/temporal GI resampling. Cheap on paper, reads like
  Lumen. Needs hardware ray tracing (Vulkan ray queries; works on RDNA too,
  though we currently target AMD). Bigger integration; listed for completeness.

## Recommendation for this engine

Implemented: **diffusion GI** (#1, see above). If we want something
demonstrable beyond it, **SVOGI** (#4) is the best quality-per-ms in a
software path.

## References

- Karl Petersen, "Simplified Diffusion for Real-Time GI" (2017) —
  https://www.khronos.org/developers/featured/graphics-programming-in-vulkan (blog:
  https://www.khronos.org/developers/featured/simplified-diffusion-for-real-time-global-illumination)
- Geometer et al., "Screen Space Global Illumination" (2016) —
  https://developer.nvidia.com/gtcs16-presentations
- NVIDIA, "Real-Time Global Illumination with Signed Distance Fields" (2021) —
  https://research.nvidia.com/en/publication/real-time-global-illumination-with-signed-distance-fields
- "Screen-Space Voxel Global Illumination" (2018) —
  https://github.com/luizmax/svogi
- NVIDIA ReSTIR paper + demo (2022+) —
  https://research.nvidia.com/publication/2022-05_restarter-next-generation-sampling-and-resampling-for-real-time-ray-tracing
