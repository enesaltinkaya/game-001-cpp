#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_ARB_shading_language_include : enable

// Indexed-corner vertex shader for the heightmap terrain pass.
//
// No implicit lattice enumeration and no height-texel fetches any more: the
// tile's render lattice is precomputed on the CPU at height-upload time
// (VulkanHeightmapTerrainPass: 256^2 corners of (worldPos, normal) in a
// per-tile VBO + a shared 255-segment IBO, geometry-identical to the old
// implicit surface — same cell math, same texel-centre bilinear heights,
// same border-aware one-sided stencil normals).  This VS is a thin
// transform of the corner attributes; the fragment shader still refines
// the normal with micro-band procedural perturbation.
//
// The push constant block is kept for pipeline-layout compatibility (the
// CPU still pushes origin/size/segments; the VS no longer samples the
// height texture, but the tile's set1 descriptor stays bound).

layout(push_constant, std430) uniform HeightmapPC {
    vec4 tile;   // x = originX, y = originZ, z = sizeMeters, w = gridSegments
    vec4 flags;  // x = heightScale, y = texDim (TEX, e.g. 512),
                 // z = wireFrame, w = debugHeightRamp
} pc;

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec3 outWorldPos;

void main() {
    vec3 worldPos = inPos;
    gl_Position = sceneBuffer.cameras[0].viewProjection * vec4(worldPos, 1.0);
    outNormal   = inNormal;
    outWorldPos = worldPos;
}
