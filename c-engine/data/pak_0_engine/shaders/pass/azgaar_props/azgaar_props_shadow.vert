#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_ARB_shading_language_include : enable

// ── Azgaar props CSM shadow vertex shader ─────────────────────────────────
// Casts the vegetation / buildings / landmarks into the sun cascade shadow
// maps.  Replicates azgaar_props.vert's transform exactly (hard LOD switch,
// yaw, uniform scale, wind sway at the current frame time) so the cast
// geometry matches what the colour pass draws this frame, then projects with
// the active cascade's light view-projection from the scene buffer.  The
// hidden LOD side collapses to a point (visScale 0), so it writes nothing —
// same as the colour / pre-pass shaders.

layout(location = 0) in vec3 inPosition; // mesh local (metres, origin at base)
layout(location = 8) in vec2 inUV;      // mesh UV (texture alpha test)
layout(location = 9) in uint inTexId; // texture-array index (0xFFFFFFFF = none)

layout(location = 2) in vec3 inPos;      // instance world position (ground)
layout(location = 3) in float inYaw;     // 0..2pi
layout(location = 4) in float inScale;   // target height (metres)
layout(location = 6) in float inPhase;   // 0..2pi wind de-sync

layout(location = 0) out vec2 outUV;
layout(location = 1) flat out uint outTexId;

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

// Must match PropShadowPushConstants in VulkanAzgaarPropsPass.c (std430).
layout(push_constant) uniform PropShadowPush {
    vec4  boundsMin;    // xyz = local AABB min (metres), w unused
    vec4  boundsMax;    // xyz = local AABB max (metres), w unused
    float swayFactor;   // 0..1, how much this species sways
    float lodRole;      // 0 = near LOD, 1 = far LOD, 2 = no LOD (always visible)
    uint  cascadeIndex; // selects sceneBuffer.shadow.shadowViewProjection[i]
} pc;

void main() {
    // Hard distance LOD switch — identical to azgaar_props.vert, so the set
    // of casters matches the visible set (exactly one LOD per tree).
    float dist = length(inPos.xz - sceneBuffer.cameras[0].position.xz);
    float visible;
    if (pc.lodRole < 0.5)      visible = (dist <  sceneBuffer.props.lod.x) ? 1.0 : 0.0; // near
    else if (pc.lodRole < 1.5) visible = (dist >= sceneBuffer.props.lod.x) ? 1.0 : 0.0; // far
    else                        visible = 1.0;                                          // no LOD
    float visScale = (visible < 0.5) ? 0.0 : 1.0;

    vec3 local = inPosition * (inScale * visScale);

    // Yaw rotation around Y (trees/rocks face random headings).
    float cy = cos(inYaw);
    float sy = sin(inYaw);
    mat3 rot = mat3(cy, 0.0, -sy,
                    0.0, 1.0, 0.0,
                    sy, 0.0,  cy);
    local = rot * local;

    // Wind sway at the CURRENT frame time (matches the colour pass' geometry
    // this frame, so canopy shadows track the swaying crowns).
    vec4 wind = sceneBuffer.props.wind;
    float span = max(pc.boundsMax.y - pc.boundsMin.y, 1e-3);
    float hN   = clamp((inPosition.y - pc.boundsMin.y) / span, 0.0, 1.0);
    float swayW = hN * hN * pc.swayFactor;
    float t    = float(sceneBuffer.time) / 1000.0;
    float sway = sin(t * wind.z + inPhase) * wind.w * swayW;

    vec3 worldPos = inPos + local;
    worldPos.xz += wind.xy * sway; // world-space drift (independent of yaw)

    // Player reaction: same radial push as azgaar_props.vert, so the cast
    // geometry matches the colour pass this frame.
    vec4  player = sceneBuffer.props.playerPos;
    float pDist  = length(vec3(inPos) - player.xyz);
    float pFall  = 1.0 - smoothstep(0.0, PROPS_PLAYER_REACH, pDist);
    float pAmp   = (PROPS_PLAYER_BASE + PROPS_PLAYER_SPEED_SCALE * player.w) * pFall * swayW;
    vec2  pDir   = (inPos.xz - player.xz) / max(pDist, 1e-3);
    worldPos.xz += pDir * pAmp;

    gl_Position = sceneBuffer.shadow.shadowViewProjection[pc.cascadeIndex]
                  * vec4(worldPos, 1.0);
    outUV    = inUV;
    outTexId = inTexId;
}