#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
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
#include "../../includes/ibl_common.shader"

// ------------------------------------------------------------------
// IBL sampling (envRotation rotates sample directions so the IBL stays in
// sync with the rotated extracted sun). Diffuse-only — terrain is matte.
// Keep matched with scene.frag.
// ------------------------------------------------------------------
vec3 sampleEnvironment(vec3 dir, float lod) {
    dir = mat3(sceneBuffer.ibl.envRotation) * dir;
    return textureLod(sampler2D(textures[nonuniformEXT(sceneBuffer.ibl.environmentMapIndex)],
                                samplers[SAMPLER_LINEAR]),
                      directionToEquirectUv(dir),
                      clamp(lod, 0.0, sceneBuffer.ibl.environmentMapMaxLod))
        .rgb;
}

vec3 sampleIrradiance(vec3 dir) {
    dir = mat3(sceneBuffer.ibl.envRotation) * dir;
    return min(texture(samplerCube(cubeTextures[nonuniformEXT(sceneBuffer.ibl.irradianceMapIndex)],
                                   samplers[SAMPLER_LINEAR]),
                       normalize(dir))
                   .rgb,
               vec3(32.0));
}

vec3 evaluateSHIrradiance(vec3 N) {
    vec3 rN         = mat3(sceneBuffer.ibl.envRotation) * N;
    const float A0  = PI;
    const float A1  = 2.0 * PI / 3.0;
    const float Y00 = 0.282095;
    const float Y1x = 0.488603;
    vec3 irr        = vec3(0.0);
    irr += sceneBuffer.ibl.shL0_M0.rgb * A0 * Y00;
    irr += sceneBuffer.ibl.shL1_Mp1.rgb * A1 * Y1x * rN.x;
    irr += sceneBuffer.ibl.shL1_Mn1.rgb * A1 * Y1x * rN.y;
    irr += sceneBuffer.ibl.shL1_M0.rgb * A1 * Y1x * rN.z;
    return max(irr, vec3(0.0));
}

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

    // Ambient / IBL — keep matched with scene.frag's ambient/IBL block
    // (diffuse-only: this pass is matte and unshadowed).  The constant
    // albedo * 0.35 fill is the fallback when IBL is disabled.
    vec3 ambient = albedo * 0.35;
    if (sceneBuffer.ibl.enabled != 0u) {
        vec3 irradiance;
        if (sceneBuffer.ibl.irradianceMapIndex != 0u)
            irradiance = sampleIrradiance(N);
        else if (sceneBuffer.ibl.hasSH != 0u)
            irradiance = evaluateSHIrradiance(N);
        else
            irradiance = sampleEnvironment(N, sceneBuffer.ibl.environmentMapMaxLod);
        ambient = irradiance * albedo / PI * sceneBuffer.ibl.intensity;
    }
    vec3 lit = albedo * (0.45 + 0.55 * ndl) * max(sceneBuffer.directionalLight.color.rgb, vec3(1.0));
    vec3 color = max(ambient + lit, albedo * 0.8);

    if (wireFrame != 0u) color = vec3(1.0, 0.0, 0.0);

    outColor = vec4(max(color, vec3(0.08, 0.28, 0.05)), 1.0);
    outNormal = OctEncode(N);
    outMaterial = vec4(0.85, 0.0, 0.0, 1.0);
}
