/* forwardplus.shader — Forward+ point/spot light accumulation
 *
 * Include AFTER globalset.shader and utils.shader.
 * Requires: N, V, NdotV, F0, roughness, metallic, baseColor, inWorldPos,
 *           T_aniso, B_aniso, anisotropy (all defined in the calling shader).
 *
 * Exposes:
 *   vec3 evaluateForwardPlusLights(...)
 */

#ifndef FORWARD_PLUS_SHADER
#define FORWARD_PLUS_SHADER

#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT       1
#define LIGHT_SPOT        2

/* -----------------------------------------------------------------------
 * Distance/spot attenuation for a point or spot light.
 * Out-parameters L (unit vector toward the light). Returns 0 when the
 * light contributes nothing.  Shared by the PBR and diffuse variants so
 * the falloff behaviour stays identical.
 * ----------------------------------------------------------------------- */
float computeLightAttenuation(GpuLight light,
                              vec3 worldPos,
                              out vec3 L) {
    int   lightType = int(light.directionAndType.w);
    float intensity = light.colorAndIntensity.w;
    float attn;

    if (lightType == LIGHT_DIRECTIONAL) {
        L    = -normalize(light.directionAndType.xyz);
        attn = intensity;
    } else {
        vec3 lightPos = light.positionAndRange.xyz;
        float range   = light.positionAndRange.w;

        /* --- attenuation & direction to light --- */
        vec3  toLight   = lightPos - worldPos;
        float dist      = length(toLight);
        L               = toLight / max(dist, 0.0001);

        /* physically-based inverse-square falloff with smooth range cutoff */
        float dist2 = max(dist * dist, 1e-4);
        attn = 1.0;
        if (range > 0.0) {
            float distOverRange = dist / range;
            float d2            = distOverRange * distOverRange;
            float d4            = d2 * d2;
            attn = max(1.0 - d4, 0.0);
            attn = (attn * attn) / dist2;
        } else {
            attn = 1.0 / dist2;
        }
        attn *= intensity;

        /* --- spot cone attenuation --- */
        if (lightType == LIGHT_SPOT) {
            vec3  spotDir   = normalize(light.directionAndType.xyz);
            float cosAngle  = dot(-L, spotDir);  /* L points toward light */
            float cosInner  = light.spotAngles.x;
            float cosOuter  = light.spotAngles.y;
            float spotAtten = clamp(
                (cosAngle - cosOuter) / max(cosInner - cosOuter, 0.001), 0.0, 1.0);
            spotAtten *= spotAtten;  /* smoother falloff */
            attn *= spotAtten;
        }
    }

    return attn;
}

/* -----------------------------------------------------------------------
 * Single-light PBR evaluation (point or spot).
 * Returns Lo contribution in linear HDR.
 * ----------------------------------------------------------------------- */
vec3 evaluateOneLight(GpuLight light,
                      vec3 worldPos,
                      vec3 N,
                      vec3 V,
                      float NdotV,
                      vec3 F0,
                      float roughness,
                      float metallic,
                      vec3  baseColor,
                      vec3  T_aniso,
                      vec3  B_aniso,
                      float anisotropy) {
    vec3 L;
    float attn = computeLightAttenuation(light, worldPos, L);
    if (attn < 0.0001) return vec3(0.0);
    vec3  lightCol = light.colorAndIntensity.rgb;
    float NdotL    = max(dot(N, L), 0.0);
    vec3  H      = normalize(V + L);
    float HdotV  = max(dot(H, V), 0.0);

    if (NdotL <= 0.0) return vec3(0.0);

    float D;
    if (anisotropy != 0.0) {
        float at = max(roughness * (1.0 + anisotropy), 0.001);
        float ab = max(roughness * (1.0 - anisotropy), 0.001);
        float TdotH = dot(T_aniso, H);
        float BdotH = dot(B_aniso, H);
        float NdotH = dot(N, H);
        float a2 = at * ab;
        vec3 v_  = vec3(ab * TdotH, at * BdotH, a2 * NdotH);
        float v2 = dot(v_, v_);
        float w2 = a2 / v2;
        D = a2 * w2 * w2 * (1.0 / PI);
    } else {
        D = DistributionGGX(N, H, roughness);
    }

    float G   = GeometrySmith(N, V, L, roughness);
    vec3  F   = fresnelSchlick(HdotV, F0);

    vec3  num = D * G * F;
    float den = 4.0 * NdotV * NdotL + 0.0001;
    vec3  specular = num / den;

    vec3 kS = F;
    vec3 kD = (1.0 - kS) * (1.0 - metallic);

    vec3 Lo = (kD * baseColor / PI + specular) * lightCol * attn * NdotL;
    return Lo;
}

/* -----------------------------------------------------------------------
 * Accumulate all point and spot lights that affect this fragment's tile.
 * ----------------------------------------------------------------------- */
vec3 evaluateForwardPlusLights(vec3 worldPos,
                               vec3 N,
                               vec3 V,
                               float NdotV,
                               vec3  F0,
                               float roughness,
                               float metallic,
                               vec3  baseColor,
                               vec3  T_aniso,
                               vec3  B_aniso,
                               float anisotropy) {
    /* Tile index from fragment coordinate */
    ivec2 tileCoord = ivec2(gl_FragCoord.xy) / ivec2(16, 16);
    uint  tileCountX = uint((sceneBuffer.cameras[0].viewport.x + 15.0) / 16.0);
    uint  tileIndex  = uint(tileCoord.y) * tileCountX + uint(tileCoord.x);

    uvec2 tileData   = lightGridBuffer.tiles[tileIndex];
    uint  startIndex = tileData.x;
    uint  lightCount = tileData.y;

    vec3 Lo = vec3(0.0);

    for (uint i = 0; i < lightCount; i++) {
        uint lightIndex = lightIndexBuffer.indices[startIndex + i];
        GpuLight light  = sceneBuffer.lights[lightIndex];
        if (int(light.directionAndType.w) == LIGHT_DIRECTIONAL) continue;

        Lo += evaluateOneLight(light, worldPos, N, V, NdotV, F0,
                               roughness, metallic, baseColor,
                               T_aniso, B_aniso, anisotropy);
    }

    return Lo;
}

/* -----------------------------------------------------------------------
 * Lambert-only (matte) accumulation of all point/spot lights affecting
 * this fragment's tile.  For vegetation-scale surfaces (grass, tree
 * canopies) the specular GGX term is negligible at their high roughness,
 * so this cheaper variant carries only the energy-consistent /PI diffuse:
 *     Lo += albedo / PI * lightColor * attenuation * max(dot(N, L), 0)
 * Same light-grid traversal as evaluateForwardPlusLights().
 * ----------------------------------------------------------------------- */
vec3 evaluateForwardPlusLightsDiffuse(vec3 worldPos,
                                      vec3 N,
                                      vec3 baseColor) {
    ivec2 tileCoord = ivec2(gl_FragCoord.xy) / ivec2(16, 16);
    uint  tileCountX = uint((sceneBuffer.cameras[0].viewport.x + 15.0) / 16.0);
    uint  tileIndex  = uint(tileCoord.y) * tileCountX + uint(tileCoord.x);

    uvec2 tileData   = lightGridBuffer.tiles[tileIndex];
    uint  startIndex = tileData.x;
    uint  lightCount = tileData.y;

    vec3 Lo = vec3(0.0);

    for (uint i = 0; i < lightCount; i++) {
        uint lightIndex = lightIndexBuffer.indices[startIndex + i];
        GpuLight light  = sceneBuffer.lights[lightIndex];
        if (int(light.directionAndType.w) == LIGHT_DIRECTIONAL) continue;

        vec3  L;
        float attn = computeLightAttenuation(light, worldPos, L);
        if (attn < 0.0001) continue;

        float NdotL = max(dot(N, L), 0.0);
        Lo += (baseColor / PI) * light.colorAndIntensity.rgb * attn * NdotL;
    }

    return Lo;
}

/* -----------------------------------------------------------------------
 * Tight specular streak accumulation for reflective low-roughness
 * surfaces (water, river ribbons, wet surfaces).  Mirrors each light about
 * the (ripple-perturbed) normal and adds a Blinn-less highlight:
 *     Lo += lightColor * attenuation * pow(max(dot(reflect(-L, N), V), 0), shininess)
 * Ripples therefore sparkle under torches.  No fresnel here — the caller
 * scales the result (e.g. by 1 - foam amount).
 * ----------------------------------------------------------------------- */
vec3 evaluateForwardPlusLightsSpecular(vec3 worldPos,
                                       vec3 N,
                                       vec3 V,
                                       float shininess) {
    ivec2 tileCoord = ivec2(gl_FragCoord.xy) / ivec2(16, 16);
    uint  tileCountX = uint((sceneBuffer.cameras[0].viewport.x + 15.0) / 16.0);
    uint  tileIndex  = uint(tileCoord.y) * tileCountX + uint(tileCoord.x);

    uvec2 tileData   = lightGridBuffer.tiles[tileIndex];
    uint  startIndex = tileData.x;
    uint  lightCount = tileData.y;

    vec3 Lo = vec3(0.0);

    for (uint i = 0; i < lightCount; i++) {
        uint lightIndex = lightIndexBuffer.indices[startIndex + i];
        GpuLight light  = sceneBuffer.lights[lightIndex];
        if (int(light.directionAndType.w) == LIGHT_DIRECTIONAL) continue;

        vec3  L;
        float attn = computeLightAttenuation(light, worldPos, L);
        if (attn < 0.0001) continue;

        vec3  R    = reflect(-L, N);          /* reflected light ray */
        float spec = pow(max(dot(R, V), 0.0), shininess);
        Lo += light.colorAndIntensity.rgb * attn * spec;
    }

    return Lo;
}

#endif /* FORWARD_PLUS_SHADER */
