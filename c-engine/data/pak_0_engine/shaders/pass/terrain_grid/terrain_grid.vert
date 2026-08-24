#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_ARB_shading_language_include : enable
precision highp float;

// Terrain grid vertex shader — generates a dense screen-visible terrain mesh
// and displaces it from the heightmap baked from the currently visible Azgaar
// source triangles.
layout(set = 1, binding = 0) uniform sampler2D heightmap;

#include "../../includes/globalset.shader"

layout(push_constant, std430) uniform TerrainGridPC {
    uint materialId; // kept ABI-compatible with terrain.frag push constants
    uint wireFrame;
    uint _pad0;
    uint _pad1;
    vec4 boundsMin;  // xyz = min world bounds
    vec4 boundsMax;  // xyz = max world bounds
    mat4 viewProjection;
};

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec2 outUV;
layout(location = 2) out vec4 outTangent;
layout(location = 3) out vec3 outWorldPos;
layout(location = 4) flat out uint outAzgaarCellId;

#define AZGAAR_INVALID_CELL_ID 0xffffffffu
#define GRID_RES 256u

vec2 vertexUv(uint vertexIndex) {
    uint triVertex = vertexIndex % 6u;
    uint cell      = vertexIndex / 6u;
    uint x         = cell % GRID_RES;
    uint y         = cell / GRID_RES;

    vec2 base = vec2(x, y) / vec2(GRID_RES);
    vec2 step = vec2(1.0 / float(GRID_RES));

    if (triVertex == 0u) return base;
    if (triVertex == 1u) return base + vec2(step.x, 0.0);
    if (triVertex == 2u) return base + vec2(0.0, step.y);
    if (triVertex == 3u) return base + vec2(step.x, 0.0);
    if (triVertex == 4u) return base + step;
    return base + vec2(0.0, step.y);
}

void main() {
    vec2 uv = vertexUv(uint(gl_VertexIndex));

    float h = texture(heightmap, uv).r;
    vec2 texel = 1.0 / vec2(textureSize(heightmap, 0));
    float hL = texture(heightmap, clamp(uv - vec2(texel.x, 0.0), vec2(0.0), vec2(1.0))).r;
    float hR = texture(heightmap, clamp(uv + vec2(texel.x, 0.0), vec2(0.0), vec2(1.0))).r;
    float hD = texture(heightmap, clamp(uv - vec2(0.0, texel.y), vec2(0.0), vec2(1.0))).r;
    float hU = texture(heightmap, clamp(uv + vec2(0.0, texel.y), vec2(0.0), vec2(1.0))).r;

    float x = mix(boundsMin.x, boundsMax.x, uv.x);
    float z = mix(boundsMin.z, boundsMax.z, uv.y);
    vec3 worldPos = vec3(x, h, z);

    float dx = max((boundsMax.x - boundsMin.x) * texel.x * 2.0, 1e-4);
    float dz = max((boundsMax.z - boundsMin.z) * texel.y * 2.0, 1e-4);
    vec3 n = normalize(vec3(-(hR - hL) / dx, 1.0, -(hU - hD) / dz));

    outNormal       = n;
    outUV           = worldPos.xz * (1.0 / 64.0);
    outTangent      = vec4(1.0, 0.0, 0.0, 1.0);
    outWorldPos     = worldPos;
    outAzgaarCellId = AZGAAR_INVALID_CELL_ID;

    gl_Position = viewProjection * vec4(worldPos, 1.0);
}
