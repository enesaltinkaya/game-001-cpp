#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_ARB_shading_language_include : enable

// ── Azgaar props CSM shadow fragment shader ───────────────────────────────
// Depth-only with alpha test: textured species (grass cards, leaf cards)
// sample their texture's alpha and discard transparent fragments, so the
// shadow follows the blade/leaf silhouette instead of the whole card quad.
// Procedural species (NO_PROPS_TEX) cast solid, as before.
//
// Hard test, no jitter compensation: the shadow map is rendered in light
// space, where the camera's TAA jitter does not apply, so a plain
// alpha < 0.5 discard is temporally stable (same test as the colour pass'
// hard branch, stochasticAlphaDiscard with frameIndex 0).

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

layout(location = 0) in vec2 inUV;
layout(location = 1) flat in uint inTexId;

void main() {
    if (inTexId != 0xFFFFFFFFu) {
        float a = textureLod(
            sampler2D(textures[nonuniformEXT(inTexId)], samplers[SAMPLER_LINEAR]),
            inUV, 0.0).a;
        if (a < 0.5) discard;
    }
}