#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_ARB_shading_language_include : enable

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outVelocity;

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"
#include "../../includes/sky.shader"

void main() {
    vec2 ndc = inUV * 2.0 - 1.0;

    // Reconstruct world-space view direction from the depth buffer
    vec4 view    = sceneBuffer.cameras[0].invProjection * vec4(ndc, 1.0, 1.0);
    vec3 viewDir = normalize(view.xyz / max(view.w, 1e-6));
    vec3 worldDir = normalize(mat3(sceneBuffer.cameras[0].invView) * viewDir);

    /* Camera-only motion vector for the sky.  The sky is not geometry, so it
     * gets no velocity from the depth pass and the velocity buffer stays at
     * its (0,0) clear value — which makes TAA resample the previous sky in
     * place and ghost the whole sky whenever the camera rotates or pans.
     *
     * Compute the motion of a point at the far-plane distance along the view
     * direction, projected through the current and previous *no-jitter* VPs —
     * the same convention as the geometry depth passes.  Translation of the
     * camera produces negligible parallax at that distance; rotation is what
     * the sky actually needs. */
    vec4 skyWorldPos4 = vec4(sceneBuffer.cameras[0].position.xyz +
                                 worldDir * sceneBuffer.cameras[0].zFar,
                             1.0);
    vec4 clipCurrent =
        sceneBuffer.cameras[0].viewProjectionNoJitter * skyWorldPos4;
    vec4 clipPrev = sceneBuffer.cameras[0].prevViewProjectionNoJitter * skyWorldPos4;
    vec2 ndcCurrent = clipCurrent.xy / max(clipCurrent.w, 1e-6);
    vec2 ndcPrev    = clipPrev.xy / max(clipPrev.w, 1e-6);
    vec2 skyVelocity = (ndcCurrent - ndcPrev) * (sceneBuffer.cameras[0].viewport * 0.5);
    skyVelocity.y    = -skyVelocity.y;
    outVelocity      = clamp(skyVelocity, vec2(-32767.0), vec2(32767.0));

    // Procedural sky.
    vec3 skyColor = skyEvaluate(worldDir);

    outColor = vec4(skyColor, 1.0);
}
