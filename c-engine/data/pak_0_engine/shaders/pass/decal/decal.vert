#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_ARB_shading_language_include : enable

#include "../../includes/globalset.shader"

struct DecalGpu {
    mat4 model;
    mat4 invModel;
    vec4 color;
    vec4 params0;
    vec4 params1;
    uvec4 params2;
};

layout(buffer_reference, std430) buffer DecalBufferRef {
    DecalGpu decals[];
};

layout(push_constant) uniform PushConstants {
    uint64_t decalsAddress;
    uint decalCount;
    uint depthIndex;
    uint normalsIndex;
    uint width;
    uint height;
} pc;

layout(location = 0) flat out uint outDecalIndex;
layout(location = 1) out vec3 outCubeLocal;

vec3 cubeVertex(uint i) {
    const vec3 v[36] = vec3[](
        vec3(-1,-1,-1), vec3( 1,-1,-1), vec3( 1, 1,-1), vec3(-1,-1,-1), vec3( 1, 1,-1), vec3(-1, 1,-1),
        vec3( 1,-1, 1), vec3(-1,-1, 1), vec3(-1, 1, 1), vec3( 1,-1, 1), vec3(-1, 1, 1), vec3( 1, 1, 1),
        vec3(-1,-1, 1), vec3(-1,-1,-1), vec3(-1, 1,-1), vec3(-1,-1, 1), vec3(-1, 1,-1), vec3(-1, 1, 1),
        vec3( 1,-1,-1), vec3( 1,-1, 1), vec3( 1, 1, 1), vec3( 1,-1,-1), vec3( 1, 1, 1), vec3( 1, 1,-1),
        vec3(-1,-1, 1), vec3( 1,-1, 1), vec3( 1,-1,-1), vec3(-1,-1, 1), vec3( 1,-1,-1), vec3(-1,-1,-1),
        vec3(-1, 1,-1), vec3( 1, 1,-1), vec3( 1, 1, 1), vec3(-1, 1,-1), vec3( 1, 1, 1), vec3(-1, 1, 1)
    );
    return v[i];
}

void main() {
    outDecalIndex = gl_InstanceIndex;
    outCubeLocal = cubeVertex(gl_VertexIndex);
    DecalBufferRef decalBuffer = DecalBufferRef(pc.decalsAddress);
    vec4 world = decalBuffer.decals[gl_InstanceIndex].model * vec4(outCubeLocal, 1.0);
    gl_Position = sceneBuffer.cameras[0].viewProjectionNoJitter * world;
}
