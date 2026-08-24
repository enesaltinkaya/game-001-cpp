#ifndef POM_SHADER
#define POM_SHADER

// ============================================================================
// Parallax Occlusion Mapping (POM)
//
// Ray-marches through a heightfield stored in a texture's alpha channel
// to produce apparent depth/relief on flat surfaces.
// ============================================================================

// Tuning constants
#define POM_MIN_STEPS          6     // minimum ray-march steps (distant / head-on)
#define POM_MAX_STEPS          8     // maximum ray-march steps (close / grazing)
#define POM_FADE_START         20.0  // distance (m) where POM begins fading
#define POM_FADE_END           30.0  // distance (m) where POM is fully off
#define POM_BINARY_STEPS       4     // binary refinement iterations

// -----------------------------------------------------------------------
// sampleMaterialTextureGrad — textureGrad() with bindless handles
// -----------------------------------------------------------------------
vec4 sampleMaterialTextureGrad(uint texIndex, uint samplerIndex,
                                vec2 uv, vec2 ddx, vec2 ddy) {
    return textureGrad(
        sampler2D(textures[nonuniformEXT(texIndex)],
                  samplers[nonuniformEXT(samplerIndex)]),
        uv, ddx, ddy);
}

// -----------------------------------------------------------------------
// samplePOMHeightBlurred — 5-tap cross-blurred height sample for ray-march
//
// The heightmap lives in the alpha channel of an 8-bit normal map.
// With only 256 discrete levels, a sub-pixel UV shift (e.g. FSR jitter)
// can make the ray-march intersection jump between frames, causing
// visible shimmering.  A 5-tap cross blur (center + 4 neighbors at
// ±1 texel) smooths the heightfield so the intersection point changes
// continuously even when the UV shifts by up to half a pixel.
// -----------------------------------------------------------------------
float samplePOMHeightBlurred(uint texIndex, uint samplerIndex,
                              vec2 uv, vec2 ddx, vec2 ddy) {
    vec2 sx = vec2(ddx.x, 0.0);
    vec2 sy = vec2(0.0, ddy.y);
    float h = 0.0;
    // Center tap (weighted 2× for smoother Gaussian-like kernel)
    h += textureGrad(sampler2D(textures[nonuniformEXT(texIndex)],
                                samplers[nonuniformEXT(samplerIndex)]),
                     uv, ddx, ddy).a * 2.0;
    h += textureGrad(sampler2D(textures[nonuniformEXT(texIndex)],
                                samplers[nonuniformEXT(samplerIndex)]),
                     uv + sx, ddx, ddy).a;
    h += textureGrad(sampler2D(textures[nonuniformEXT(texIndex)],
                                samplers[nonuniformEXT(samplerIndex)]),
                     uv - sx, ddx, ddy).a;
    h += textureGrad(sampler2D(textures[nonuniformEXT(texIndex)],
                                samplers[nonuniformEXT(samplerIndex)]),
                     uv + sy, ddx, ddy).a;
    h += textureGrad(sampler2D(textures[nonuniformEXT(texIndex)],
                                samplers[nonuniformEXT(samplerIndex)]),
                     uv - sy, ddx, ddy).a;
    return h / 6.0;
}

// -----------------------------------------------------------------------
// parallaxOcclusionMap
//
// Performs a linear ray-march + binary refinement through a heightfield.
//
// texIndex/samplerIndex : bindless height texture (height in .a channel)
// uv                    : initial tiled UV
// viewDirTS             : view direction in tangent space (points toward camera)
// heightScale           : POM depth in UV space (see caller for computation)
// fadeFactor            : 0 = POM off, 1 = full POM (distance-based)
// ddx, ddy              : pre-computed UV gradients for textureGrad
//
// Returns: vec3(offsetU, offsetV, finalHeight)
// -----------------------------------------------------------------------
vec3 parallaxOcclusionMap(uint texIndex, uint samplerIndex,
                          vec2 uv, vec3 viewDirTS, float heightScale,
                          float fadeFactor, vec2 ddx, vec2 ddy) {
    // Early out if POM is disabled at this distance
    if (fadeFactor < 0.001) {
        float h = sampleMaterialTextureGrad(texIndex, samplerIndex, uv, ddx, ddy).a;
        return vec3(uv, h);
    }

    // Normalize view direction in tangent space
    vec3 V = normalize(viewDirTS);

    // Adaptive step count: more steps at grazing angles, fewer when head-on
    float angleFactor = 1.0 - abs(V.z);
    int numSteps = int(mix(float(POM_MIN_STEPS), float(POM_MAX_STEPS), angleFactor));

    // Step size along the height axis
    float stepHeight = 1.0 / float(numSteps);

    // UV offset per step: project view direction onto the UV plane
    vec2 uvStep = (V.xy / max(abs(V.z), 0.001)) * heightScale * stepHeight;

    // Center-bias: shift the ray start so that height=0.5 in the
    // heightmap corresponds to the geometric surface.
    vec2 halfDepthOffset = (V.xy / max(abs(V.z), 0.001)) * heightScale * 0.5;

    // Start above the surface but pre-offset UVs so the midpoint
    // of the march aligns with the original surface.
    float currentHeight = 1.0;
    vec2 currentUV = uv + halfDepthOffset;
    float prevSampledHeight = 1.0;
    // Use blurred height samples during ray-march to reduce quantization
    // jumps from 8-bit heightmaps when UV shifts (FSR jitter, etc.).
    float sampledHeight = samplePOMHeightBlurred(texIndex, samplerIndex,
                                                  currentUV, ddx, ddy);

    // Linear search: step along until the ray goes below the heightfield
    for (int i = 0; i < numSteps && currentHeight > sampledHeight; i++) {
        currentUV -= uvStep;
        currentHeight -= stepHeight;
        prevSampledHeight = sampledHeight;
        sampledHeight = samplePOMHeightBlurred(texIndex, samplerIndex,
                                                currentUV, ddx, ddy);
    }

    // Binary refinement: narrow down the intersection point
    vec2 prevUV = currentUV + uvStep;
    float prevHeight = currentHeight + stepHeight;

    for (int i = 0; i < POM_BINARY_STEPS; i++) {
        vec2 midUV = (prevUV + currentUV) * 0.5;
        float midHeight = (prevHeight + currentHeight) * 0.5;
        float midSample = samplePOMHeightBlurred(texIndex, samplerIndex,
                                                  midUV, ddx, ddy);
        if (midHeight > midSample) {
            prevUV = midUV;
            prevHeight = midHeight;
            prevSampledHeight = midSample;
        } else {
            currentUV = midUV;
            currentHeight = midHeight;
            sampledHeight = midSample;
        }
    }

    // Blend between original UV and POM-offset UV using fade factor
    vec2 finalUV = mix(uv, currentUV, fadeFactor);
    float finalHeight = sampleMaterialTextureGrad(texIndex, samplerIndex,
                                                   finalUV, ddx, ddy).a;

    return vec3(finalUV, finalHeight);
}

// -----------------------------------------------------------------------
// pomOffsetUV — convenience wrapper that returns only the offset UV
// -----------------------------------------------------------------------
vec2 pomOffsetUV(uint texIndex, uint samplerIndex,
                 vec2 uv, vec3 viewDirTS, float heightScale,
                 float fadeFactor, vec2 ddx, vec2 ddy) {
    return parallaxOcclusionMap(texIndex, samplerIndex,
                                uv, viewDirTS, heightScale,
                                fadeFactor, ddx, ddy).xy;
}

#endif // POM_SHADER
