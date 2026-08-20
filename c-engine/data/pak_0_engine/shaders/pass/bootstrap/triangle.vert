#version 460

layout(location = 0) out vec3 outColor;

void main() {
    vec2 position;
    vec3 color;

    if (gl_VertexIndex == 0) {
        position = vec2(0.0, -0.6);
        color = vec3(1.0, 0.2, 0.2);
    } else if (gl_VertexIndex == 1) {
        position = vec2(0.6, 0.6);
        color = vec3(0.2, 1.0, 0.2);
    } else {
        position = vec2(-0.6, 0.6);
        color = vec3(0.2, 0.4, 1.0);
    }

    outColor = color;
    gl_Position = vec4(position, 0.0, 1.0);
}
