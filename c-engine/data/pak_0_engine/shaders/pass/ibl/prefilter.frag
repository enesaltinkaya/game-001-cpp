#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_ARB_shading_language_include : enable

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    uint environmentMapIndex;
    uint faceIndex;
    float roughness;
    float pad;
};

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"
#include "../../includes/ibl_common.shader"

// Sun-threshold clamping: excess energy is handled by the extracted sun light.
const float SUN_THRESHOLD = 10.0;

vec3 sampleEnvironmentLod(vec3 dir, float lod) {
    vec3 radiance = textureLod(sampler2D(textures[nonuniformEXT(environmentMapIndex)],
                                         samplers[SAMPLER_LINEAR]),
                               directionToEquirectUv(dir),
                               max(lod, 0.0))
                        .rgb;
    float lum = dot(radiance, vec3(0.2126, 0.7152, 0.0722));
    if (lum > SUN_THRESHOLD) {
        radiance *= SUN_THRESHOLD / lum;
    }
    return radiance;
}

void main() {
    vec2 uv = inUV * 2.0 - 1.0;
    vec3 N  = cubemapDirection(faceIndex, uv);
    vec3 R  = N;
    vec3 V  = R;

    // For roughness 0, just sample the environment directly
    if (roughness < 0.001) {
        outColor = vec4(sampleEnvironmentLod(N, 0.0), 1.0);
        return;
    }

    const uint SAMPLE_COUNT = 512u;
    vec3 prefilteredColor   = vec3(0.0);
    float totalWeight       = 0.0;

    // Environment map resolution for PDF-based mip selection.
    ivec2 envSize = textureSize(
        sampler2D(textures[nonuniformEXT(environmentMapIndex)],
                  samplers[SAMPLER_LINEAR]), 0);
    float saTexel = 4.0 * PI / (float(envSize.x) * float(envSize.y));

    for (uint i = 0u; i < SAMPLE_COUNT; i++) {
        vec2 xi = Hammersley(i, SAMPLE_COUNT);
        vec3 H  = ImportanceSampleGGX(xi, N, roughness);
        vec3 L  = normalize(2.0 * dot(V, H) * H - V);

        float NdotH = max(dot(N, H), 0.0);
        float HdotV = max(dot(H, V), 0.0);
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0) {
            float D   = DistributionGGX(N, H, roughness);
            float pdf = D * NdotH / (4.0 * HdotV) + 0.0001;
            float saSample = 1.0 / (float(SAMPLE_COUNT) * pdf + 0.0001);
            float mipLevel = roughness == 0.0
                                 ? 0.0
                                 : 0.5 * log2(saSample / saTexel);

            prefilteredColor += sampleEnvironmentLod(L, mipLevel) * NdotL;
            totalWeight += NdotL;
        }
    }

    prefilteredColor /= max(totalWeight, 0.001);
    outColor          = vec4(prefilteredColor, 1.0);
}
