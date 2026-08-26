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

layout(push_constant) uniform PushConstants {
    uint64_t transformBufferAddress;
    uint64_t drawInstanceBufferAddress;
    uint64_t culledBufferAddress;
};

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"
#include "../../includes/shadow.shader"
#include "../../includes/forwardplus.shader"

vec4 sampleMaterialTexture(uint texIndex, uint samplerIndex, vec2 uv) {
    return texture(
        sampler2D(textures[nonuniformEXT(texIndex)], samplers[nonuniformEXT(samplerIndex)]),
        uv);
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

    vec3 kS = F;
    vec3 kD = (1.0 - kS) * (1.0 - metallic);
    vec3 Lo = (kD * baseColor.rgb / PI + specular) * lightColor * NdotL * shadow;

    // Ambient (from the sun UBO): zeroed, so shadowed areas are pure black.
    // Kept in the lighting equation for easy tuning.  Scaled by kD for
    // energy conservation with specular.
    vec3 ambient = dirLight.ambient.rgb;
    vec3 color   = Lo + ambient * kD * baseColor.rgb;

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
}
