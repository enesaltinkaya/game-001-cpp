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
#include "../../includes/forwardplus.shader"

// ── Push constants (must match WaterPushConstants in VulkanAzgaarWaterPass.c) ─
layout(push_constant) uniform WaterPushConstants {
    uint  depthIndex;
    uint  width;
    uint  height;
    float nearZ;
    float farZ;
    float projM00;
    float projM11;
    float projM20;  // jittered-VP x translation term (TAA), 0 when jitter off
    float projM21;  // jittered-VP y translation term (TAA)
} pc;

// ── Scene depth sampling ───────────────────────────────────────────────
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
    // The scene depth was rasterized with the per-frame jittered projection
    // (TAA), whose translation terms (projM20/21) shift clip.xy by ±a
    // sub-pixel each frame.  The unjittered reconstruction
    // (viewPos.x = ndc.x * linZ / projM00) ignores them, so the recovered
    // terrain height would walk back and forth with the jitter — visible as
    // a shaky waterline/foam band at the beach.  Add the terms back.
    vec3 viewPos = vec3((ndc.x + pc.projM20) * linZ / pc.projM00,
                        (ndc.y + pc.projM21) * linZ / pc.projM11,
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
    // Same small, crisp disc as sky.shader (shared look).
    float sunDisc = smoothstep(0.99985, 0.99995, sunDot);
    float sunGlow = pow(sunDot, 256.0) * 0.2;
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
    // Key every waterline decision (discard / shore foam / absorption depth)
    // to the *undisturbed* water level, not the wave-displaced vertex height.
    // Animating the waterline with the Gerstner crests/troughs made the
    // shoreline flicker over the shallow beach: as a trough passed, the
    // surface dipped below the terrain and a dry patch popped into the water
    // (the "holes"); a crest sealed it again.  The waves still shape the
    // surface appearance (normals + ripples); they just no longer toggle
    // where the water is present.
    float waterY    = sceneBuffer.water.surfaceY.x;
    // Organic shoreline: displace the waterline with slow noise so it is not
    // a straight, hard edge, and fade the water out in a band *below* that
    // line.  The fade is asymmetric: the water is hard-culled above the
    // displaced line (no translucent film over dry land) and blends into it
    // from the water side over SHORE_FADE.
    const float SHORE_FADE = 0.25;  // metres of fade below the shoreline
    // The shoreline irregularity is STATIC (no time term): a drifting waterline
    // made the beach puddle's outline morph continuously (the noise offset
    // walked ~7 m/s through the 50 m wavelength field), so the water read as
    // leaking in and out of the shallows.  The foam/ripples still animate; the
    // waterline itself is stable.
    float shoreNoise = snoise(vec2(inWorldPos.x, inWorldPos.z) * 0.02);
    // The noise displacement is only effective within a couple of metres of
    // the waterline.  On a gently sloping beach the terrain sits inside the
    // ±0.2 m noise band over wide areas, so an unweighted line carves
    // animated dry "holes" and puddles into the shallow water.  Weighting by
    // proximity keeps water deeper than ~1 m pinned to a stable sea level.
    float lineProx = 1.0 - smoothstep(1.0, 3.0, abs(terrainPos.y - waterY));
    // The meander must not strand standing water on dry land.  Where the
    // terrain sits above sea level only the *downward* part of the
    // displacement applies: on a flat shelf a few centimetres above the
    // waterline the upward meander (up to +0.2 m) pushed the displaced
    // line above the terrain, so the early-out never fired and the
    // shallow-solid opacity + shore foam painted an animated water film
    // over the dry sand.  Submerged shoreline (terrainPos.y < waterY) is
    // unchanged — the full ±0.2 m meander still shapes the waterline.
    float displace = 0.2 * shoreNoise * lineProx;
    float dryT     = smoothstep(0.0, 0.05, terrainPos.y - waterY);
    float shoreY   = waterY + displace * (1.0 - dryT) + min(displace, 0.0) * dryT;
    // Hard cull over dry land, keyed to the *undisturbed* sea level: the
    // meander/foam/fade banding only ever shapes the water-side edge.  The old
    // test (terrain > displaced line + 0.05) let the animated film — ripples,
    // shore foam, shallow tint — bleed up to ~0.5 m *above* sea level onto
    // dry sand, ghosting a moving water sheet over the beach and over the
    // player character standing on it (their pixels reconstruct to
    // character height ≈ sea level, inside the old band).  Any pixel whose
    // surface (terrain, or the character in front of it) is at or above
    // sea level gets no water at all: the animation runs only where the
    // terrain is genuinely submerged.
    if (terrainPos.y >= waterY) discard;

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
    // Crest foam: only at genuine wave peaks.  inWaveHeightFactor is a
    // true [0,1] crest metric (vertex normalises by the actual peak
    // displacement), so a high smoothstep keeps it to the tips.  The
    // ripple detail *modulates* the crest foam (breaks it into organic
    // patches) instead of acting as an independent low-threshold foam
    // source — the old additive term lit broad swaths of open water.
    float crestFoam = smoothstep(0.75, 1.0, inWaveHeightFactor)
                    * (0.4 + 0.6 * smoothstep(0.3, 0.8, abs(ripple)));

    // Shore foam: a band around the *displaced waterline* (lineDepth > 0
    // under water, < 0 dry land).  Keying the foam to absolute depth from the
    // undisturbed sea level instead made the whole flat lagoon shelf (terrain
    // 0..-0.5 m, hundreds of metres wide) read as solid foam — a white wash
    // over the beach.  The band straddles the waterline so the white line
    // sits on the water side of the edge.
    // A thin, partially-intense line: on the gently sloping lagoon shelf the
    // ±0.4 m full-white band read as a foam wash over the whole beach.  Real
    // calm-water shorelines are a slim bright line; the wide shallows around
    // it should read as pale shallow water, not foam.
    float lineDepth = shoreY - terrainY;
    float shoreFoam = (1.0 - smoothstep(0.0, 0.2, abs(lineDepth))) * 0.7;
    // landT = 1 where the terrain is above the displaced shoreline (dry
    // land); the water fades out on the water side of the line and is hard-
    // culled on the land side, so no translucent film over dry ground.
    float landT         = smoothstep(shoreY - SHORE_FADE, shoreY, terrainY);
    float foamA = clamp(max(crestFoam, shoreFoam), 0.0, 1.0);
    // Note: foamColor.a is the shore-foam *threshold* (metres) used above;
    // the foam itself is a full-strength white line (the old foamA * foamColor.a
    // capped the blend at 30% and turned the waterline into a grey haze).

    // ── 7. Compose ──────────────────────────────────────────────────────
    vec3 color = mix(waterColor, skyRefl, fresnel) + sunSpecular;
    // Point/spot light streaks (e.g. a torch on a lake): the same Forward+
    // light grid as terrain/scene, evaluated as a tight streak on the
    // ripple-perturbed normal so the ripples glint.  Suppressed where foam
    // is present.
    color += evaluateForwardPlusLightsSpecular(
                 inWorldPos, N, V, sceneBuffer.water.sunSpecularPower)
           * (1.0 - foamA);
    vec3 foamColor = sceneBuffer.water.foamColor.rgb;
    color = mix(color, foamColor, foamA);

    // Alpha: more solid at grazing angles, less at normal incidence.
    // The nadir base is kept high enough that the green seabed of the
    // shallow lagoon basin does not read as a glass sheet through the water.
    float alpha = mix(0.55, 0.85, fresnel);
    // Shallow water: get more opaque as the terrain approaches the waterline
    // so the bright lagoon floor does not show through and make the near-shore
    // water read as dry "holes"; deep water keeps the translucent nadir so the
    // open basin stays glassy-clear.
    float shallowSolid = 1.0 - smoothstep(0.0, 2.5, waterDepth);
    alpha = max(alpha, mix(alpha, 0.9, shallowSolid));
    alpha = max(alpha, foamA);
    // Fully transparent over dry land (terrain above the water surface),
    // with a noise-widened soft band — no hard cutoff line.
    alpha = mix(alpha, 0.0, landT);

    if (any(isnan(color)) || any(isinf(color))) color = vec3(0.0);
    color = clamp(color, vec3(0.0), vec3(65504.0));

    outColor = vec4(color, alpha);
}
