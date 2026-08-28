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

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outNormal;
layout(location = 2) out vec4 outMaterial;

// Push constants layout must match the C side, but we don't actually use them
// in the fragment shader. We include utils+globalset for material/texture
// access.
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
#include "../../includes/shadow.shader"
#include "../../includes/forwardplus.shader"

// ------------------------------------------------------------------
// Texture sampling helpers
// ------------------------------------------------------------------
vec4 sampleMaterialTexture(uint texIndex, uint samplerIndex, vec2 uv) {
    return texture(sampler2D(textures[nonuniformEXT(texIndex)],
                             samplers[nonuniformEXT(samplerIndex)]),
                   uv);
}

// ------------------------------------------------------------------
// Anisotropic GGX (Burley parameterization)
// ------------------------------------------------------------------
float DistributionGGXAnisotropic(vec3 N,
                                 vec3 H,
                                 vec3 T,
                                 vec3 B,
                                 float roughness,
                                 float anisotropy) {
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
// IBL sampling (envRotation rotates sample directions so the IBL stays in
// sync with the rotated extracted sun). Keep matched with scene.frag.
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
// Main
// ------------------------------------------------------------------
void main() {
    Material material = materialBuffer.materials[inMaterialId];

    // Base color
    vec4 baseColor = material.baseColor;

    if ((material.featureMask & (1u << MAT_HAS_TEXTURE_COLOR)) != 0u) {
        vec2 uv = inUV * material.baseColorOffsetScale.zw +
                  material.baseColorOffsetScale.xy;
        baseColor *= sampleMaterialTexture(material.colorTexture,
                                           material.colorTextureSampler,
                                           uv);
    }

    // Stochastic alpha test — dither with IGN so TAA/FSR resolves
    // sub-pixel alpha edges temporally instead of binary shimmer.
    if ((material.featureMask & (1u << MAT_ALPHA_MASK)) != 0u) {
        float cutoff = material.rmas.z;
        float a      = baseColor.a;
        // frameIndex=0: must match depth pass for depth-equal.
        // clip() enables alpha-to-coverage for smooth sub-pixel edges.
        if (stochasticAlphaDiscard(a, cutoff, gl_FragCoord.xy, 0u)) {
            discard;
        }
    }

    // Roughness & Metallic
    float roughness = material.rmas.x;
    float metallic  = material.rmas.y;

    if ((material.featureMask & (1u << MAT_HAS_TEXTURE_ROUGHNESS_METALLIC)) !=
        0u) {
        vec2 uv =
            inUV * material.rmaOffsetScale.zw + material.rmaOffsetScale.xy;
        vec4 rm = sampleMaterialTexture(material.rmTexture,
                                        material.rmTextureSampler,
                                        uv);
        roughness *= rm.g;
        metallic *= rm.b;
    }

    roughness = clamp(roughness, 0.04, 1.0);
    metallic  = clamp(metallic, 0.0, 1.0);

    // Normal mapping
    vec3 N = normalize(inNormal);

    // Flip normal for back faces on double-sided materials
    if ((material.featureMask & (1u << MAT_IS_DOUBLE_SIDED)) != 0u) {
        if (!gl_FrontFacing) {
            N = -N;
        }
    }

    if ((material.featureMask & (1u << MAT_HAS_TEXTURE_NORMAL)) != 0u) {
        vec3 T   = normalize(inTangent.xyz);
        vec3 B   = cross(N, T) * inTangent.w;
        mat3 TBN = mat3(T, B, N);

        vec2 uv = inUV * material.normalOffsetScale.zw +
                  material.normalOffsetScale.xy;
        vec3 normalMap = sampleMaterialTexture(material.normalTexture,
                                               material.normalTextureSampler,
                                               uv)
                             .rgb;
        normalMap = normalMap * 2.0 - 1.0;
        // Match Blender Normal Map node: first transform to world at full
        // strength, then mix with the geometric normal by the strength
        // factor.  This differs from scaling XY in tangent space and gives
        // identical results to Blender for any strength value.
        vec3 mappedN = normalize(TBN * normalMap);
        N            = normalize(mix(N, mappedN, material.rmas.w));
    }

    // View direction
    vec3 V      = normalize(sceneBuffer.cameras[0].position.xyz - inWorldPos);
    float NdotV = max(dot(N, V), 0.001);

    // IOR-based F0
    float ior = 1.5;
    if ((material.featureMask & (1u << MAT_HAS_IOR)) != 0u) {
        ior = material.specularColorIor.w;
    }
    float f0_dielectric = pow((ior - 1.0) / (ior + 1.0), 2.0);
    vec3 F0             = mix(vec3(f0_dielectric), baseColor.rgb, metallic);

    // Specular override (KHR_materials_specular)
    if ((material.featureMask & (1u << MAT_HAS_SPECULAR)) != 0u) {
        float specularFactor = material.specularAnisotropyRotation.x;
        vec3 specularColor   = material.specularColorIor.xyz;
        // Per KHR_materials_specular: specularFactor and specularColor
        // scale the dielectric F0.  Metallic F0 (= baseColor) is unchanged.
        vec3 dielectricF0 = vec3(f0_dielectric) * specularFactor * specularColor;
        F0 = mix(dielectricF0, baseColor.rgb, metallic);
    }

    // Tangent frame for anisotropy
    vec3 T_aniso = normalize(inTangent.xyz);
    vec3 B_aniso = normalize(cross(N, T_aniso) * inTangent.w);

    if ((material.featureMask & (1u << MAT_HAS_ANISOTROPY)) != 0u) {
        float rotation = material.specularAnisotropyRotation.y;
        float cosR     = cos(rotation);
        float sinR     = sin(rotation);
        vec3 T_rot     = cosR * T_aniso + sinR * B_aniso;
        vec3 B_rot     = -sinR * T_aniso + cosR * B_aniso;
        T_aniso        = T_rot;
        B_aniso        = B_rot;
    }

    // Directional light PBR evaluation
    DirectionalLight dirLight = sceneBuffer.directionalLight;
    vec3 lightDir             = normalize(dirLight.direction.xyz);
    vec3 lightColor           = dirLight.color.rgb * dirLight.direction.w;

    vec3 L      = -lightDir;  // direction TO the light (negate shine direction)
    vec3 H      = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    // Distribution
    float D;
    float anisotropy = 0.0;
    if ((material.featureMask & (1u << MAT_HAS_ANISOTROPY)) != 0u) {
        anisotropy = material.sheenColorAnisotropy.w;
        D          = DistributionGGXAnisotropic(N,
                                       H,
                                       T_aniso,
                                       B_aniso,
                                       roughness,
                                       anisotropy);
    } else {
        D = DistributionGGX(N, H, roughness);
    }

    float G = GeometrySmith(N, V, L, roughness);
    vec3 F  = fresnelSchlick(HdotV, F0);

    vec3 numerator    = D * G * F;
    float denominator = 4.0 * NdotV * NdotL + 0.0001;
    vec3 specular     = numerator / denominator;

    vec3 kS = F;
    vec3 kD = (1.0 - kS) * (1.0 - metallic);

    vec4 shadowFull = sampleShadowFull(inWorldPos, N);
    float contactShadow = sampleContactShadow();
    vec3 shadow = shadowFull.rgb * contactShadow;
    #if SHADOW_DEBUG > 0
        outColor    = debugShadow(inWorldPos, N);
        outNormal   = OctEncode(normalize(N));
        outMaterial = vec4(roughness, metallic, 0.0, 0.0);
        return;
    #endif
    vec3 Lo = (kD * baseColor.rgb / PI + specular) * lightColor * NdotL * shadow;

    // Forward+ point/spot lights
    Lo += evaluateForwardPlusLights(inWorldPos, N, V, NdotV, F0,
                                    roughness, metallic, baseColor.rgb,
                                    T_aniso, B_aniso, anisotropy);

    // Ambient occlusion
    float materialAo = 1.0;
    if ((material.featureMask & (1u << MAT_HAS_TEXTURE_OCCLUSION)) != 0u) {
        vec2 uv = inUV * material.occlusionOffsetScale.zw +
                  material.occlusionOffsetScale.xy;
        materialAo = sampleMaterialTexture(material.occlusionTexture,
                                           material.occlusionTextureSampler,
                                           uv)
                         .r;
    }

    // Ambient / IBL — keep this matched with scene.frag. Ambient comes
    // exclusively from the IBL resource; no default fill.
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

    // Shadow darkening: reduce ambient/IBL in shadowed areas.
    // 0.0 = no effect, 1.0 = pitch black in shadow.
    // Use only cascade shadows (shadowFull) here — contact shadows are
    // screen-space self-occlusion and should not suppress IBL.
    // At grazing angles (NdotL → 0), self-shadowing in the shadow map is
    // an artifact (orthogonal faces shadowing each other). Preserve ambient
    // fill for these faces so they don't go pitch black.
    vec3 shadowCascade    = shadowFull.rgb;
    vec3 shadowDarkFactor = mix(vec3(1.0), vec3(1.0 - SHADOW_DARKNESS), vec3(1.0 - shadowCascade));
    shadowDarkFactor      = mix(shadowDarkFactor, vec3(1.0), smoothstep(0.3, 0.0, NdotL));

    // Material AO weights the direct light slightly so contact darkening
    // stays visible in strongly sun-lit scenes.
    float directAo = mix(1.0, materialAo, 0.55);
    vec3 color     = (ambientDiffuse + ambientSpecular) * shadowDarkFactor + Lo * directAo;

    // Clearcoat
    if ((material.featureMask & (1u << MAT_HAS_CLEARCOAT)) != 0u) {
        float clearcoat          = material.clearcoatSheenTransmission.x;
        float clearcoatRoughness = material.clearcoatSheenTransmission.y;
        clearcoatRoughness       = clamp(clearcoatRoughness, 0.04, 1.0);

        float Dc = DistributionGGX(N, H, clearcoatRoughness);
        float Gc = GeometrySmith(N, V, L, clearcoatRoughness);
        vec3 Fc  = fresnelSchlick(HdotV, vec3(0.04));

        vec3 clearcoatSpecular =
            (Dc * Gc * Fc) / (4.0 * NdotV * NdotL + 0.0001);
        color = color * (1.0 - clearcoat * Fc) +
                clearcoatSpecular * clearcoat * NdotL;
    }

    // Sheen
    if ((material.featureMask & (1u << MAT_HAS_SHEEN)) != 0u) {
        vec3 sheenColor      = material.sheenColorAnisotropy.xyz;
        float sheenRoughness = max(material.clearcoatSheenTransmission.z, 0.04);

        float NdotH     = max(dot(N, H), 0.0);
        float sinNdotH2 = 1.0 - NdotH * NdotH;
        float sheenD    = (2.0 + 1.0 / sheenRoughness) *
                       pow(sinNdotH2, 0.5 / sheenRoughness) / (2.0 * PI);
        vec3 sheenContribution = sheenColor * sheenD * NdotL;

        color += sheenContribution;
    }

    // Transmission
    if ((material.featureMask & (1u << MAT_HAS_TRANSMISSION)) != 0u) {
        float transmission = material.clearcoatSheenTransmission.w;

        vec3 transmittedLight =
            baseColor.rgb * max(dot(N, -L), 0.0) * transmission;

        if ((material.featureMask & (1u << MAT_HAS_VOLUME)) != 0u) {
            float thickness       = material.volumeFactors.x;
            float attenuationDist = material.volumeFactors.y;
            vec3 attenuationColor = material.volumeColor.xyz;

            if (attenuationDist > 0.0) {
                vec3 absorption =
                    -log(max(attenuationColor, vec3(0.0001))) / attenuationDist;
                transmittedLight *= exp(-absorption * thickness);
            }
        }

        vec3 fresnelTransmission = fresnelSchlick(NdotV, F0);
        color                    = mix(color,
                    color + transmittedLight,
                    (1.0 - fresnelTransmission) * transmission);
    }

    // Emissive
    vec3 emissive = vec3(0.0);

    if ((material.featureMask & (1u << MAT_HAS_EMISSIVE_FACTOR)) != 0u) {
        emissive = material.emissive.rgb * material.emissive.w;
    }

    if ((material.featureMask & (1u << MAT_HAS_TEXTURE_EMISSIVE)) != 0u) {
        vec2 uv = inUV * material.emissionOffsetScale.zw +
                  material.emissionOffsetScale.xy;
        vec3 emissiveTex =
            sampleMaterialTexture(material.emissiveTexture,
                                  material.emissiveTextureSampler,
                                  uv)
                .rgb;
        emissive *= emissiveTex;
    }

    color += emissive;

    // Final output
    float alpha = baseColor.a;
    if ((material.featureMask & (1u << MAT_ALPHA_OPAQUE)) != 0u) {
        alpha = 1.0;
    }

    // Prevent NaN/Inf from reaching the HDR render target.
    if (any(isnan(color)) || any(isinf(color))) color = vec3(0.0);
    color = clamp(color, vec3(0.0), vec3(65504.0));

    outColor    = vec4(color, alpha);
    outNormal   = OctEncode(normalize(N));
    outMaterial = vec4(roughness, metallic, 0.0, 0.0);
}
