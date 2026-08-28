// ============================================================================
// Shadow sampling with hardware bilinear PCF
//
// Features:
//   - 3×3 tent-filtered hardware bilinear PCF
//   - Texel-scaled normal offset for shadow acne / terminator prevention
//   - Small constant receiver depth bias
//   - Cascade blending to hide seams between cascade levels
//
// Set SHADOW_DEBUG to visualize different stages:
//   0 = normal shadow (production)
//   1 = cascade index as color (R=0, G=1, B=2, Y=3)
//   2 = shadow UV as RG color (verify UV mapping)
//   3 = currentDepth as grayscale (verify depth from matrix)
//   4 = sampled shadow map depth as grayscale (verify shadow map content)
//   5 = depth difference (currentDepth - sampledDepth) visualized
//   6 = raw shadow result (white = lit, black = shadow, no PCF)
// ============================================================================

#define SHADOW_DEBUG 0

/* How much ambient/IBL light is removed inside cascade shadows
 * (0 = shadowed areas keep full ambient, 1 = pitch black in shadow).
 * Used by the scene and heightmap_terrain fragment shaders. */
#define SHADOW_DARKNESS 0.6

/* Cascade blend region: fraction of each cascade's range used for blending.
 * Must match SHADOW_CASCADE_BLEND_FRACTION in VulkanShadowPass.c so the
 * cascade frustums overlap enough to supply valid shadow data. */
#define CASCADE_BLEND_FRACTION 0.3

/* -----------------------------------------------------------------------
 * Cascade selection / helpers
 * ----------------------------------------------------------------------- */

int selectCascade(vec3 worldPos) {
    ShadowData sd = sceneBuffer.shadow;
    Camera cam = sceneBuffer.cameras[0];

    /* Compute view-space depth of the fragment */
    vec4 viewPos = cam.view * vec4(worldPos, 1.0);
    float depth = -viewPos.z; /* view-space depth (positive forward) */

    for (int i = 0; i < int(sd.cascadeCount); i++) {
        if (depth < sd.cascadeSplits[i]) {
            return i;
        }
    }
    return int(sd.cascadeCount) - 1;
}

/* Returns the view-space depth for cascade blending */
float getViewDepth(vec3 worldPos) {
    Camera cam = sceneBuffer.cameras[0];
    vec4 viewPos = cam.view * vec4(worldPos, 1.0);
    return -viewPos.z;
}

vec4 cascadeDebugColor(int cascade) {
    if (cascade == 0) return vec4(1.0, 0.0, 0.0, 1.0); // red
    if (cascade == 1) return vec4(0.0, 1.0, 0.0, 1.0); // green
    if (cascade == 2) return vec4(0.0, 0.0, 1.0, 1.0); // blue
    return vec4(1.0, 1.0, 0.0, 1.0);                    // yellow
}

/* -----------------------------------------------------------------------
 * Debug shadow visualization
 * ----------------------------------------------------------------------- */

vec4 debugShadow(vec3 worldPos, vec3 normal) {
    ShadowData sd = sceneBuffer.shadow;

    int cascade = selectCascade(worldPos);
    uint mapIndex = sd.shadowMapIndex[cascade];

    if (mapIndex == 0u && sd.shadowParams.z == 0.0)
        return vec4(1.0, 0.0, 1.0, 1.0); // magenta = no shadow map

    vec3 biasedPos = worldPos + normal * sd.shadowParams.y;

    vec4 lightClip = sd.shadowViewProjection[cascade] * vec4(biasedPos, 1.0);
    vec3 projCoords = lightClip.xyz / lightClip.w;

    vec2 shadowUV = vec2(projCoords.x * 0.5 + 0.5, 0.5 - projCoords.y * 0.5);
    float currentDepth = projCoords.z;

    if (shadowUV.x < 0.0 || shadowUV.x > 1.0 || shadowUV.y < 0.0 || shadowUV.y > 1.0)
        return vec4(0.0, 0.0, 1.0, 1.0); // blue = out of bounds

    float sampledDepth = texture(
        sampler2D(textures[nonuniformEXT(mapIndex)],
                  samplers[SAMPLER_BORDER_NEAREST]),
        shadowUV
    ).r;

    float bias = sd.shadowParams.x;

    if (SHADOW_DEBUG == 1) {
        return cascadeDebugColor(cascade);
    } else if (SHADOW_DEBUG == 2) {
        return vec4(shadowUV, 0.0, 1.0);
    } else if (SHADOW_DEBUG == 3) {
        return vec4(vec3(currentDepth), 1.0);
    } else if (SHADOW_DEBUG == 4) {
        return vec4(vec3(sampledDepth), 1.0);
    } else if (SHADOW_DEBUG == 5) {
        float diff = currentDepth - sampledDepth;
        if (diff > 0.0)
            return vec4(0.0, min(diff * 50.0, 1.0), 0.0, 1.0);
        else
            return vec4(min(-diff * 50.0, 1.0), 0.0, 0.0, 1.0);
    } else if (SHADOW_DEBUG == 6) {
        float lit = (currentDepth - bias > sampledDepth) ? 0.0 : 1.0;
        return vec4(vec3(lit), 1.0);
    }

    return vec4(1.0);
}

/* Estimate the world-space size of one shadow texel for an orthographic
 * cascade directly from the light view-projection matrix.  The previous
 * fixed 0.001 world-unit normal offset was far below one texel for most
 * cascades, so smooth white objects (especially the test spheres) could
 * self-shadow into visible wavy acne/terminator bands. */
float cascadeWorldTexelSize(int cascade) {
    mat4 m = sceneBuffer.shadow.shadowViewProjection[cascade];

    /* Row vectors that map world position to clip X/Y.  GLSL matrices are
     * column-major, so row 0 is (m[0][0], m[1][0], m[2][0]). */
    vec3 clipRowX = vec3(m[0][0], m[1][0], m[2][0]);
    vec3 clipRowY = vec3(m[0][1], m[1][1], m[2][1]);

    float uvPerWorldUnit = 0.5 * max(length(clipRowX), length(clipRowY));
    if (uvPerWorldUnit <= 0.0)
        return sceneBuffer.shadow.shadowParams.y;

    return sceneBuffer.shadow.shadowParams.w / uvPerWorldUnit;
}

/* -----------------------------------------------------------------------
 * Sample shadow for a single cascade.
 *
 * Uses a fixed 3×3 tent-filtered PCF in shadow texel space. Each sampler2DShadow
 * tap with bilinear filtering performs comparison filtering in hardware.
 * ----------------------------------------------------------------------- */
float sampleShadowCascade(int cascade, vec3 worldPos, vec3 normal,
                           float bias, float NdotL) {
    ShadowData sd = sceneBuffer.shadow;
    uint mapIndex = sd.shadowMapIndex[cascade];

    /* Normal bias scaled by cascade texel size and by sin(angle-to-light).
     * Biasing by at least ~2 shadow texels is much more stable than a fixed
     * world-unit value: near cascades stay tight, far cascades get enough
     * offset to avoid self-shadowing. */
    float sinAngle = sqrt(max(1.0 - NdotL * NdotL, 0.0));
    float texelWorld = cascadeWorldTexelSize(cascade);
    float normalBias = max(sd.shadowParams.y, texelWorld * 2.0);
    normalBias = min(normalBias, 0.25); /* avoid obvious Peter Panning far away */
    vec3 biasedPos = worldPos + normal * normalBias * sinAngle;

    /* Project world position into light clip space */
    vec4 lightClip = sd.shadowViewProjection[cascade] * vec4(biasedPos, 1.0);
    vec3 projCoords = lightClip.xyz / lightClip.w;

    /* Remap XY from [-1,1] to [0,1]; Y flipped for Vulkan shadow pass */
    vec2 shadowUV = vec2(projCoords.x * 0.5 + 0.5, 0.5 - projCoords.y * 0.5);

    /* Keep receiver depth bias constant across cascades.  A derivative-based
     * receiver-plane bias looked good on close spheres, but in large outdoor
     * receiver shadows it changed abruptly between cascades and produced a
     * visible straight seam through the shadow.  The texel-scaled normal bias
     * above handles the acne case without that cascade-dependent depth term. */
    float receiverDepth = projCoords.z - sd.shadowParams.x;

    /* Out of shadow map bounds */
    if (shadowUV.x < 0.0 || shadowUV.x > 1.0 ||
        shadowUV.y < 0.0 || shadowUV.y > 1.0 ||
        receiverDepth < 0.0 || receiverDepth > 1.0)
        return 1.0;

    /* Hardware slope-scaled raster depth bias is still applied while writing
     * the shadow map; the receiver-side biases above cover the remaining
     * sampling/terminator cases without needing a large constant bias. */

    /* 3×3 tent-filtered hardware bilinear PCF.
     * This keeps the visible edge smoothing while avoiding the heavy 25-tap
     * kernel cost. */
    float texelSize = sd.shadowParams.w;
    float sum = 0.0;
    float weightSum = 0.0;
    for (int y = -1; y <= 1; y++) {
        float wy = (y == 0) ? 2.0 : 1.0;
        for (int x = -1; x <= 1; x++) {
            float wx = (x == 0) ? 2.0 : 1.0;
            float weight = wx * wy;
            vec2 uv = shadowUV + vec2(float(x), float(y)) * texelSize;
            sum += texture(sampler2DShadow(textures[nonuniformEXT(mapIndex)],
                                           samplers[SAMPLER_SHADOW_CMP]),
                           vec3(uv, receiverDepth)) * weight;
            weightSum += weight;
        }
    }
    return sum / weightSum;
}

/* -----------------------------------------------------------------------
 * Main shadow entry point — called from forward fragment shaders
 * ----------------------------------------------------------------------- */
vec4 sampleShadowFull(vec3 worldPos, vec3 normal) {
    if (SHADOW_DEBUG > 0)
        return debugShadow(worldPos, normal);

    ShadowData sd = sceneBuffer.shadow;

    /* No shadow map bound */
    if (sd.cascadeCount == 0u)
        return vec4(1.0);

    /* Select cascade based on view-space depth */
    int cascade = selectCascade(worldPos);
    uint mapIndex = sd.shadowMapIndex[cascade];

    if (mapIndex == 0u && sd.shadowParams.z == 0.0)
        return vec4(1.0);

    /* NdotL is still needed for the normal bias inside sampleShadowCascade. */
    vec3 lightDir = normalize(sceneBuffer.directionalLight.direction.xyz);
    float NdotL = clamp(dot(normal, -lightDir), 0.0, 1.0);

    float shadow = sampleShadowCascade(cascade, worldPos, normal, 0.0, NdotL);

    /* Cascade blending: smooth the transition between cascades. */
    if (cascade < int(sd.cascadeCount) - 1) {
        float viewDepth = getViewDepth(worldPos);
        float cascadeFar = sd.cascadeSplits[cascade];
        float cascadeNear = (cascade == 0) ? sceneBuffer.cameras[0].zNear : sd.cascadeSplits[cascade - 1];
        float cascadeRange = cascadeFar - cascadeNear;
        float blendRegion = cascadeRange * CASCADE_BLEND_FRACTION;
        float distToEdge = cascadeFar - viewDepth;

        if (distToEdge < blendRegion && blendRegion > 0.0) {
            float blendFactor = smoothstep(0.0, blendRegion, distToEdge);

            float nextShadow = sampleShadowCascade(cascade + 1, worldPos, normal, 0.0, NdotL);
            shadow = mix(nextShadow, shadow, blendFactor);
        }
    }

    /* .rgb and .a both contain cascade-only shadow */
    return vec4(vec3(shadow), shadow);
}

/* Sample screen-space contact shadow for the current fragment.
 * Returns 1.0 when no contact shadow texture is bound. */
float sampleContactShadow(void) {
    if (sceneBuffer.shadow.contactShadowImageIndex == 0u)
        return 1.0;
    vec2 screenUV = gl_FragCoord.xy / sceneBuffer.cameras[0].viewport;
    return texture(
        sampler2D(textures[nonuniformEXT(sceneBuffer.shadow.contactShadowImageIndex)],
                  samplers[SAMPLER_CLAMP_LINEAR]),
        screenUV).r;
}

/* Legacy wrapper — returns full shadow (cascade × contact) as vec3. */
vec3 sampleShadow(vec3 worldPos, vec3 normal) {
    return sampleShadowFull(worldPos, normal).rgb;
}
