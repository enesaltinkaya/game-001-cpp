#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_ARB_shading_language_include : enable

// ── Azgaar river ribbon vertex shader ──────────────────────────────────
// Unlike the water grid, the river ribbon is authored directly in world
// space, so there is no camera-following recentering.  Per-vertex attributes
// (SceneVertex layout):
//   loc0 position  = lifted world position (terrain height + 3 cm)
//   loc1 normal    = up (0,1,0)
//   loc2 tangent   = xyz flow direction, w width factor (m)
//   loc3 uv        = x arc length (for scrolling ripples), y flow factor
//                      (0..1, discharge-normalised)

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inUV;

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec3 outFlowDir;
layout(location = 3) out float outArcLen;
layout(location = 4) out float outFlowFactor;

void main() {
    gl_Position = sceneBuffer.cameras[0].viewProjection * vec4(inPosition, 1.0);
    outWorldPos = inPosition;
    outNormal   = inNormal;

    vec3 fd    = inTangent.xyz;
    float fdLen = length(fd);
    outFlowDir = fdLen > 1e-6 ? fd / fdLen : vec3(1.0, 0.0, 0.0);
    outArcLen     = inUV.x;
    outFlowFactor = inUV.y;
}
