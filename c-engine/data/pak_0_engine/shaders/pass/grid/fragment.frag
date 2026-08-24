#version 450
#extension GL_EXT_buffer_reference : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_ARB_shading_language_include : enable
#extension GL_ARB_gpu_shader_int64 : enable

#include "../../includes/globalset.shader"

layout(location = 0) in vec3 worldPos;

layout(location = 0) out vec4 finalColor;

void main() {
    finalColor = vec4(vec3(0.4, 0.4, 0.4), 1.0);
}
