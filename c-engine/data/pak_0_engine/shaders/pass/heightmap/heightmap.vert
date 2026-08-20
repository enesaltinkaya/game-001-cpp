#version 450
// Heightmap bake vertex shader — passes position through
// Input: world-space position in attribute 0 (vec3)
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aTangent;
layout(location = 3) in vec2 aUV;

layout(location = 0) out vec3 vPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec4 vTangent;
layout(location = 3) out vec2 vUV;

// Orthographic camera push constants
layout(push_constant, std430) uniform HeightmapPC {
    mat4 orthoProj;
    vec4 boundsMin;  // xyz = min, w = unused
    vec4 boundsMax;  // xyz = max, w = unused
    vec4 cameraPos;  // xyz = camera position (top center)
};

void main() {
    vPos    = aPos;
    vNormal = normalize(aNormal);
    vTangent = aTangent;
    vUV     = aUV;

    // For heightmap baking, we use an orthographic projection
    // Camera looks straight down (-Y), positioned above terrain center
    vec3 pos = aPos;
    vec4 clipPos = orthoProj * vec4(pos, 1.0);
    gl_Position = clipPos;
}
