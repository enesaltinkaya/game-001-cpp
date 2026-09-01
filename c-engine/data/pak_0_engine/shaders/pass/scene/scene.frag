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

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outNormal;
layout(location = 2) out vec4 outMaterial;
layout(location = 3) out vec4 outAlbedo;  // base albedo (R16G16B16A16)

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

vec4 sampleMaterialTexture(uint texIndex, uint samplerIndex, vec2 uv) {
    return texture(
        sampler2D(textures[nonuniformEXT(texIndex)], samplers[nonuniformEXT(samplerIndex)]),
        uv);
}

// ---------------------------------------------------------------------------
// IBL sampling (envRotation rotates sample directions so the IBL stays in
// sync with the rotated extracted sun)
// ---------------------------------------------------------------------------
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
    Material material = materialBuffer.materials[inMaterialId];

    // Base color
    vec4 baseColor = material.baseColor;
    if ((material.featureMask & (1u << MAT_HAS_TEXTURE_COLOR)) != 0u) {
        vec2 uv = inUV * material.baseColorOffsetScale.zw + material.baseColorOffsetScale.xy;

        /* Jitter compensation for alpha-cutout: undo the UV shift caused
         * by the jittered projection so the texture sample position is
         * temporally stable.  Must match scene_depth.frag. */
        if ((material.featureMask & (1u << MAT_ALPHA_MASK)) != 0u) {
            vec2 jitterPx = -vec2(sceneBuffer.cameras[0].jitterX, sceneBuffer.cameras[0].jitterY) *
                            sceneBuffer.cameras[0].viewport;
            uv += dFdx(uv) * jitterPx.x + dFdy(uv) * jitterPx.y;
        }

        baseColor *= sampleMaterialTexture(material.colorTexture, material.colorTextureSampler, uv);
    }

    // Alpha test — plain binary test for temporally stable edges.
    //
    // With alpha-to-coverage disabled (when FSR is active), the alpha
    // test produces clean binary edges that move smoothly with camera
    // motion.  The reactive mask tells FSR to suppress temporal
    // accumulation at these edges, preventing shimmer.
    //
    // Must match scene_depth.frag for depth-equal to pass.
    if ((material.featureMask & (1u << MAT_ALPHA_MASK)) != 0u) {
        float cutoff = material.rmas.z;
        float a      = baseColor.a;
        if (stochasticAlphaDiscard(a, cutoff, gl_FragCoord.xy, 0u)) discard;
    }

    float occluderAlpha = cameraOccluderAlpha(inEntity);
    if (occluderAlpha < CAMERA_OCCLUDER_OPAQUE_UNDERLAY_ALPHA) {
        discard;
    }

    // Roughness & metallic
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

    // Normal
    vec3 N = normalize(inNormal);
    if ((material.featureMask & (1u << MAT_IS_DOUBLE_SIDED)) != 0u) {
        if (!gl_FrontFacing) N = -N;
    }
    if ((material.featureMask & (1u << MAT_HAS_TEXTURE_NORMAL)) != 0u) {
        vec3 T   = normalize(inTangent.xyz);
        vec3 B   = cross(N, T) * inTangent.w;
        mat3 TBN = mat3(T, B, N);
        vec2 uv  = inUV * material.normalOffsetScale.zw + material.normalOffsetScale.xy;
        vec3 nm =
            sampleMaterialTexture(material.normalTexture, material.normalTextureSampler, uv).rgb;
        nm           = nm * 2.0 - 1.0;
        vec3 mappedN = normalize(TBN * nm);
        N            = normalize(mix(N, mappedN, material.rmas.w));
    }

    // View direction
    vec3 V      = normalize(sceneBuffer.cameras[0].position.xyz - inWorldPos);
    float NdotV = max(dot(N, V), 0.001);

    // F0
    float ior = 1.5;
    if ((material.featureMask & (1u << MAT_HAS_IOR)) != 0u) ior = material.specularColorIor.w;
    float f0_dielectric = pow((ior - 1.0) / (ior + 1.0), 2.0);
    vec3 F0             = mix(vec3(f0_dielectric), baseColor.rgb, metallic);

    // Directional light
    DirectionalLight dirLight = sceneBuffer.directionalLight;
    vec3 lightDir             = normalize(dirLight.direction.xyz);
    vec3 lightColor           = dirLight.color.rgb * dirLight.direction.w;
    vec3 L                    = -lightDir;
    vec3 H                    = normalize(V + L);
    float NdotL               = max(dot(N, L), 0.0);
    float HdotV               = max(dot(H, V), 0.0);

    float D       = DistributionGGX(N, H, roughness);
    float G       = GeometrySmith(N, V, L, roughness);
    vec3 F        = fresnelSchlick(HdotV, F0);
    vec3 specular = (D * G * F) / (4.0 * NdotV * NdotL + 0.0001);

    // Shadows (cascaded + contact)
    vec4 shadowFull     = sampleShadowFull(inWorldPos, N);
    float contactShadow = sampleContactShadow();
    vec3 shadow         = shadowFull.rgb * contactShadow;

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

    vec3 kS = F;
    vec3 kD = (1.0 - kS) * (1.0 - metallic);
    vec3 Lo = (kD * baseColor.rgb / PI + specular) * lightColor * NdotL * shadow;

    // Ambient / IBL
    vec3 ambientDiffuse  = vec3(0.0);
    vec3 ambientSpecular = vec3(0.0);
    if (sceneBuffer.ibl.enabled != 0u) {
        vec3 R                  = reflect(-V, N);
        float iblIntensity      = sceneBuffer.ibl.intensity;
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

            /* Screen-space GI (plans/ssgi.md D5): mix the temporal GI
             * estimate over the IBL diffuse term.  gi.rgb is cosine-
             * weighted irradiance of the surrounding surfaces, so an
             * all-miss texel equals `irradiance` above by construction
             * (the estimate uses the identical IBL fallback chain) and
             * open-sky areas stay unchanged; gi.a is the estimate
             * confidence and gates the mix.  Units: same receiver-side
             * scaling as the IBL term (kD_ibl * baseColor / PI), so the
             * mix is energy-consistent in both branches.  NO
             * shadowDarkFactor here — the composite below applies it to
             * the whole ambient once.
             *
             * The index is the PREVIOUS frame's history (this pass runs
             * before the GI pass); 0xFFFFFFFFu = absent (disabled /
             * startup / resize) keeps the plain IBL ambient.  The history
             * is a temporally filtered field, sampled bilinearly at the
             * jittered G-buffer position (same UV as the contact-shadow
             * fetch). */
            if (sceneBuffer.gi.giImageIndex != 0xFFFFFFFFu) {
                vec2 giUv = gl_FragCoord.xy / sceneBuffer.cameras[0].viewport;
                vec4 gi   = texture(
                    sampler2D(textures[nonuniformEXT(sceneBuffer.gi.giImageIndex)],
                              samplers[SAMPLER_LINEAR]),
                    giUv);
                ambientDiffuse =
                    mix(ambientDiffuse,
                        kD_ibl * gi.rgb * baseColor.rgb / PI * iblIntensity,
                        clamp(gi.a * sceneBuffer.gi.giIntensity, 0.0, 1.0));
            }
        }
    }


    vec3 color = (ambientDiffuse + ambientSpecular) * shadowDarkFactor + Lo;

    // Forward+ point/spot lights
    vec3 T_aniso = vec3(0.0);
    vec3 B_aniso = vec3(0.0);
    color += evaluateForwardPlusLights(inWorldPos,
                                       N,
                                       V,
                                       NdotV,
                                       F0,
                                       roughness,
                                       metallic,
                                       baseColor.rgb,
                                       T_aniso,
                                       B_aniso,
                                       0.0);

    // Emissive
    vec3 emissive = vec3(0.0);
    if ((material.featureMask & (1u << MAT_HAS_EMISSIVE_FACTOR)) != 0u)
        emissive = material.emissive.rgb * material.emissive.w;
    if ((material.featureMask & (1u << MAT_HAS_TEXTURE_EMISSIVE)) != 0u) {
        vec2 uv = inUV * material.emissionOffsetScale.zw + material.emissionOffsetScale.xy;
        emissive *=
            sampleMaterialTexture(material.emissiveTexture, material.emissiveTextureSampler, uv)
                .rgb;
    }
    color += emissive;

    float alpha = baseColor.a;
    if ((material.featureMask & (1u << MAT_ALPHA_OPAQUE)) != 0u) alpha = 1.0;

    if (any(isnan(color)) || any(isinf(color))) color = vec3(0.0);
    color = clamp(color, vec3(0.0), vec3(65504.0));

    outColor        = vec4(color, alpha);
    outNormal       = OctEncode(normalize(N));
    float alphaMask = (material.featureMask & (1u << MAT_ALPHA_MASK)) != 0u ? 1.0 : 0.0;
    outMaterial     = vec4(roughness, metallic, alphaMask, 0.0);
    outAlbedo       = vec4(baseColor.rgb, 0.0);
}
