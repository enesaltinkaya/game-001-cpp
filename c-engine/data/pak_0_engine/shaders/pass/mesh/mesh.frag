#version 460

layout(location = 0) in vec3 outColor;
layout(location = 0) out vec4 finalColor;

void main() {
    finalColor = vec4(outColor, 1.0);
}
