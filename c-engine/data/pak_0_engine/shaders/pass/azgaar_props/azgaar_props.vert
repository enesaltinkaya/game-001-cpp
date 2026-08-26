#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_ARB_shading_language_include : enable

// ── Azgaar props vertex shader (workstream B) ─────────────────────────────
// Instanced vegetation / landmarks.  Binding 0 = merged species mesh
// (SceneVertex: position/normal/uv), binding 1 = per-instance transform
// (PropInstance: pos/yaw/scale/color/phase/species).  The per-species push
// constants carry the local AABB (sway weight normalisation) and the sway
// factor, so the wind animation is identical for procedural placeholders and
// later hand-drawn .glb models (D11).

layout(location = 0) in vec3 inPosition;  // mesh local (metres, origin at base)
layout(location = 1) in vec3 inNormal;
layout(location = 8) in vec2 inUV;
layout(location = 9) in uint inTexId;     // texture-array index (NO_PROPS_TEX = none)
layout(location = 10) in vec3 inVertColor; // per-part colour (white = tintable)

layout(location = 2) in vec3 inPos;       // instance world position (ground)
layout(location = 3) in float inYaw;      // 0..2pi
layout(location = 4) in float inScale;    // target height (metres)
layout(location = 5) in vec3 inColor;     // per-instance tint
layout(location = 6) in float inPhase;    // 0..2pi wind de-sync
layout(location = 7) in uint inSpecies;

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

// Must match PropPushConstants in VulkanAzgaarPropsPass.c (std430).
layout(push_constant) uniform PropPush {
    vec4  boundsMin;  // xyz = local AABB min (metres)
    vec4  boundsMax;  // xyz = local AABB max (metres)
    float swayFactor; // 0..1, how much this species sways
    float lodRole;    // 0 = near LOD, 1 = far LOD, 2 = no LOD (always visible)
} pc;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec3 outColor;
layout(location = 3) out vec2 outUV;
layout(location = 4) flat out uint outSpecies;
layout(location = 5) flat out uint outTexId;
layout(location = 6) out vec3 outVertColor;

void main() {
    // Hard distance LOD switch: the near and far LODs are two instances of the
    // same tree; exactly one is visible based on the live camera distance (no
    // cross-fade blend).  The hidden side collapses to a point (visScale 0) so
    // it produces no fragments.  The cull stage keeps both only within the
    // hysteresis ring around the switch, so this is the final arbiter there.
    float dist = length(inPos.xz - sceneBuffer.cameras[0].position.xz);
    float visible;
    if (pc.lodRole < 0.5)      visible = (dist <  sceneBuffer.props.lod.x) ? 1.0 : 0.0; // near
    else if (pc.lodRole < 1.5) visible = (dist >= sceneBuffer.props.lod.x) ? 1.0 : 0.0; // far
    else                        visible = 1.0;                                          // no LOD
    float visScale = (visible < 0.5) ? 0.0 : 1.0;

    // Uniform scale (mesh local is already metre-scale; inScale ==
    // targetMeters / unitHeight, identical for placeholders and .glb models).
    vec3 local = inPosition * (inScale * visScale);

    // Yaw rotation around Y (trees/rocks face random headings).
    float cy = cos(inYaw);
    float sy = sin(inYaw);
    mat3 rot = mat3(cy, 0.0, -sy,
                    0.0, 1.0, 0.0,
                    sy, 0.0,  cy);
    local = rot * local;
    vec3 nrm = rot * inNormal;

    // Wind sway: weight by the fraction of the mesh height (authored space,
    // so it is identical for unit-height placeholders and hand-drawn .glb
    // models), squared so the base stays anchored.  Direction + strength come
    // from the shared AzgaarPropsData wind vector; the per-instance phase
    // de-syncs the field.
    vec4 wind = sceneBuffer.props.wind; // xy dir, z speed, w strength
    float span = max(pc.boundsMax.y - pc.boundsMin.y, 1e-3);
    float hN   = clamp((inPosition.y - pc.boundsMin.y) / span, 0.0, 1.0);
    float swayW = hN * hN * pc.swayFactor;
    float t    = float(sceneBuffer.time) / 1000.0;
    float sway = sin(t * wind.z + inPhase) * wind.w * swayW;

    vec3 worldPos = inPos + local;
    worldPos.xz += wind.xy * sway; // world-space drift (independent of yaw)

    // Player reaction (v1, stateless): radial push away from the player's
    // ground position.  Amplitude = standing base + speed-scaled term, so the
    // grass parts around a standing player and swishes harder while running.
    // Weighted by swayW (height² x species sway factor) so the base stays
    // anchored and static species (rocks, buildings) are unaffected.  The
    // falloff uses the 3D distance to the player, so grass on a hill below
    // the player does not react; the push itself is horizontal (xz).
    vec4  player = sceneBuffer.props.playerPos;
    float pDist  = length(vec3(inPos) - player.xyz);
    float pFall  = 1.0 - smoothstep(0.0, PROPS_PLAYER_REACH, pDist);
    float pAmp   = (PROPS_PLAYER_BASE + PROPS_PLAYER_SPEED_SCALE * player.w) * pFall * swayW;
    vec2  pDir   = (inPos.xz - player.xz) / max(pDist, 1e-3);
    worldPos.xz += pDir * pAmp;

    gl_Position = sceneBuffer.cameras[0].viewProjection * vec4(worldPos, 1.0);

    outWorldPos = worldPos;
    outNormal   = normalize(nrm);
    outColor    = inColor;
    outUV       = inUV;
    outSpecies  = inSpecies;
    outTexId    = inTexId;
    outVertColor = inVertColor;
}