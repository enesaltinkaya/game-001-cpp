#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_ARB_shading_language_include : enable

// ── Azgaar water fragment shader ───────────────────────────────────────
// Depth-tinted, animated, reflective ocean surface.
//
// Reads:  sceneBuffer.water (WaterData)
//         scene depth buffer (sampled as a texture)
// Writes: sceneColor (blend SRC_ALPHA)
//
// Pipeline: only color attachment 0 is bound (skybox-style).  The scene
// depth buffer is sampled (not bound as a render attachment) to recover the
// terrain height below the water, driving the real water depth and the
// shore foam band that replaces the old hard depth-test cutoff at the
// waterline.

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in float inWaveHeightFactor;

layout(location = 0) out vec4 outColor;  // sceneColor (HDR, R16G16B16A16)

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

// ── Push constants (must match WaterPushConstants in VulkanAzgaarWaterPass.c) ─
layout(push_constant) uniform WaterPushConstants {
    uint  depthIndex;
    uint  width;
    uint  height;
    float nearZ;
    float farZ;
    float projM00;
    float projM11;
} pc;

// ── Scene depth sampling (mirrors gtao.comp) ──────────────────────────
// The water pass no longer binds the depth buffer as a render attachment,
// so it is sampled here to recover the terrain height below the water.
float linearizeDepth(float d) {
    return (pc.nearZ * pc.farZ) / max(pc.nearZ + (pc.farZ - pc.nearZ) * d, 1e-7);
}

vec3 terrainPosAtPixel() {
    float d = texelFetch(sampler2D(textures[nonuniformEXT(pc.depthIndex)],
                                   samplers[SAMPLER_NEAREST]),
                  ivec2(gl_FragCoord.xy), 0).r;
    vec2 uv  = (vec2(gl_FragCoord.xy) + 0.5) / vec2(pc.width, pc.height);
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float linZ  = linearizeDepth(d);
    vec3 viewPos = vec3(ndc.x * linZ / pc.projM00,
                        ndc.y * linZ / pc.projM11,
                        -linZ);
    return (sceneBuffer.cameras[0].invView * vec4(viewPos, 1.0)).xyz;
}

// ── Procedural sky gradient + sun disc (mirrors skybox.frag) ────────────
// Extracted so water and skybox sample identical sky.
vec3 sampleAnalyticSky(vec3 worldDir) {
    vec3 zenithColor  = vec3(0.15, 0.35, 0.75);
    vec3 horizonColor = vec3(0.55, 0.75, 0.95);
    vec3 groundColor  = vec3(0.25, 0.28, 0.32);

    float t = clamp(worldDir.y, 0.0, 1.0);
    float skyMix = pow(t, 0.5);
    vec3 skyColor = mix(horizonColor, zenithColor, skyMix);
    float groundFade = smoothstep(0.0, -0.15, worldDir.y);
    skyColor = mix(skyColor, groundColor, groundFade);

    // Sun disc + glow
    vec3 sunDir = normalize(-sceneBuffer.directionalLight.direction.xyz);
    float sunDot = max(dot(worldDir, sunDir), 0.0);
    float sunDisc = smoothstep(0.9985, 0.9995, sunDot);
    float sunGlow = pow(sunDot, 64.0) * 0.4;
    vec3 sunColor = sceneBuffer.directionalLight.color.rgb
                  * sceneBuffer.directionalLight.direction.w;
    skyColor += sunDisc * sunColor * 8.0;
    skyColor += sunGlow * sunColor;
    return skyColor;
}

void main() {
    if (sceneBuffer.water.enabled < 0.5) discard;

    // ── 0. Early-out over dry land ─────────────────────────────────────
    // Reconstruct the terrain position from the scene depth buffer and bail
    // out before any wave / ripple / foam / sky shading when this pixel is
    // fully over dry land (terrain above the waterline + foam band): the
    // final alpha there is 0 anyway, so all of that shading is wasted.
    // Costs one depth fetch + one noise tap instead of the full shader.
    float time = float(sceneBuffer.time) / 1000.0;
    vec3 terrainPos = terrainPosAtPixel();
    float waterY    = inWorldPos.y;
    // Widen the land/water transition band with noise so the waterline is
    // not a straight, hard edge.  bandEdge = base * (1 + noise): base 0.4 m,
    // noise ±0.3 → bandEdge 0.28..0.52 m, total fade band 0.56..1.04 m
    // (was 0.8..3.2 m, too long).
    float shoreNoise = snoise(vec2(inWorldPos.x, inWorldPos.z) * 0.02
                          + vec2(time * 0.15, time * 0.1));
    float bandEdge   = 0.4 * (1.0 + 0.3 * shoreNoise);
    if (terrainPos.y > waterY + bandEdge) discard;

    vec3 N = normalize(inWorldNormal);
    vec3 V = normalize(sceneBuffer.cameras[0].position.xyz - inWorldPos);

    // ── 1. Procedural fragment ripples (multi-octave) ───────────────────
    float ripple = 0.0;
    vec2 windDir = vec2(cos(sceneBuffer.water.windAngle), sin(sceneBuffer.water.windAngle));
    vec2 uv1 = inWorldPos.xz * sceneBuffer.water.rippleScale * 0.5;
    // Ripple UVs scroll slowly so the surface detail doesn't shimmer/fast-
    // move when viewed up close at the shoreline.
    vec2 uv2 = inWorldPos.xz * sceneBuffer.water.rippleScale * 1.7 + time * 0.15;
    vec2 uv3 = inWorldPos.xz * sceneBuffer.water.rippleScale * 3.1 + time * 0.35;

    ripple += snoise(uv1) * 0.5;
    ripple += snoise(uv2) * 0.3;
    ripple += snoise(uv3) * 0.2;

    // Perturb normal with ripples
    float dudv = 0.05;
    vec2 grad = vec2(
        snoise(uv2 + vec2(dudv, 0.0)) - snoise(uv2 - vec2(dudv, 0.0)),
        snoise(uv3 + vec2(0.0, dudv)) - snoise(uv3 - vec2(0.0, dudv))
    ) * dudv * sceneBuffer.water.normalStrength;
    N = normalize(N + vec3(grad.x, 0.0, grad.y));

    // ── 2. Fresnel (Schlick) ────────────────────────────────────────────
    float NdotV = max(dot(N, V), 0.0);
    float fresnel = pow(1.0 - NdotV, sceneBuffer.water.fresnelPower) * sceneBuffer.water.fresnelScale;
    fresnel = clamp(fresnel, 0.0, 1.0);

    // ── 3. Sky reflection ───────────────────────────────────────────────
    vec3 reflectDir = reflect(-V, N);
    vec3 skyRefl = sampleAnalyticSky(reflectDir);

    // ── 4. Sun specular (Blinn-Phong) ───────────────────────────────────
    vec3 sunDir = normalize(-sceneBuffer.directionalLight.direction.xyz);
    vec3 sunColor = sceneBuffer.directionalLight.color.rgb
                  * sceneBuffer.directionalLight.direction.w;
    vec3 H = normalize(V + sunDir);
    float NdotH = max(dot(N, H), 0.0);
    float sunSpec = pow(NdotH, sceneBuffer.water.sunSpecularPower)
                  * sceneBuffer.water.sunSpecularIntensity;
    vec3 sunSpecular = sunSpec * sunColor;

    // ── 5. Depth-driven absorption (Beer-Lambert) ───────────────────────
    // Uses the terrain position reconstructed in the early-out block above
    // so the water colour transitions smoothly from shallow to deep instead
    // of a constant shallow depth.
    float terrainY   = terrainPos.y;
    float waterDepth = max(waterY - terrainY, 0.0);
    float absorptionT = clamp(waterDepth / sceneBuffer.water.deepColor.a, 0.0, 1.0);
    vec3 shallowTint = sceneBuffer.water.shallowColor.rgb;
    vec3 deepTint   = sceneBuffer.water.deepColor.rgb;
    vec3 waterColor = mix(shallowTint, deepTint, absorptionT);

    // ── 6. Foam (crest + shore) ─────────────────────────────────────────
    // Crest foam from vertex wave height factor
    float crestFoam = smoothstep(0.6, 1.0, inWaveHeightFactor);
    // Add ripple-driven foam at wave peaks
    crestFoam = max(crestFoam, smoothstep(0.7, 0.95, abs(ripple) * 0.5 + inWaveHeightFactor));

    // Shore foam: a band around the waterline, modulated by animated noise
    // so the edge is irregular — this replaces the old hard depth-test
    // cutoff that made the waterline look like a sharp line.
    float foamThreshold = sceneBuffer.water.foamColor.a;  // metres
    float shoreFoam     = smoothstep(foamThreshold, 0.0, waterDepth);
    // landT = 1 where the terrain is above the water surface (dry land);
    // the water fades out there instead of painting a translucent film
    // over the ground.  Fully-dry pixels already discarded above, so
    // landT < 1 here and the fade band is applied to survivors only.
    float landT         = smoothstep(waterY - bandEdge, waterY + bandEdge, terrainY);
    shoreFoam          *= (1.0 - landT);
    float foamA = clamp(max(crestFoam, shoreFoam), 0.0, 1.0);

    // ── 7. Compose ──────────────────────────────────────────────────────
    vec3 color = mix(waterColor, skyRefl, fresnel) + sunSpecular;
    vec3 foamColor = sceneBuffer.water.foamColor.rgb;
    color = mix(color, foamColor, foamA * sceneBuffer.water.foamColor.a);

    // Alpha: more solid at grazing angles, less at normal incidence
    float alpha = mix(0.35, 0.85, fresnel);
    alpha = max(alpha, foamA * 0.8);
    // Fully transparent over dry land (terrain above the water surface),
    // with a noise-widened soft band — no hard cutoff line.
    alpha = mix(alpha, 0.0, landT);

    if (any(isnan(color)) || any(isinf(color))) color = vec3(0.0);
    color = clamp(color, vec3(0.0), vec3(65504.0));

    outColor = vec4(color, alpha);
}
