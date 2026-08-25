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
    float rcasStrength;  // 0 = off; AMD RCAS in this pass (upscaler off)
    uint pad[4];
};

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"
#include "../../includes/rcas.shader"

/* This pass composites the input for the FFX LPM tone/gamut mapper:
 * scene HDR + bloom + exposure, still LINEAR. The LPM pass (lpm) applies
 * the tone curve + display gamma and writes the final 8-bit image, which
 * is blitted into the swapchain (or the lens input). The custom
 * tonemapping curves (AgX/ACES/...) that used to run here were replaced
 * by LPM. */

/* Sharpening: AMD RCAS has two placements, both driven by the aaCasStrength
 * setting —
 *   upscaler on:  inside the FSR3 upscaler dispatch (VulkanFsrPass) on
 *                 the upscaled image (this pass receives strength 0)
 *   upscaler off: here, on the exposed HDR composite (TAA / raw path),
 *                 using the same vendored RCAS kernel — the same domain
 *                 the upscaler's internal RCAS works in (HDR, pre-LPM). */

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

    return hdr * sceneBuffer.cameras[0].exposure;
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

    /* RCAS in the HDR domain — after bloom/exposure, before LPM.
     * exp2(2*s - 2) maps the slider to FsrRcasCon's linear sharpness, the
     * same remap the FSR3 upscaler host applies. */
    vec3 hdr;
    if (rcasStrength > 0.0) {
        vec3 b = sampleSceneHdr(uv + texelSize * vec2(0.0, -1.0));
        vec3 d = sampleSceneHdr(uv + texelSize * vec2(-1.0, 0.0));
        vec3 e = sampleSceneHdr(uv);
        vec3 f = sampleSceneHdr(uv + texelSize * vec2(1.0, 0.0));
        vec3 h = sampleSceneHdr(uv + texelSize * vec2(0.0, 1.0));
        hdr    = rcasFilter(b, d, e, f, h, exp2(2.0 * rcasStrength - 2.0));
    } else {
        hdr = sampleSceneHdr(uv);
    }

    /* R16F attachment: store the linear HDR composite as-is — LPM does
     * the tone curve + gamma. */
    outColor = vec4(hdr, 1.0);
}