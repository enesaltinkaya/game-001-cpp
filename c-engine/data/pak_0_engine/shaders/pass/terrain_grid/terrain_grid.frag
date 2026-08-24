#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_ARB_shading_language_include : enable
precision highp float;

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec3 inWorldPos;
layout(location = 4) flat in uint inAzgaarCellId;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outNormal;
layout(location = 2) out vec4 outMaterial;

layout(push_constant) uniform PushConstants {
    uint materialId;
    uint wireFrame;
};

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

void main() {
    vec3 N = normalize(inNormal);
    vec3 sunDir = normalize(-sceneBuffer.directionalLight.direction.xyz);
    float ndl = max(dot(N, sunDir), 0.0);

    float h = clamp((inWorldPos.y - sceneBuffer.terrain.worldMin.y) /
                    max(sceneBuffer.terrain.worldMax.y - sceneBuffer.terrain.worldMin.y, 1.0),
                    0.0, 1.0);
    vec3 low  = vec3(0.18, 0.38, 0.13);
    vec3 mid  = vec3(0.34, 0.30, 0.20);
    vec3 high = vec3(0.46, 0.46, 0.43);
    vec3 albedo = mix(low, mid, smoothstep(0.18, 0.45, h));
    albedo = mix(albedo, high, smoothstep(0.50, 0.85, h));

    vec3 ambient = albedo * 0.35;
    vec3 lit = albedo * (0.45 + 0.55 * ndl) * max(sceneBuffer.directionalLight.color.rgb, vec3(1.0));
    vec3 color = max(ambient + lit, albedo * 0.8);

    if (wireFrame != 0u) color = vec3(1.0, 0.0, 0.0);

    outColor = vec4(max(color, vec3(0.08, 0.28, 0.05)), 1.0);
    outNormal = OctEncode(N);
    outMaterial = vec4(0.85, 0.0, 0.0, 1.0);
}
