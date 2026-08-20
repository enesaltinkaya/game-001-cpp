#version 460

layout(location = 0) in vec4 inColor;

layout(location = 0) out vec4 outColor;

void main() {
    // Dark edges with full opacity
    outColor = vec4(0.0, 0.0, 0.0, 0.7);
}
