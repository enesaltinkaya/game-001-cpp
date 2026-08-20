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
layout(location = 5) in flat uint inEntity;

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
     * across large triangles spanning a depth range (ground plane,
     * walls at grazing angles).  The vertex shader outputs raw clip
     * coordinates; the hardware interpolates them perspective-correctly,
     * and we perform the perspective divide here per-pixel. */
    vec2 ndcCurrent  = inClipCurrent.xy / inClipCurrent.w;
    vec2 ndcPrev     = inClipPrev.xy    / inClipPrev.w;
    vec2 pixelVelocity = (ndcCurrent - ndcPrev) * (sceneBuffer.cameras[0].viewport * 0.5);
    pixelVelocity.y    = -pixelVelocity.y;
    vec2 velocity = clamp(pixelVelocity, vec2(-32767.0), vec2(32767.0));

    Material material = materialBuffer.materials[inMaterialId];

    if ((material.featureMask & (1u << MAT_ALPHA_MASK)) != 0u) {
        vec4 baseColor = material.baseColor;

        /* ── Jitter compensation for alpha-cutout edges.
         *
         *    The jittered projection shifts which world point maps to each
         *    pixel.  At alpha-cutout edges this shifts the discard boundary
         *    between frames, causing the fragment to appear/disappear and
         *    producing violent flickering under FSR / temporal AA.
         *
         *    Compensation strategy:
         *    1. Compute the jitter-induced screen-space offset in pixels.
         *    2. Convert to UV-space offset using the UV gradient
         *       (dFdx/dFdy of the UV).  This tells us how much the UV
         *       changes per pixel — the inverse gives us the UV offset
         *       corresponding to the jitter.
         *    3. Apply the offset BEFORE sampling the texture and BEFORE
         *       the alpha test.  After correction the alpha test samples
         *       at the same world-space position each frame.
         *
         *    This must run for ALL alpha-cutout geometry, not just
         *    textured ones.  For untextured geometry the UV gradient
         *    is still valid and ensures the discard pattern is stable.
         */
        vec2 jitterPx = -vec2(sceneBuffer.cameras[0].jitterX,
                              sceneBuffer.cameras[0].jitterY)
                        * sceneBuffer.cameras[0].viewport;
        vec2 uv = inUV * material.baseColorOffsetScale.zw + material.baseColorOffsetScale.xy;

        /* Apply jitter compensation using UV derivatives.  For
         * untextured materials the texture lookup below is a no-op
         * (baseColor is already correct), but the alpha test still
         * benefits from the stable UV. */
        uv += dFdx(uv) * jitterPx.x + dFdy(uv) * jitterPx.y;

        if ((material.featureMask & (1u << MAT_HAS_TEXTURE_COLOR)) != 0u) {
            baseColor *= sampleMaterialTexture(material.colorTexture, material.colorTextureSampler, uv);
        }

        float cutoff = material.rmas.z;
        float alpha  = baseColor.a;

        /* Write velocity for ALL depth-passing fragments, BEFORE alpha
         * discard. */
        outVelocity = velocity;

        if (stochasticAlphaDiscard(alpha, cutoff, gl_FragCoord.xy, 0u))
            discard;
    }

    float occluderAlpha = cameraOccluderAlpha(inEntity);
    if (occluderAlpha < CAMERA_OCCLUDER_OPAQUE_UNDERLAY_ALPHA) {
        discard;
    }

    /* Non-alpha-cutout fragments also need velocity written. */
    outVelocity = velocity;

    // For alpha-cutout geometry (grass, foliage), override the normal
    // to world-up in view space.  GTAO only sees depth + normals, so
    // vertical grass planes look like solid walls and get heavy
    // darkening.  Writing an upward normal makes GTAO treat foliage as
    // ground-like, eliminating the dark-plane artifact.
    if ((material.featureMask & (1u << MAT_ALPHA_MASK)) != 0u) {
        vec3 upView = normalize((sceneBuffer.cameras[0].view * vec4(0.0, 1.0, 0.0, 0.0)).xyz);
        outViewNormalXY = upView.xy;
    } else {
        outViewNormalXY = normalize(inViewNormal).xy;
    }
}
