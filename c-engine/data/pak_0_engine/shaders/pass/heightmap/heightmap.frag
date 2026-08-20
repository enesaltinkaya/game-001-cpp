#version 450
precision highp float;
// Heightmap bake fragment shader — outputs Y position as height
layout(location = 0) in vec3 vPos;
layout(location = 0) out float oHeight;

void main() {
    oHeight = vPos.y;
}
