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
#include "../../includes/ibl_common.shader"

vec4 sampleMaterialTexture(uint texIndex, uint samplerIndex, vec2 uv) {
    return texture(sampler2D(textures[nonuniformEXT(texIndex)],
                             samplers[nonuniformEXT(samplerIndex)]),
                   uv);
}

// ------------------------------------------------------------------
// IBL sampling (envRotation rotates sample directions so the IBL stays in
// sync with the rotated extracted sun). Keep matched with triangle.frag.
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

vec3 samplePrefilter(vec3 dir, float lod) {
    dir = mat3(sceneBuffer.ibl.envRotation) * dir;
    return textureLod(samplerCube(cubeTextures[nonuniformEXT(sceneBuffer.ibl.prefilterMapIndex)],
                                  samplers[SAMPLER_LINEAR]),
                      normalize(dir),
                      clamp(lod, 0.0, sceneBuffer.ibl.prefilterMapMaxLod))
        .rgb;
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

    // Ambient / IBL — keep matched with triangle.frag's ambient/IBL block.
    // The reflection pass renders unshadowed, so no shadow darkening.
    vec3 ambientDiffuse  = vec3(0.0);
    vec3 ambientSpecular = vec3(0.0);
    if (sceneBuffer.ibl.enabled != 0u) {
        vec3 R                  = reflect(-V, N);
        float iblIntensity      = sceneBuffer.ibl.intensity;
        float iblSpecIntensity  = sceneBuffer.ibl.specularIntensity;
        float maxLod            = sceneBuffer.ibl.prefilterMapMaxLod;
        float specLod           = sqrt(roughness) * maxLod;

        if (sceneBuffer.ibl.prefilterMapIndex != 0u && sceneBuffer.ibl.brdfLutIndex != 0u) {
            vec3 prefilteredColor = samplePrefilter(R, specLod);
            vec2 brdf = texture(sampler2D(textures[nonuniformEXT(sceneBuffer.ibl.brdfLutIndex)],
                                          samplers[SAMPLER_CLAMP_LINEAR]),
                                vec2(NdotV, roughness))
                            .rg;
            vec3 specFactor = F0 * brdf.x + brdf.y;
            vec3 kD_ibl     = (1.0 - specFactor) * (1.0 - metallic);

            ambientSpecular = prefilteredColor * specFactor * iblIntensity * iblSpecIntensity;

            vec3 irradiance;
            if (sceneBuffer.ibl.irradianceMapIndex != 0u)
                irradiance = sampleIrradiance(N);
            else if (sceneBuffer.ibl.hasSH != 0u)
                irradiance = evaluateSHIrradiance(N);
            else
                irradiance = sampleEnvironment(N, sceneBuffer.ibl.environmentMapMaxLod);
            ambientDiffuse = kD_ibl * irradiance * baseColor.rgb / PI * iblIntensity;
        }
    }

    vec3 color = ambientDiffuse + ambientSpecular + Lo;

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
