#version 460
#extension GL_ARB_shading_language_include : enable

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec2 outColor;

#include "../../includes/utils.shader"
#include "../../includes/ibl_common.shader"

// IBL-specific geometry functions using k = roughness² / 2
// (different from direct lighting which uses k = (roughness + 1)² / 8)
float GeometrySchlickGGX_IBL(float NdotV, float roughness) {
    float k   = (roughness * roughness) / 2.0;
    float nom = NdotV;
    float den = NdotV * (1.0 - k) + k;
    return nom / den;
}

float GeometrySmith_IBL(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX_IBL(NdotV, roughness)
         * GeometrySchlickGGX_IBL(NdotL, roughness);
}

vec2 IntegrateBRDF(float NdotV, float roughness) {
    vec3 V;
    V.x = sqrt(max(1.0 - NdotV * NdotV, 0.0));
    V.y = 0.0;
    V.z = NdotV;

    float A = 0.0;
    float B = 0.0;

    vec3 N = vec3(0.0, 0.0, 1.0);

    const uint SAMPLE_COUNT = 1024u;
    for (uint i = 0u; i < SAMPLE_COUNT; i++) {
        vec2 Xi = Hammersley(i, SAMPLE_COUNT);
        vec3 H  = ImportanceSampleGGX(Xi, N, roughness);
        vec3 L  = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(L.z, 0.0);
        float NdotH = max(H.z, 0.0);
        float VdotH = max(dot(V, H), 0.0);

        if (NdotL > 0.0) {
            float G     = GeometrySmith_IBL(N, V, L, roughness);
            float G_Vis = (G * VdotH) / max(NdotH * NdotV, 0.0001);
            float Fc    = pow(1.0 - VdotH, 5.0);

            A += (1.0 - Fc) * G_Vis;
            B += Fc * G_Vis;
        }
    }

    return vec2(A, B) / float(SAMPLE_COUNT);
}

void main() {
    vec2 uv = vec2(inUV.x, 1.0 - inUV.y);
    outColor = IntegrateBRDF(clamp(uv.x, 0.0, 1.0), clamp(uv.y, 0.0, 1.0));
}
