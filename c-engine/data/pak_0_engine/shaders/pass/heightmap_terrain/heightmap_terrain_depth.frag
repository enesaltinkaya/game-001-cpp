#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_ARB_shading_language_include : enable

// Depth/velocity pre-pass fragment shader for the heightmap terrain pass.
// Identical contract to terrain_depth.frag (pixel motion vectors for FSR +
// view-space normal XY for downstream normal-based passes).

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

layout(location = 0) in vec4 inClipCurrent;
layout(location = 1) in vec3 inViewNormal;
layout(location = 2) in vec4 inClipPrev;

layout(location = 0) out vec2 outVelocity;
layout(location = 1) out vec2 outViewNormalXY;

void main() {
    vec2 ndcCurrent  = inClipCurrent.xy / inClipCurrent.w;
    vec2 ndcPrev     = inClipPrev.xy    / inClipPrev.w;
    vec2 pixelVelocity = (ndcCurrent - ndcPrev) * (sceneBuffer.cameras[0].viewport * 0.5);
    pixelVelocity.y    = -pixelVelocity.y;
    outVelocity = clamp(pixelVelocity, vec2(-32767.0), vec2(32767.0));

    vec3 n = normalize(inViewNormal);
    outViewNormalXY = n.xy;
}