#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_ARB_shading_language_include : enable

// Simplified triangle fragment shader for planar reflection pass.
// Skips: shadows, AO, forward+ lights, clearcoat, sheen, transmission.
// Outputs only color (no normals/material G-buffer).

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec2 inUV;
layout(location = 2) in flat uint inMaterialId;
layout(location = 3) in vec4 inTangent;
layout(location = 4) in vec3 inWorldPos;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    uint64_t positionBufferAddress;
    uint64_t attributeBufferAddress;
    uint64_t indexBufferAddress;
    uint64_t triangleInstanceBufferAddress;
    uint64_t culledBufferAddress;
    uint64_t drawCountBufferAddress;
    uint64_t transformBufferAddress;
    uint64_t jointMatrixBufferAddress;
    uint64_t entitySkinMapBufferAddress;
    uint64_t prevJointMatrixBufferAddress;
    uint maxTriangleInstances;
};

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

vec4 sampleMaterialTexture(uint texIndex, uint samplerIndex, vec2 uv) {
    return texture(sampler2D(textures[nonuniformEXT(texIndex)],
                             samplers[nonuniformEXT(samplerIndex)]),
                   uv);
}

void main() {
    // Discard ground-plane fragments: the floor at y≈0 is closer to the
    // mirrored camera than the reflected objects and would occlude them.
    if (inWorldPos.y < 0.05) discard;

    Material material = materialBuffer.materials[inMaterialId];

    vec4 baseColor = material.baseColor;
    if ((material.featureMask & (1u << MAT_HAS_TEXTURE_COLOR)) != 0u) {
        vec2 uv = inUV * material.baseColorOffsetScale.zw +
                  material.baseColorOffsetScale.xy;
        baseColor *= sampleMaterialTexture(material.colorTexture,
                                           material.colorTextureSampler, uv);
    }

    if ((material.featureMask & (1u << MAT_ALPHA_MASK)) != 0u) {
        float cutoff = material.rmas.z;
        float a      = baseColor.a;
        if (stochasticAlphaDiscard(a, cutoff, gl_FragCoord.xy,
                                   sceneBuffer.cameras[0].frameIndex))
            discard;
    }

    float roughness = material.rmas.x;
    float metallic  = material.rmas.y;
    if ((material.featureMask & (1u << MAT_HAS_TEXTURE_ROUGHNESS_METALLIC)) != 0u) {
        vec2 uv = inUV * material.rmaOffsetScale.zw + material.rmaOffsetScale.xy;
        vec4 rm = sampleMaterialTexture(material.rmTexture, material.rmTextureSampler, uv);
        roughness *= rm.g;
        metallic  *= rm.b;
    }
    roughness = clamp(roughness, 0.04, 1.0);
    metallic  = clamp(metallic, 0.0, 1.0);

    vec3 N = normalize(inNormal);
    if ((material.featureMask & (1u << MAT_IS_DOUBLE_SIDED)) != 0u) {
        if (!gl_FrontFacing) N = -N;
    }
    // Normal mapping disabled in reflection pass – use geometric normal
    // only so that normal-strength doesn't distort the reflection.

    vec3 V = normalize(sceneBuffer.cameras[0].position.xyz - inWorldPos);
    float NdotV = max(dot(N, V), 0.001);

    float ior = 1.5;
    if ((material.featureMask & (1u << MAT_HAS_IOR)) != 0u) ior = material.specularColorIor.w;
    float f0_dielectric = pow((ior - 1.0) / (ior + 1.0), 2.0);
    vec3 F0 = mix(vec3(f0_dielectric), baseColor.rgb, metallic);

    // Directional light (unshadowed)
    DirectionalLight dirLight = sceneBuffer.directionalLight;
    vec3 lightDir = normalize(dirLight.direction.xyz);
    vec3 lightColor = dirLight.color.rgb * dirLight.direction.w;
    vec3 L = -lightDir;
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    float D = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    vec3  F = fresnelSchlick(HdotV, F0);

    vec3 specular = (D * G * F) / (4.0 * NdotV * NdotL + 0.0001);
    vec3 kD = (1.0 - F) * (1.0 - metallic);
    vec3 Lo = (kD * baseColor.rgb / PI + specular) * lightColor * NdotL;

    // No ambient / IBL: the mirrored render is lit by the direct (unshadowed)
    // sun only, so shadowed reflections stay dark.
    vec3 color = Lo;

    vec3 emissive = vec3(0.0);
    if ((material.featureMask & (1u << MAT_HAS_EMISSIVE_FACTOR)) != 0u) {
        emissive = material.emissive.rgb * material.emissive.w;
    }
    if ((material.featureMask & (1u << MAT_HAS_TEXTURE_EMISSIVE)) != 0u) {
        vec2 uv = inUV * material.emissionOffsetScale.zw + material.emissionOffsetScale.xy;
        emissive *= sampleMaterialTexture(material.emissiveTexture, material.emissiveTextureSampler, uv).rgb;
    }
    color += emissive;

    float alpha = baseColor.a;
    if ((material.featureMask & (1u << MAT_ALPHA_OPAQUE)) != 0u) alpha = 1.0;

    if (any(isnan(color)) || any(isinf(color))) color = vec3(0.0);
    color = clamp(color, vec3(0.0), vec3(65504.0));

    outColor = vec4(color, alpha);
}
