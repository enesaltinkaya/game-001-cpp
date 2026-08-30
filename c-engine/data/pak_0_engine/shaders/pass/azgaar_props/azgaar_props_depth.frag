#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_ARB_shading_language_include : enable

// Depth/velocity pre-pass fragment shader for the azgaar props pass.
// Identical contract to heightmap_terrain_depth.frag (pixel motion vectors
// for FSR + view-space normal XY for downstream normal-based passes).

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

layout(location = 0) in vec4 inClipCurrent;
layout(location = 1) in vec3 inViewNormal;
layout(location = 2) in vec4 inClipPrev;
layout(location = 3) in vec2 inUV;
layout(location = 4) flat in uint inTexId;
layout(location = 5) flat in uint inSpecies;

layout(location = 0) out vec2 outVelocity;
layout(location = 1) out vec2 outViewNormalXY;

void main() {
    /* ── Alpha discard — must match azgaar_props.frag exactly. ──
     *
     * The pre-pass runs first and writes motion vectors for every pixel the
     * props geometry covers.  Without the same alpha test the colour pass
     * applies, gap pixels (background visible between branches) would carry
     * the LEAF motion vector even though the rendered content there is the
     * background.  TAA/FSR would then reproject the background's history
     * with the leaf's parallax velocity, smearing and ghosting everything
     * seen through the canopy gaps.
     *
     * This pass rasterizes with the identical (jittered) transform as the
     * colour pass, so gl_FragCoord, the interpolated UV and the UV jitter
     * compensation below all match pixel-for-pixel: velocity is written
     * only where a leaf is actually drawn, and gap pixels keep the
     * background's own motion vector written by the terrain/scene
     * pre-passes.
     */
    /* Must match azgaar_props.frag: same jitter compensation + HARD alpha
     * test (frameIndex 0). */
    vec2 cutUV = inUV;
    vec4 tex = vec4(1.0);
    if (inTexId != 0xFFFFFFFFu) {
        vec2 jitterPx = -vec2(sceneBuffer.cameras[0].jitterX, sceneBuffer.cameras[0].jitterY)
                        * sceneBuffer.cameras[0].viewport;
        cutUV += dFdx(inUV) * jitterPx.x + dFdy(inUV) * jitterPx.y;
    }
    if (inSpecies == 12u) {
        float d = length(cutUV - 0.5);
        if (d > 0.30) discard;
    }
    if (inTexId != 0xFFFFFFFFu) {
        tex = texture(
            sampler2D(textures[nonuniformEXT(inTexId)], samplers[SAMPLER_LINEAR]),
            cutUV);
        if (stochasticAlphaDiscard(tex.a, 0.5, gl_FragCoord.xy, 0u))
            discard;
    }

    vec2 ndcCurrent  = inClipCurrent.xy / inClipCurrent.w;
    vec2 ndcPrev     = inClipPrev.xy    / inClipPrev.w;
    vec2 pixelVelocity = (ndcCurrent - ndcPrev) * (sceneBuffer.cameras[0].viewport * 0.5);
    pixelVelocity.y    = -pixelVelocity.y;
    outVelocity = clamp(pixelVelocity, vec2(-32767.0), vec2(32767.0));

    vec3 n = normalize(inViewNormal);
    /* Must match azgaar_props.frag: the props pipes render double-sided
     * (noCull), so back faces must store the OUTWARD normal.  Structures
     * whose visible surfaces are back faces (inward mesh normals) would
     * otherwise write an inverted normal to the view-normal G-buffer, and
     * contact shadow / occlusion — which only see depth + normals — occlude
     * their tops as if the surface faced away from the sky (dark top /
     * lit bottom glitch).
     */
    if (!gl_FrontFacing) n = -n;
    outViewNormalXY = n.xy;
}
