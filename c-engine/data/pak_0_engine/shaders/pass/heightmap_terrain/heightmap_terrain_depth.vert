#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_ARB_shading_language_include : enable

// Depth/velocity pre-pass vertex shader for the heightmap terrain pass.
//
// Same indexed corner VBO/IBO as the scene pass (see
// heightmap_terrain.vert): the CPU precomputed the lattice corners
// (worldPos + normal) at upload time, so this VS only transforms them and
// emits the clip-space positions for current and previous camera (motion
// vectors for FSR) plus the view-space normal.  Rendering these tiles into
// the depth pre-pass keeps downstream passes (contact shadows, HiZ,
// occlusion) and FSR velocity consistent with the surface the scene pass
// draws.

layout(push_constant, std430) uniform HeightmapPC {
    vec4 tile;   // x = originX, y = originZ, z = sizeMeters, w = gridSegments
    vec4 flags;  // x = heightScale, y = texDim (TEX), z/w unused here
} pc;

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec4 outClipCurrent;
layout(location = 1) out vec3 outViewNormal;
layout(location = 2) out vec4 outClipPrev;

void main() {
    vec4 worldPos4 = vec4(inPos, 1.0);
    gl_Position    = sceneBuffer.cameras[0].viewProjection * worldPos4;
    outClipCurrent = sceneBuffer.cameras[0].viewProjectionNoJitter * worldPos4;
    outClipPrev    = sceneBuffer.cameras[0].prevViewProjectionNoJitter * worldPos4;
    outViewNormal  = normalize((sceneBuffer.cameras[0].view * vec4(inNormal, 0.0)).xyz);
}
