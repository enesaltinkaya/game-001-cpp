#version 460

layout(location = 0) in vec4 inPos;     // xyz = world pos, w = 1.0
layout(location = 1) in vec4 inColor;   // rgba 0-1

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    uint vertexCount;
    uint pad[3];
};

void main() {
    vec3 localPos = inPos.xyz;
    gl_Position = viewProjection * vec4(localPos, 1.0);
    outColor = inColor;
}
