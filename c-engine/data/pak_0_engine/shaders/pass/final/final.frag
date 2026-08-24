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
    float contrast;
    float rcasStrength;  // 0 = off; AMD RCAS in this pass (upscaler off)
    uint tonemapMode;
    uint pad[2];
};

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"
#include "../../includes/rcas.shader"

/* Sharpening: AMD RCAS has two placements, both driven by the aaCasStrength
 * setting —
 *   upscaler on:  inside the FSR3 upscaler dispatch (VulkanFsrPass) on
 *                 the upscaled image (this pass receives strength 0)
 *   upscaler off: here, on the tonemapped LDR result (TAA / raw path)
 *                 using the same vendored RCAS kernel. */

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

vec3 tonemapAndContrast(vec3 hdr) {
    float exposure   = sceneBuffer.cameras[0].exposure;
    vec3 exposed     = hdr * exposure;
    vec3 ldr;

    // Analytic curves only (the IBL-baked AgX LUTs were removed together
    // with IBL; AgX modes use the polynomial approximation).
    if (tonemapMode == 0u || tonemapMode == 1u) {
        ldr = agxBase(exposed);
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

vec3 fetchDisplay(vec2 uv) {
    return tonemapAndContrast(sampleSceneHdr(uv));
}

void main() {
    vec2 uv = vec2(inUV.x, 1.0 - inUV.y);

    /* Source-texel grid: the color texture may be render-resolution
     * (renderScale < 1 with the upscaler off), so RCAS taps must step in
     * source texels, not output pixels. */
    ivec2 colorSize = textureSize(
        sampler2D(textures[nonuniformEXT(colorTextureIndex)], samplers[SAMPLER_CLAMP_LINEAR]),
        0);
    vec2 texelSize = 1.0 / vec2(max(colorSize, ivec2(1)));

    /* RCAS in the display-referred domain — after bloom/tonemap/contrast,
     * before dithering. exp2(2*s - 2) maps the slider to FsrRcasCon's linear
     * sharpness, the same remap the FSR3 upscaler host applies. */
    vec3 ldr;
    if (rcasStrength > 0.0) {
        vec3 b = fetchDisplay(uv + texelSize * vec2(0.0, -1.0));
        vec3 d = fetchDisplay(uv + texelSize * vec2(-1.0, 0.0));
        vec3 e = fetchDisplay(uv);
        vec3 f = fetchDisplay(uv + texelSize * vec2(1.0, 0.0));
        vec3 h = fetchDisplay(uv + texelSize * vec2(0.0, 1.0));
        ldr    = rcasFilter(b, d, e, f, h, exp2(2.0 * rcasStrength - 2.0));
    } else {
        ldr = fetchDisplay(uv);
    }

    /* sRGB attachment (swapchain or lens input): keep output linear —
     * the attachment store encodes. */
    outColor = vec4(ldr, 1.0);
}
