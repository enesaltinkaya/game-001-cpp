#version 460
#extension GL_ARB_shading_language_include : enable

layout(location = 0) out vec2 outUV;

void main() {
    vec2 position;

    if (gl_VertexIndex == 0) {
        position = vec2(-1.0, -1.0);
    } else if (gl_VertexIndex == 1) {
        position = vec2(3.0, -1.0);
    } else {
        position = vec2(-1.0, 3.0);
    }

    outUV = position * 0.5 + 0.5;

    /* Reverse-Z far plane maps to depth 0.0. */
    gl_Position = vec4(position, 0.0, 1.0);
}
