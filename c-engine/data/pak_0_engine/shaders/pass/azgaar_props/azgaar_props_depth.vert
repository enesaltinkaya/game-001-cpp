#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_ARB_shading_language_include : enable

// ── Azgaar props depth/velocity vertex shader ─────────────────────────────
// Replicates the instance transform + wind sway from azgaar_props.vert so
// that the depth/velocity pre-pass can generate correct motion vectors for
// the animated props.  Outputs clip positions for the current (non-jittered)
// and previous cameras plus the view-space normal.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 8) in vec2 inUV;
layout(location = 9) in uint inTexId;

layout(location = 2) in vec3 inPos;
layout(location = 3) in float inYaw;
layout(location = 4) in float inScale;
layout(location = 6) in float inPhase;
layout(location = 7) in uint inSpecies;

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

layout(push_constant) uniform PropPush {
    vec4  boundsMin;
    vec4  boundsMax;
    float swayFactor;
    float lodRole; // 0 = near LOD, 1 = far LOD, 2 = no LOD (always visible)
} pc;

layout(location = 0) out vec4 outClipCurrent;
layout(location = 1) out vec3 outViewNormal;
layout(location = 2) out vec4 outClipPrev;
layout(location = 3) out vec2 outUV;
layout(location = 4) flat out uint outTexId;
layout(location = 5) flat out uint outSpecies;

void main() {
    // Match azgaar_props.vert: hard LOD switch, collapse the hidden side to a
    // point so it writes no depth / velocity (the pre-pass is depth-only, no
    // alpha blend).
    float dist = length(inPos.xz - sceneBuffer.cameras[0].position.xz);
    float visible;
    if (pc.lodRole < 0.5)      visible = (dist <  sceneBuffer.props.lod.x) ? 1.0 : 0.0;
    else if (pc.lodRole < 1.5) visible = (dist >= sceneBuffer.props.lod.x) ? 1.0 : 0.0;
    else                        visible = 1.0;
    float visScale = (visible < 0.5) ? 0.0 : 1.0;

    vec3 local = inPosition * (inScale * visScale);

    float cy = cos(inYaw);
    float sy = sin(inYaw);
    mat3 rot = mat3(cy, 0.0, -sy,
                     0.0, 1.0, 0.0,
                     sy, 0.0,  cy);
    local = rot * local;
    vec3 nrm = rot * inNormal;

    vec4 wind = sceneBuffer.props.wind;
    float span = max(pc.boundsMax.y - pc.boundsMin.y, 1e-3);
    float hN   = clamp((inPosition.y - pc.boundsMin.y) / span, 0.0, 1.0);
    float swayW = hN * hN * pc.swayFactor;

    // Sway at the CURRENT frame time — used for the depth write (gl_Position)
    // and the current clip.  Matches azgaar_props.vert exactly.
    float tCurr  = float(sceneBuffer.time) / 1000.0;
    float swayCur = sin(tCurr * wind.z + inPhase) * wind.w * swayW;

    // Sway at the PREVIOUS frame time (sceneBuffer.prevTime).  Using the
    // previous time for outClipPrev means the velocity buffer captures the
    // wind-sway displacement of animated props (tree leaves) between frames.
    // Without this the motion vector only reflects camera motion, so TAA
    // can't track the swaying canopies and the tree leaves shimmer.
    float tPrev   = float(sceneBuffer.prevTime) / 1000.0;
    float swayPrev = sin(tPrev * wind.z + inPhase) * wind.w * swayW;

    vec3 worldPosCur  = inPos + local;
    worldPosCur.xz   += wind.xy * swayCur;
    vec3 worldPosPrev = inPos + local;
    worldPosPrev.xz  += wind.xy * swayPrev;

    // Player reaction: same radial push as azgaar_props.vert (must match
    // exactly, the pre-pass writes the depth the colour pass tests against).
    // The CURRENT player position is used for both the cur and prev clips —
    // a 1-frame lag in the reaction is invisible, and it keeps the motion
    // vector consistent with the depth write.
    vec4  player = sceneBuffer.props.playerPos;
    float pDist  = length(vec3(inPos) - player.xyz);
    float pFall  = 1.0 - smoothstep(0.0, PROPS_PLAYER_REACH, pDist);
    float pAmp   = (PROPS_PLAYER_BASE + PROPS_PLAYER_SPEED_SCALE * player.w) * pFall * swayW;
    vec2  pDir   = (inPos.xz - player.xz) / max(pDist, 1e-3);
    worldPosCur.xz  += pDir * pAmp;
    worldPosPrev.xz += pDir * pAmp;

    vec4 worldPos4     = vec4(worldPosCur, 1.0);
    vec4 prevWorldPos4 = vec4(worldPosPrev, 1.0);
    gl_Position    = sceneBuffer.cameras[0].viewProjection * worldPos4;
    outClipCurrent = sceneBuffer.cameras[0].viewProjectionNoJitter * worldPos4;
    outClipPrev    = sceneBuffer.cameras[0].prevViewProjectionNoJitter * prevWorldPos4;
    outViewNormal  = normalize((sceneBuffer.cameras[0].view * vec4(nrm, 0.0)).xyz);

    // Routed to the fragment so it can apply the SAME alpha discard as the
    // colour pass (see azgaar_props_depth.frag).
    outUV      = inUV;
    outTexId   = inTexId;
    outSpecies = inSpecies;
}
