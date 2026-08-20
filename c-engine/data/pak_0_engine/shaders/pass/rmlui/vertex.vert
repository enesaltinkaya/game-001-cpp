#version 450
#extension GL_EXT_buffer_reference : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_ARB_shading_language_include : enable
#extension GL_ARB_gpu_shader_int64 : enable

#include "../../includes/globalset.shader"

struct RmlVertex {
    float x, y;
    int color;
    float u, v;
};

struct RmlInstanceData {
    mat4 transform;
    vec2 translation;
    int textureId;
    int padding;
};

layout(buffer_reference) readonly buffer VertexBufferRef {
    RmlVertex vertices[];
};  

layout(buffer_reference) readonly buffer RmlInstanceDataRef {
    RmlInstanceData data[];
};

layout(push_constant) uniform puchConstants {
    VertexBufferRef vertexBuffer;
    RmlInstanceDataRef instanceData;
};

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec4 fragColor;
layout(location = 2) out flat int textureId;

void main() {
    RmlVertex vertex = vertexBuffer.vertices[gl_VertexIndex];
    fragTexCoord     = vec2(vertex.u, vertex.v);

    vec2 translatedPos = vec2(vertex.x, vertex.y) + instanceData.data[gl_InstanceIndex].translation;
    gl_Position        = instanceData.data[gl_InstanceIndex].transform * vec4(translatedPos, 0, 1);
    textureId          = instanceData.data[gl_InstanceIndex].textureId;

    float a   = (vertex.color >> 24) & 0xFF;
    float b   = (vertex.color >> 16) & 0xFF;
    float g   = (vertex.color >> 8) & 0xFF;
    float r   = vertex.color & 0xFF;
    fragColor = vec4(r / 254., g / 254., b / 254., a / 254.);
}
