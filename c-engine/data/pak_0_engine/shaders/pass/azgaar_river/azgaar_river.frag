#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_ARB_shading_language_include : enable

// ── Azgaar river ribbon fragment shader ─────────────────────────────────
// Derived from azgaar_water.frag: same ripple noise + fresnel + sun
// specular, but the ripple UVs are advected along the per-vertex flow
// direction (loc2) at a speed proportional to the river's discharge (loc4,
// flowFactor 0..1).  A subtle dark bed tint comes from the depth-buffer
// height difference (terrain below the ribbon), replacing a constant tint.

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inFlowDir;
layout(location = 3) in float inArcLen;
layout(location = 4) in float inFlowFactor;

layout(location = 0) out vec4 outColor;  // sceneColor (HDR, R16G16B16A16)

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"
#include "../../includes/forwardplus.shader"

// ── Push constants (must match RiverPushConstants in VulkanAzgaarRiverPass.c) ─
layout(push_constant) uniform RiverPushConstants {
    uint  depthIndex;
    uint  width;
    uint  height;
    float nearZ;
    float farZ;
    float projM00;
    float projM11;
} pc;

// Procedural sky (identical to skybox.frag / water.frag)
vec3 sampleAnalyticSky(vec3 worldDir) {
    vec3 zenithColor  = vec3(0.15, 0.35, 0.75);
    vec3 horizonColor = vec3(0.55, 0.75, 0.95);
    vec3 groundColor  = vec3(0.25, 0.28, 0.32);

    float t = clamp(worldDir.y, 0.0, 1.0);
    float skyMix = pow(t, 0.5);
    vec3 skyColor = mix(horizonColor, zenithColor, skyMix);
    float groundFade = smoothstep(0.0, -0.15, worldDir.y);
    skyColor = mix(skyColor, groundColor, groundFade);

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
    float time = float(sceneBuffer.time) / 1000.0;

    // ── Flow-advected ripples ────────────────────────────────────────────
    // Stream coordinate along the flow direction; advected at a speed driven
    // by the river's discharge (inFlowFactor 0..1).
    vec2 flowXZ = inFlowDir.xz;
    float flowCoord = dot(inWorldPos.xz, flowXZ) * 0.5;
    float flowSpeed = 0.2 + 0.8 * inFlowFactor;
    float ripplePhase = time * flowSpeed;

    float s1 = inArcLen * 0.05 - ripplePhase;
    float s2 = inArcLen * 0.11 - ripplePhase * 1.7;
    float ripple = snoise(vec2(flowCoord, s1));
    ripple += 0.5 * snoise(vec2(flowCoord * 2.3 + 7.0, s2));

    // Perturb the up-normal with the ripple gradient
    float dudv = 0.05;
    vec2 grad = vec2(
        snoise(vec2(flowCoord, s1 + dudv)) - snoise(vec2(flowCoord, s1 - dudv)),
        snoise(vec2(s1, flowCoord + dudv)) - snoise(vec2(s1, flowCoord - dudv))
    ) * dudv * 0.6;
    vec3 N = normalize(inNormal + vec3(grad.x, 0.0, grad.y));

    vec3 V = normalize(sceneBuffer.cameras[0].position.xyz - inWorldPos);

    // ── Fresnel (Schlick) ────────────────────────────────────────────────
    float NdotV = max(dot(N, V), 0.0);
    float fresnel = clamp(pow(1.0 - NdotV, 3.0) * 1.2, 0.0, 1.0);

    // ── Sky reflection + sun specular ─────────────────────────────────────
    vec3 reflectDir = reflect(-V, N);
    vec3 skyRefl = sampleAnalyticSky(reflectDir);
    vec3 sunDir = normalize(-sceneBuffer.directionalLight.direction.xyz);
    vec3 sunColor = sceneBuffer.directionalLight.color.rgb
                  * sceneBuffer.directionalLight.direction.w;
    vec3 H = normalize(V + sunDir);
    vec3 sunSpecular = pow(max(dot(N, H), 0.0), 32.0) * sunColor;

    // ── Bed tint ─────────────────────────────────────────────────────────
    // The ribbon tracks the terrain within ~3 cm, so the water is essentially
    // shallow everywhere along the ribbon; use the shallow tint directly (the
    // deep tint would be unreachable here).  Occlusion of the ribbon by the
    // opaque scene is handled by the depth-test-only render attachment in the
    // pass (see VulkanAzgaarRiverPass.c), which fixes the "always on top" bug.
    vec3 waterColor = vec3(0.16, 0.24, 0.22);

    // Compose
    vec3 color = mix(waterColor, skyRefl, fresnel) + sunSpecular;
    // Point/spot light streaks: tight glint on the ripple-perturbed normal,
    // using the same Forward+ light grid as terrain/scene.  Exponent 32
    // matches this pass's sun streak pow so both read consistently.
    color += evaluateForwardPlusLightsSpecular(inWorldPos, N, V, 32.0);
    // Subtle animated bed darkening from the ripple field
    color *= (1.0 - 0.15 * clamp(ripple * 0.5 + 0.5, 0.0, 1.0));

    float alpha = mix(0.70, 0.95, fresnel);

    if (any(isnan(color)) || any(isinf(color))) color = vec3(0.0);
    color = clamp(color, vec3(0.0), vec3(65504.0));

    outColor = vec4(color, alpha);
}
