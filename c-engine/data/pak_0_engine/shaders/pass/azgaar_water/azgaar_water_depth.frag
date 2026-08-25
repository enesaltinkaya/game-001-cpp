#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_ARB_shading_language_include : enable

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

layout(location = 0) in vec4 inClipCurrent;
layout(location = 1) in vec3 inViewNormal;
layout(location = 2) in vec4 inClipPrev;

layout(location = 0) out vec2 outVelocity;
layout(location = 1) out vec2 outViewNormalXY;

// Must match the vertex shader's WaterPushConstants (same push-constant
// payload is pushed once per draw and read by both stages).
layout(push_constant) uniform WaterPushConstants {
    uint  depthIndex;
    uint  width;
    uint  height;
    float nearZ;
    float farZ;
    float projM00;
    float projM11;
    float projM20;
    float projM21;
} pc;

float linearizeDepth(float d) {
    return (pc.nearZ * pc.farZ) / max(pc.nearZ + (pc.farZ - pc.nearZ) * d, 1e-7);
}

// Reconstruct the terrain/world height under this pixel from the scene depth
// buffer (mirrors azgaar_water.frag terrainPosAtPixel, jitter-corrected).
// The depth buffer also contains the player character, so this is the SAME
// "is there water here" test the color pass uses to decide what it draws.
float terrainHeightAtPixel() {
    float d = texelFetch(sampler2D(textures[nonuniformEXT(pc.depthIndex)],
                                   samplers[SAMPLER_NEAREST]),
                        ivec2(gl_FragCoord.xy), 0).r;
    vec2 uv  = (vec2(gl_FragCoord.xy) + 0.5) / vec2(pc.width, pc.height);
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float linZ = linearizeDepth(d);
    vec3 viewPos = vec3((ndc.x + pc.projM20) * linZ / pc.projM00,
                        (ndc.y + pc.projM21) * linZ / pc.projM11,
                        -linZ);
    return (sceneBuffer.cameras[0].invView * vec4(viewPos, 1.0)).xyz.y;
}

void main() {
    // ── Early-out over dry land (identical to azgaar_water.frag) ────────
    // Where the reconstructed surface (terrain, or the player character
    // occluding it) sits at or above the undisturbed sea level there is no
    // water: keep this pixel's scene motion vector / view normal untouched
    // instead of stamping the swell animation over the dry beach or the
    // character standing on it.
    float waterY = sceneBuffer.water.surfaceY.x;
    if (terrainHeightAtPixel() >= waterY) discard;

    vec2 ndcCurrent = inClipCurrent.xy / inClipCurrent.w;
    vec2 ndcPrev    = inClipPrev.xy    / inClipPrev.w;
    vec2 pixelVelocity = (ndcCurrent - ndcPrev) * (sceneBuffer.cameras[0].viewport * 0.5);
    pixelVelocity.y = -pixelVelocity.y;
    outVelocity = clamp(pixelVelocity, vec2(-32767.0), vec2(32767.0));

    vec3 n = normalize(inViewNormal);
    outViewNormalXY = n.xy;
}