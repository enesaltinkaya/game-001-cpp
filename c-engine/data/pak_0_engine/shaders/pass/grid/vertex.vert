#version 450
#extension GL_EXT_buffer_reference : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_ARB_shading_language_include : enable
#extension GL_ARB_gpu_shader_int64 : enable

#include "../../includes/globalset.shader"

struct GridVertex {
    vec3 position;
    float _pad;
};

layout(buffer_reference) readonly buffer VertexBufferRef {
    GridVertex vertices[];
};

layout(push_constant) uniform PushConstants {
    VertexBufferRef vertexBuffer;
};

layout(location = 0) out vec3 worldPos;

void main() {
    GridVertex vertex = vertexBuffer.vertices[gl_VertexIndex];
    worldPos = vertex.position;

    gl_Position = sceneBuffer.cameras[0].viewProjection * vec4(worldPos, 1.0);
}
