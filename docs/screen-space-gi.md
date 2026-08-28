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

- Lowest-effort first step: **diffusion GI** (#1) — only needs the existing
  direct-lighting buffer plus normals/depth, no new dependencies.
- If we want something demonstrable: **SVOGI** (#4) — best quality-per-ms in
  a software path.

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
