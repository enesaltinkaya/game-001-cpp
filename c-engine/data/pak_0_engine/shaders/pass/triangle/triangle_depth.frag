#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_ARB_shading_language_include : enable

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

layout(location = 0) in vec4 inClipCurrent;
layout(location = 1) in vec2 inUV;
layout(location = 2) in flat uint inMaterialId;
layout(location = 3) in vec3 inViewNormal;
layout(location = 4) in vec4 inClipPrev;

layout(location = 0) out vec2 outVelocity;
layout(location = 1) out vec2 outViewNormalXY;

vec4 sampleMaterialTexture(uint texIndex, uint samplerIndex, vec2 uv) {
    return texture(sampler2D(textures[nonuniformEXT(texIndex)],
                             samplers[nonuniformEXT(samplerIndex)]),
                   uv);
}

void main() {
    /* Per-pixel velocity from perspective-correct clip-space positions.
     * This avoids the noperspective interpolation error that occurs
     * across large triangles spanning a depth range. */
    vec2 ndcCurrent  = inClipCurrent.xy / inClipCurrent.w;
    vec2 ndcPrev     = inClipPrev.xy    / inClipPrev.w;
    vec2 pixelVelocity = (ndcCurrent - ndcPrev) * (sceneBuffer.cameras[0].viewport * 0.5);
    pixelVelocity.y    = -pixelVelocity.y;
    vec2 velocity = clamp(pixelVelocity, vec2(-32767.0), vec2(32767.0));

    Material material = materialBuffer.materials[inMaterialId];

    if ((material.featureMask & (1u << MAT_ALPHA_MASK)) != 0u) {
        vec4 baseColor = material.baseColor;
        if ((material.featureMask & (1u << MAT_HAS_TEXTURE_COLOR)) != 0u) {
            vec2 uv = inUV * material.baseColorOffsetScale.zw +
                      material.baseColorOffsetScale.xy;
            baseColor *= sampleMaterialTexture(material.colorTexture,
                                               material.colorTextureSampler,
                                               uv);
        }
        // Stochastic alpha test: dither the discard threshold with
        // interleaved gradient noise so TAA/FSR resolves sub-pixel
        // alpha edges temporally instead of binary shimmer.
        float cutoff = material.rmas.z;
        float alpha  = baseColor.a;
        // frameIndex=0: stable discard pattern avoids temporal shimmer.
        // clip() enables alpha-to-coverage for smooth sub-pixel edges.
        if (stochasticAlphaDiscard(alpha, cutoff, gl_FragCoord.xy, 0u)) {
            discard;
        }
    }

    outVelocity = velocity;
    outViewNormalXY = normalize(inViewNormal).xy;
}
