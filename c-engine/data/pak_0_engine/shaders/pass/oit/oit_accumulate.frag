#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_ARB_shading_language_include : enable

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec2 inUV;
layout(location = 2) in flat uint inMaterialId;
layout(location = 3) in vec4 inTangent;
layout(location = 4) in vec3 inWorldPos;
layout(location = 5) in flat uint inEntity;

layout(location = 0) out vec4 outAccum;   // RGBA16F additive
layout(location = 1) out float outReveal;  // R8 multiplicative

layout(push_constant) uniform PushConstants {
    uint64_t transformBufferAddress;
    uint64_t drawInstanceBufferAddress;
    uint64_t culledBufferAddress;
};

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"
#include "../../includes/ibl_common.shader"

#include "../../includes/shadow.shader"
#include "../../includes/forwardplus.shader"

// ------------------------------------------------------------------
// Texture sampling helpers (same as triangle.frag)
// ------------------------------------------------------------------
vec4 sampleMaterialTexture(uint texIndex, uint samplerIndex, vec2 uv) {
    return texture(sampler2D(textures[nonuniformEXT(texIndex)],
                             samplers[nonuniformEXT(samplerIndex)]),
                   uv);
}

// ------------------------------------------------------------------
// IBL sampling (envRotation rotates sample directions so the IBL stays in
// sync with the rotated extracted sun)
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

// ------------------------------------------------------------------
// Anisotropic GGX
// ------------------------------------------------------------------
float DistributionGGXAnisotropic(vec3 N, vec3 H, vec3 T, vec3 B,
                                 float roughness, float anisotropy) {
    float at = max(roughness * (1.0 + anisotropy), 0.001);
    float ab = max(roughness * (1.0 - anisotropy), 0.001);
    float TdotH = dot(T, H);
    float BdotH = dot(B, H);
    float NdotH = dot(N, H);
    float a2 = at * ab;
    vec3 v   = vec3(ab * TdotH, at * BdotH, a2 * NdotH);
    float v2 = dot(v, v);
    float w2 = a2 / v2;
    return a2 * w2 * w2 * (1.0 / PI);
}

// ------------------------------------------------------------------
// Main
// ------------------------------------------------------------------
void main() {
    Material material = materialBuffer.materials[inMaterialId];

    vec4 baseColor = material.baseColor;
    if ((material.featureMask & (1u << MAT_HAS_TEXTURE_COLOR)) != 0u) {
        vec2 uv = inUV * material.baseColorOffsetScale.zw +
                  material.baseColorOffsetScale.xy;
        baseColor *= sampleMaterialTexture(material.colorTexture,
                                           material.colorTextureSampler, uv);
    }

    float roughness = material.rmas.x;
    float metallic  = material.rmas.y;
    if ((material.featureMask & (1u << MAT_HAS_TEXTURE_ROUGHNESS_METALLIC)) != 0u) {
        vec2 uv = inUV * material.rmaOffsetScale.zw + material.rmaOffsetScale.xy;
        vec4 rm = sampleMaterialTexture(material.rmTexture, material.rmTextureSampler, uv);
        roughness *= rm.g;
        metallic *= rm.b;
    }
    roughness = clamp(roughness, 0.04, 1.0);
    metallic  = clamp(metallic, 0.0, 1.0);

    vec3 N = normalize(inNormal);
    if ((material.featureMask & (1u << MAT_HAS_TEXTURE_NORMAL)) != 0u) {
        vec3 T   = normalize(inTangent.xyz);
        vec3 B   = cross(N, T) * inTangent.w;
        mat3 TBN = mat3(T, B, N);
        vec2 uv = inUV * material.normalOffsetScale.zw + material.normalOffsetScale.xy;
        vec3 normalMap = sampleMaterialTexture(material.normalTexture,
                                               material.normalTextureSampler, uv).rgb;
        normalMap = normalMap * 2.0 - 1.0;
        vec3 mappedN = normalize(TBN * normalMap);
        N = normalize(mix(N, mappedN, material.rmas.w));
    }

    vec3 V      = normalize(sceneBuffer.cameras[0].position.xyz - inWorldPos);
    float NdotV = max(dot(N, V), 0.001);

    float ior = 1.5;
    if ((material.featureMask & (1u << MAT_HAS_IOR)) != 0u) {
        ior = material.specularColorIor.w;
    }
    float f0_dielectric = pow((ior - 1.0) / (ior + 1.0), 2.0);
    vec3 F0 = mix(vec3(f0_dielectric), baseColor.rgb, metallic);

    // Specular override (KHR_materials_specular)
    if ((material.featureMask & (1u << MAT_HAS_SPECULAR)) != 0u) {
        float specularFactor = material.specularAnisotropyRotation.x;
        vec3 specularColor   = material.specularColorIor.xyz;
        vec3 dielectricF0 = vec3(f0_dielectric) * specularFactor * specularColor;
        F0 = mix(dielectricF0, baseColor.rgb, metallic);
    }

    vec3 T_aniso = normalize(inTangent.xyz);
    vec3 B_aniso = normalize(cross(N, T_aniso) * inTangent.w);
    if ((material.featureMask & (1u << MAT_HAS_ANISOTROPY)) != 0u) {
        float rotation = material.specularAnisotropyRotation.y;
        float cosR = cos(rotation);
        float sinR = sin(rotation);
        vec3 T_rot = cosR * T_aniso + sinR * B_aniso;
        vec3 B_rot = -sinR * T_aniso + cosR * B_aniso;
        T_aniso = T_rot;
        B_aniso = B_rot;
    }

    DirectionalLight dirLight = sceneBuffer.directionalLight;
    vec3 lightDir = normalize(dirLight.direction.xyz);
    vec3 lightColor = dirLight.color.rgb * dirLight.direction.w;

    vec3 L      = -lightDir;
    vec3 H      = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    float D;
    float anisotropy = 0.0;
    if ((material.featureMask & (1u << MAT_HAS_ANISOTROPY)) != 0u) {
        anisotropy = material.sheenColorAnisotropy.w;
        D = DistributionGGXAnisotropic(N, H, T_aniso, B_aniso, roughness, anisotropy);
    } else {
        D = DistributionGGX(N, H, roughness);
    }

    float G = GeometrySmith(N, V, L, roughness);
    vec3 F  = fresnelSchlick(HdotV, F0);

    vec3 specular = (D * G * F) / (4.0 * NdotV * NdotL + 0.0001);
    vec3 kS = F;
    vec3 kD = (1.0 - kS) * (1.0 - metallic);

    vec4 shadowFull = sampleShadowFull(inWorldPos, N);
    float contactShadow = sampleContactShadow();
    vec3 shadow = shadowFull.rgb * contactShadow;
    vec3 Lo = (kD * baseColor.rgb / PI + specular) * lightColor * NdotL * shadow;

    Lo += evaluateForwardPlusLights(inWorldPos, N, V, NdotV, F0,
                                    roughness, metallic, baseColor.rgb,
                                    T_aniso, B_aniso, anisotropy);

    float occluderAlphaForEntity = cameraOccluderAlpha(inEntity);

    // Ambient / IBL — keep this matched with scene.frag so an object does
    // not change brightness when it moves from opaque rendering to OIT.
    // Ambient comes exclusively from the IBL resource; no default fill.
    vec3 ambientDiffuse  = vec3(0.0);
    vec3 ambientSpecular = vec3(0.0);
    if (sceneBuffer.ibl.enabled != 0u) {
        vec3 R                 = reflect(-V, N);
        float iblIntensity     = sceneBuffer.ibl.intensity;
        float iblSpecIntensity = sceneBuffer.ibl.specularIntensity;
        float maxLod           = sceneBuffer.ibl.prefilterMapMaxLod;
        float specLod          = sqrt(roughness) * maxLod;

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

    // Clearcoat
    if ((material.featureMask & (1u << MAT_HAS_CLEARCOAT)) != 0u) {
        float clearcoat          = material.clearcoatSheenTransmission.x;
        float clearcoatRoughness = clamp(material.clearcoatSheenTransmission.y, 0.04, 1.0);
        float Dc = DistributionGGX(N, H, clearcoatRoughness);
        float Gc = GeometrySmith(N, V, L, clearcoatRoughness);
        vec3 Fc  = fresnelSchlick(HdotV, vec3(0.04));
        vec3 clearcoatSpecular = (Dc * Gc * Fc) / (4.0 * NdotV * NdotL + 0.0001);
        color = color * (1.0 - clearcoat * Fc) + clearcoatSpecular * clearcoat * NdotL;
    }

    // Sheen
    if ((material.featureMask & (1u << MAT_HAS_SHEEN)) != 0u) {
        vec3 sheenColor      = material.sheenColorAnisotropy.xyz;
        float sheenRoughness = max(material.clearcoatSheenTransmission.z, 0.04);
        float NdotH     = max(dot(N, H), 0.0);
        float sinNdotH2 = 1.0 - NdotH * NdotH;
        float sheenD    = (2.0 + 1.0 / sheenRoughness) *
                       pow(sinNdotH2, 0.5 / sheenRoughness) / (2.0 * PI);
        color += sheenColor * sheenD * NdotL;
    }

    // Transmission
    if ((material.featureMask & (1u << MAT_HAS_TRANSMISSION)) != 0u) {
        float transmission = material.clearcoatSheenTransmission.w;
        vec3 transmittedLight = baseColor.rgb * max(dot(N, -L), 0.0) * transmission;
        if ((material.featureMask & (1u << MAT_HAS_VOLUME)) != 0u) {
            float thickness       = material.volumeFactors.x;
            float attenuationDist = material.volumeFactors.y;
            vec3 attenuationColor = material.volumeColor.xyz;
            if (attenuationDist > 0.0) {
                vec3 absorption = -log(max(attenuationColor, vec3(0.0001))) / attenuationDist;
                transmittedLight *= exp(-absorption * thickness);
            }
        }
        vec3 fresnelTransmission = fresnelSchlick(NdotV, F0);
        color = mix(color, color + transmittedLight, (1.0 - fresnelTransmission) * transmission);
    }

    // Emissive
    vec3 emissive = vec3(0.0);
    if ((material.featureMask & (1u << MAT_HAS_EMISSIVE_FACTOR)) != 0u) {
        emissive = material.emissive.rgb * material.emissive.w;
    }
    if ((material.featureMask & (1u << MAT_HAS_TEXTURE_EMISSIVE)) != 0u) {
        vec2 uv = inUV * material.emissionOffsetScale.zw + material.emissionOffsetScale.xy;
        emissive *= sampleMaterialTexture(material.emissiveTexture, material.emissiveTextureSampler, uv).rgb;
    }
    color += emissive;

    // Prevent NaN/Inf
    if (any(isnan(color)) || any(isinf(color))) color = vec3(0.0);
    color = clamp(color, vec3(0.0), vec3(65504.0));

    float alpha = baseColor.a;
    alpha = min(alpha, occluderAlphaForEntity);

    // WBOIT weight function (McGuire & Bavoil 2013)
    // Linearize reverse-Z depth to camera-space distance for correct weighting.
    float zNear = sceneBuffer.cameras[0].zNear;
    float zFar  = sceneBuffer.cameras[0].zFar;
    float linearZ = zNear * zFar / (zNear + gl_FragCoord.z * (zFar - zNear));
    float weight = alpha * max(1e-2, min(3e3,
        10.0 / (1e-5 + pow(linearZ / 5.0, 2.0) + pow(linearZ / 200.0, 6.0))));

    outAccum  = vec4(color * alpha * weight, alpha * weight);
    outReveal = alpha;
}
