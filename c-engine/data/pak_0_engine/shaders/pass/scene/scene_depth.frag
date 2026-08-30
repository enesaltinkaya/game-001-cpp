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
        float cutoff = material.rmas.z;
        float alpha  = material.baseColor.a;

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
    // to world-up in view space.  Contact shadow / occlusion only see depth +
    // normals, so vertical grass planes look like solid walls and get heavy
    // darkening.  Writing an upward normal treats foliage as ground-like,
    // eliminating the dark-plane artifact.
    if ((material.featureMask & (1u << MAT_ALPHA_MASK)) != 0u) {
        vec3 upView = normalize((sceneBuffer.cameras[0].view * vec4(0.0, 1.0, 0.0, 0.0)).xyz);
        outViewNormalXY = upView.xy;
    } else {
        outViewNormalXY = normalize(inViewNormal).xy;
    }
}
