#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_ARB_shading_language_include : enable

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inUV;

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

layout(location = 0) out vec4 outClipCurrent;
layout(location = 1) out vec3 outViewNormal;
layout(location = 2) out vec4 outClipPrev;

void main() {
    vec3 worldPos     = inPosition;
    vec3 prevWorldPos = inPosition;

    gl_Position    = sceneBuffer.cameras[0].viewProjection * vec4(worldPos, 1.0);
    outClipCurrent = sceneBuffer.cameras[0].viewProjectionNoJitter * vec4(worldPos, 1.0);
    outClipPrev    = sceneBuffer.cameras[0].prevViewProjectionNoJitter * vec4(prevWorldPos, 1.0);
    outViewNormal  = normalize((sceneBuffer.cameras[0].view * vec4(inNormal, 0.0)).xyz);
}
