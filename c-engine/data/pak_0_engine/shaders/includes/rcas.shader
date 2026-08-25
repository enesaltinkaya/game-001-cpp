/* -----------------------------------------------------------------------
   AMD FidelityFX RCAS (Robust Contrast Adaptive Sharpening)

   Vendored from the FSR3.1 SDK (cpp-thirdparty/fsr3.1):
     include/FidelityFX/gpu/fsr1/ffx_fsr1.h, FsrRcasFilterF() — f32 path,
     with FSR_RCAS_DENOISE enabled, which is the exact configuration the
     FSR3 upscaler uses for its RCAS pass
     (gpu/fsr3upscaler/ffx_fsr3upscaler_rcas.h: FSR_RCAS_DENOISE 1).
   Recast from the FFX macro layer to plain GLSL; the math is unchanged.

   The kernel limits sharpening by *relative* contrast (hitMin/hitMax are
   neighbor ratios, not absolute-range terms), so it is range-robust:
   FSR3 runs it on HDR radiance before tonemapping; this engine also runs
   it on the exposed HDR composite in the final pass (before LPM) when the
   upscaler is off — both placements are valid.

   rcasSharpness is the linear sharpness produced by FsrRcasCon:
   exp2(-stops) — 1.0 = maximum, 0.25 = 2 stops (mild). Map the engine's
   0..1 strength slider with exp2(2*s - 2) (the FSR3 host-side remap) so
   both placements feel identical.
   ----------------------------------------------------------------------- */

#define RCAS_LIMIT (0.25 - 1.0 / 16.0)

/* Approximate reciprocal with fp16-ish precision (AMD
 * ffxApproximateReciprocalMedium). The resolve deliberately uses this to
 * avoid visible tonality changes. */
float rcasRcpMed(float x) { return exp2(-log2(x)); }

/* Sharpening algorithm uses a minimal 3x3 neighborhood (cross taps):
 *    b
 *  d e f
 *    h
 */
vec3 rcasFilter(vec3 b, vec3 d, vec3 e, vec3 f, vec3 h, float rcasSharpness) {
    // Luma times 2.
    float bL = b.b * 0.5 + (b.r * 0.5 + b.g);
    float dL = d.b * 0.5 + (d.r * 0.5 + d.g);
    float eL = e.b * 0.5 + (e.r * 0.5 + e.g);
    float fL = f.b * 0.5 + (f.r * 0.5 + f.g);
    float hL = h.b * 0.5 + (h.r * 0.5 + h.g);

    // Noise detection.
    float nz = 0.25 * bL + 0.25 * dL + 0.25 * fL + 0.25 * hL - eL;
    nz = clamp(abs(nz) * rcasRcpMed(max(max(max(bL, dL), eL), max(fL, hL)) -
                                    min(min(min(bL, dL), eL), min(fL, hL))),
               0.0, 1.0);
    nz = -0.5 * nz + 1.0;

    // Min and max of ring.
    float mn4R = min(min(min(b.r, d.r), f.r), h.r);
    float mn4G = min(min(min(b.g, d.g), f.g), h.g);
    float mn4B = min(min(min(b.b, d.b), f.b), h.b);
    float mx4R = max(max(max(b.r, d.r), f.r), h.r);
    float mx4G = max(max(max(b.g, d.g), f.g), h.g);
    float mx4B = max(max(max(b.b, d.b), f.b), h.b);

    // Limiters (high precision division).
    float hitMinR = mn4R / (4.0 * mx4R);
    float hitMinG = mn4G / (4.0 * mx4G);
    float hitMinB = mn4B / (4.0 * mx4B);
    float hitMaxR = (1.0 - mx4R) / (4.0 * mn4R - 4.0);
    float hitMaxG = (1.0 - mx4G) / (4.0 * mn4G - 4.0);
    float hitMaxB = (1.0 - mx4B) / (4.0 * mn4B - 4.0);
    float lobeR   = max(-hitMinR, hitMaxR);
    float lobeG   = max(-hitMinG, hitMaxG);
    float lobeB   = max(-hitMinB, hitMaxB);
    float lobe =
        max(-RCAS_LIMIT, min(max(max(lobeR, lobeG), lobeB), 0.0)) * rcasSharpness;

    /* Engine extension (deviation from upstream): rcasSharpness may exceed
     * 1.0 (strength slider goes to 1.5). AMD only ever uses multipliers in
     * [0.25, 1.0], where the resolve denominator 1 + 4*lobe is guaranteed
     * >= 0.25. Above 1.0 an unclamped lobe could reach -0.375 and flip the
     * denominator negative (inverted / NaN pixels). Re-clamp to RCAS_LIMIT
     * after the multiply: >1.0 amplifies the adaptive lobe but never past
     * the kernel's hard safety bound. */
    lobe = max(-RCAS_LIMIT, lobe);

    // Apply noise removal.
    lobe *= nz;

    // Resolve.
    float rcpL = rcasRcpMed(4.0 * lobe + 1.0);
    return (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL;
}
