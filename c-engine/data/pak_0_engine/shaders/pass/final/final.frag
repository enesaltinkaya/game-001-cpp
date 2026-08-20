#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_ARB_shading_language_include : enable

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    uint colorTextureIndex;
    uint bloomTextureIndex;
    float bloomStrength;
    float casStrength;  // 0 = off, ~0.5 typical, 1.0 = max sharpening
    float contrast;     // 1.0 = neutral, >1.0 = more contrast
    uint pad[3];
};

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

/* -----------------------------------------------------------------------
   AMD FidelityFX CAS (Contrast Adaptive Sharpening) — simplified scalar

   Important: apply CAS in the display-referred domain, after bloom and
   tonemapping, not on unclamped HDR radiance. The standard CAS shaping term
   assumes values in/near [0, 1]. In HDR (>1.0) the (1 - mx) branch collapses
   toward zero/negative, which effectively disables sharpening over much of the
   image and makes the strength slider look broken.
   ----------------------------------------------------------------------- */

vec3 sampleSceneHdr(vec2 uv) {
    vec3 hdr = texture(sampler2D(textures[nonuniformEXT(colorTextureIndex)],
                                 samplers[SAMPLER_CLAMP_LINEAR]),
                       uv)
                   .rgb;

    if (bloomTextureIndex != 0u) {
        vec3 bloom = texture(sampler2D(textures[nonuniformEXT(bloomTextureIndex)],
                                       samplers[SAMPLER_CLAMP_LINEAR]),
                             uv)
                         .rgb;
        hdr += bloom * bloomStrength;
    }

    return hdr;
}

/* -----------------------------------------------------------------------
   3D LUT sampling from a 2D atlas texture.
   The atlas is an 8×8 grid of 64×64 tiles encoding a 64^3 3D LUT.

   The LUT was baked from Blender's OCIO AgX pipeline:
     Linear Rec.709 → E-Gamut → log2 [-12.47393, 12.5260688117] → AgX .cube → sRGB display

   Input to the LUT is log2-encoded E-Gamut, so we must:
     1. Convert Linear Rec.709 → E-Gamut (matrix multiply)
     2. Log2 encode over the full AgX allocation range (~25 stops)
   Output is sRGB gamma — the shader linearizes for the sRGB swapchain.
   ----------------------------------------------------------------------- */
#define TONEMAP_LUT_SIZE 64
#define TONEMAP_ATLAS_COLS 8
#define TONEMAP_ATLAS_W 512.0
#define TONEMAP_ATLAS_H 512.0
#define TONEMAP_MIN_EV (-12.47393)
#define TONEMAP_MAX_EV 12.5260688117

vec3 sampleTonemapLut(uint lutIdx, vec3 linearColor) {
    // Rec.709 → E-Gamut (matching Blender's OCIO AgX pipeline)
    const mat3 rec709ToEGamut = mat3(0.559371133310077,
                                     0.076220705483506,
                                     0.065526711778770,
                                     0.304783346623009,
                                     0.787971770102846,
                                     0.164546754603724,
                                     0.135845560480477,
                                     0.135807466174722,
                                     0.769926501650374);
    vec3 egamut               = rec709ToEGamut * linearColor;

    // Log2 encode to [0,1] over the full AgX allocation range (~25 stops)
    vec3 logColor =
        clamp((log2(max(egamut, vec3(1e-10))) - TONEMAP_MIN_EV) / (TONEMAP_MAX_EV - TONEMAP_MIN_EV),
              0.0,
              1.0);

    // r=X within tile, g=Y within tile, b=slice index
    float bScaled = logColor.b * float(TONEMAP_LUT_SIZE - 1);
    float slice0  = floor(bScaled);
    float slice1  = min(slice0 + 1.0, float(TONEMAP_LUT_SIZE - 1));
    float bFrac   = bScaled - slice0;

    // Compute tile positions for both slices
    float col0 = mod(slice0, float(TONEMAP_ATLAS_COLS));
    float row0 = floor(slice0 / float(TONEMAP_ATLAS_COLS));
    float col1 = mod(slice1, float(TONEMAP_ATLAS_COLS));
    float row1 = floor(slice1 / float(TONEMAP_ATLAS_COLS));

    // UV within each tile: half-texel inset maps [0,1] to texel centres
    // [0.5, SIZE-0.5].  Clamp to [0.5, SIZE-0.5] so the bilinear 2×2
    // footprint (±0.5 texels) never crosses into a neighbouring tile.
    float rTexel =
        clamp(logColor.r * float(TONEMAP_LUT_SIZE - 1) + 0.5, 0.5, float(TONEMAP_LUT_SIZE) - 0.5);
    float gTexel =
        clamp(logColor.g * float(TONEMAP_LUT_SIZE - 1) + 0.5, 0.5, float(TONEMAP_LUT_SIZE) - 0.5);

    vec2 uv0 =
        vec2(col0 * float(TONEMAP_LUT_SIZE) + rTexel, row0 * float(TONEMAP_LUT_SIZE) + gTexel) /
        vec2(TONEMAP_ATLAS_W, TONEMAP_ATLAS_H);
    vec2 uv1 =
        vec2(col1 * float(TONEMAP_LUT_SIZE) + rTexel, row1 * float(TONEMAP_LUT_SIZE) + gTexel) /
        vec2(TONEMAP_ATLAS_W, TONEMAP_ATLAS_H);

    // Sample both slices with bilinear filtering, then lerp for trilinear
    vec3 c0 =
        texture(sampler2D(textures[nonuniformEXT(lutIdx)], samplers[SAMPLER_CLAMP_LINEAR]), uv0)
            .rgb;
    vec3 c1 =
        texture(sampler2D(textures[nonuniformEXT(lutIdx)], samplers[SAMPLER_CLAMP_LINEAR]), uv1)
            .rgb;

    // LUT output is sRGB gamma; linearize for the sRGB swapchain.
    vec3 srgb = mix(c0, c1, bFrac);
    return toLinear(max(srgb, vec3(0.0)));
}

vec3 tonemapAndContrast(vec3 hdr) {
    float exposure   = sceneBuffer.cameras[0].exposure;
    uint tonemapMode = sceneBuffer.ibl.tonemapMode;
    vec3 exposed     = hdr * exposure;
    vec3 ldr;

    uint lutIdx       = sceneBuffer.ibl.tonemapLutIndex;
    uint lutPunchyIdx = sceneBuffer.ibl.tonemapLutPunchyIndex;
    if (tonemapMode == 0u && lutIdx != 0u) {
        // AgX via ground-truth LUT (baked from Blender's OCIO pipeline)
        ldr = sampleTonemapLut(lutIdx, exposed);
    } else if (tonemapMode == 0u) {
        ldr = agxBase(exposed);  // AgX (polynomial fallback)
    } else if (tonemapMode == 1u && lutPunchyIdx != 0u) {
        // AgX Punchy via ground-truth LUT (Blender OCIO + Punchy look)
        ldr = sampleTonemapLut(lutPunchyIdx, exposed);
    } else if (tonemapMode == 1u) {
        ldr = agxBase(exposed);  // AgX Punchy fallback (no LUT available)
    } else if (tonemapMode == 2u) {
        ldr = aces(exposed);  // ACES Filmic
    } else if (tonemapMode == 3u) {
        ldr = filmic(exposed);  // Filmic (Hable-like)
    } else if (tonemapMode == 4u) {
        ldr = reinhard(exposed);  // Reinhard
    } else if (tonemapMode == 5u) {
        ldr = tonemapUncharted2(exposed);  // Uncharted 2
    } else if (tonemapMode == 6u) {
        ldr = uchimura(exposed);  // Uchimura (Gran Turismo)
    } else if (tonemapMode == 7u) {
        ldr = unreal(exposed);  // Unreal Engine 3
    } else {
        ldr = agxBase(exposed);  // fallback
    }

    if (contrast != 1.0) {
        // Apply contrast in perceptual (gamma) space where 0.5 is mid-gray.
        // The tonemapped LDR values are linear; operating directly on them
        // uses a wrong midpoint (linear 0.5 ≈ perceptual 0.73).
        vec3 perceptual = fromLinear(max(ldr, vec3(0.0)));
        perceptual      = clamp((perceptual - 0.5) * contrast + 0.5, 0.0, 1.0);
        ldr             = toLinear(perceptual);
    }

    return ldr;
}

vec3 sampleDisplayLdr(vec2 uv) {
    return tonemapAndContrast(sampleSceneHdr(uv));
}

vec3 casFilter(vec2 uv, vec2 texelSize, float sharpness) {
    // Fetch 3×3 neighbourhood in display-referred LDR space.
    vec3 a = sampleDisplayLdr(uv + texelSize * vec2(-1.0, -1.0));
    vec3 b = sampleDisplayLdr(uv + texelSize * vec2(0.0, -1.0));
    vec3 c = sampleDisplayLdr(uv + texelSize * vec2(1.0, -1.0));
    vec3 d = sampleDisplayLdr(uv + texelSize * vec2(-1.0, 0.0));
    vec3 e = sampleDisplayLdr(uv);
    vec3 f = sampleDisplayLdr(uv + texelSize * vec2(1.0, 0.0));
    vec3 g = sampleDisplayLdr(uv + texelSize * vec2(-1.0, 1.0));
    vec3 h = sampleDisplayLdr(uv + texelSize * vec2(0.0, 1.0));
    vec3 i = sampleDisplayLdr(uv + texelSize * vec2(1.0, 1.0));

    vec3 mnCross = min(min(min(b, d), min(f, h)), e);
    vec3 mxCross = max(max(max(b, d), max(f, h)), e);
    vec3 mnRing  = min(min(a, c), min(g, i));
    vec3 mxRing  = max(max(a, c), max(g, i));

    vec3 mn = min(mnCross, mnRing);
    vec3 mx = max(mxCross, mxRing);

    // CAS weight: w = sqrt(min(mn/mx, (1-mx)/(1-mn))) mapped through sharpness
    vec3 amp =
        clamp(min(mn / max(mx, vec3(1e-5)), (1.0 - mx) / max(1.0 - mn, vec3(1e-5))), 0.0, 1.0);
    amp = sqrt(amp);

    // 0.0 => off, 1.0 => strongest sharpening
    // Peak is capped near AMD's RCAS_LIMIT (0.25 - 1/16 = 0.1875): beyond
    // that the denominator (1 + 4w) shrinks toward zero and the filter
    // overshoots into ringing. The previous 0.125 cap kept even the max
    // slider position too weak to counteract TAA's temporal softness.
    float peak = -mix(0.0, 0.22, sharpness);
    vec3 w     = amp * peak;

    return (w * (b + d + f + h) + e) / (1.0 + 4.0 * w);
}

/* -----------------------------------------------------------------------
   Blue-noise dither — ±0.5/255 rotated per frame to prevent 8-bit banding
   ----------------------------------------------------------------------- */
vec3 blueNoiseDither(vec3 color, vec2 fragCoord) {
    uint noiseIdx = sceneBuffer.ibl.blueNoiseIndex;
    if (noiseIdx == 0u) return color;

    uint frameIdx = sceneBuffer.cameras[0].frameIndex;
    // Rotate the tile offset each frame (golden ratio spatial + temporal)
    vec2 noiseUV = fragCoord / 128.0;  // blue noise is 128×128
    float angle  = float(frameIdx % 8) * (2.0 * PI / 8.0);
    float c_a = cos(angle), s_a = sin(angle);
    noiseUV = mat2(c_a, -s_a, s_a, c_a) * noiseUV;

    float noise =
        texture(sampler2D(textures[nonuniformEXT(noiseIdx)], samplers[SAMPLER_NEAREST]), noiseUV).r;

    // Dither in approximate sRGB space so the ±0.5 LSB amplitude is
    // perceptually correct at all brightness levels.  A plain linear
    // dither of ±0.5/255 is too weak for bright pixels where one sRGB
    // step spans ~0.007 in linear.
    vec3 srgb = fromLinear(max(color, vec3(0.0)));
    srgb += (noise - 0.5) / 255.0;
    return toLinear(max(srgb, vec3(0.0)));
}

void main() {
    vec2 uv = vec2(inUV.x, 1.0 - inUV.y);

    /* Use the actual source texture dimensions here.
     *
     * sceneBuffer.cameras[0].viewport stores the internal render resolution,
     * which is smaller than the display resolution when FSR is active.
     * Using that lower resolution here makes the final CAS pass step across
     * the already-upscaled image with texel offsets that are too large,
     * softening the result and making FSR modes look like visibly low-res
     * rendering. */
    ivec2 colorSize = textureSize(
        sampler2D(textures[nonuniformEXT(colorTextureIndex)], samplers[SAMPLER_CLAMP_LINEAR]),
        0);
    vec2 texelSize = 1.0 / vec2(max(colorSize, ivec2(1)));

    /* CAS belongs near the end of the pipeline: after TAA/upscaling,
     * after bloom composition, after tonemap/contrast, but before dithering
     * and UI. */
    vec3 ldr;
    if (casStrength > 0.0) {
        ldr = casFilter(uv, texelSize, casStrength);
    } else {
        ldr = sampleDisplayLdr(uv);
    }

    ldr = blueNoiseDither(ldr, gl_FragCoord.xy);

    /* Swapchain is an sRGB attachment, so keep the shader output linear. */
    outColor = vec4(ldr, 1.0);
}
