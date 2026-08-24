#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_ARB_shading_language_include : enable

// ── Weather particle shading ──────────────────────────────────────────
// Fully procedural shapes (no textures): soft disc for snow, streak
// gradient for rain, low-alpha blob for dust, pointed spinning ellipse
// for leaves.  On top of the shape:
//   - cheap analytic light (billboard normal = view direction),
//   - the scene's fog block (same recipe as the composite pass) so distant
//     particles sit in the haze,
//   - soft-particle depth fade: the scene depth is sampled at the fragment
//     and alpha fades to 0 over the last 0.5 m before intersection — no
//     hard clipping where flakes meet the ground, and particles behind
//     opaque geometry disappear entirely.

layout(location = 0) in vec2 inQuad;
layout(location = 1) in flat uint inType;
layout(location = 2) in flat uint inSeed;
layout(location = 3) in float inViewZ;
layout(location = 4) in float inAlpha;
layout(location = 5) flat in vec3 inWorldPos;

layout(location = 0) out vec4 outColor;  // sceneColor (HDR, R16G16B16A16)

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

// Must match WeatherDrawPushConstants in VulkanAzgaarWeatherPass.c.
layout(push_constant) uniform Push {
    uint  depthIndex;
    uint  width;
    uint  height;
    float nearZ;
    float farZ;
    float projM00;
    float projM11;
} pc;

#define WEATHER_TYPE_SNOW    0u
#define WEATHER_TYPE_RAIN    1u
#define WEATHER_TYPE_DUST    2u
#define WEATHER_TYPE_LEAVES  3u

// Linearize the scene depth buffer (water-pass recipe).  Returns
// the positive view-space distance the depth value encodes.
float linearizeDepth(float d) {
    return (pc.nearZ * pc.farZ) / max(pc.nearZ + (pc.farZ - pc.nearZ) * d, 1e-7);
}

float seedRand(uint seed) {
    uint state = seed * 747796405u + 2891336453u;
    uint word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return float(((word >> 22u) ^ word) >> 8) * (1.0 / 16777216.0);
}

// Procedural shape coverage per type.  inQuad is the [-1,1]² corner.
float shapeAlpha(uint type, uint seed, float t) {
    if (type == WEATHER_TYPE_SNOW) {
        // Soft disc with a faint six-fold crystal hint (seed-rotated).
        float a = seedRand(seed) * 6.2831853;
        vec2 q = mat2(cos(a), -sin(a), sin(a), cos(a)) * inQuad;
        float d = length(q);
        float disc = smoothstep(1.0, 0.55, d);
        float star = 0.85 + 0.15 * cos(6.0 * atan(q.y, q.x));
        return disc * star;
    }
    if (type == WEATHER_TYPE_RAIN) {
        // Streak: soft along its length, tight across; brighter head.
        float len = pow(max(1.0 - abs(inQuad.y), 0.0), 1.3);
        float wid = smoothstep(1.0, 0.2, abs(inQuad.x));
        float head = mix(0.6, 1.25, smoothstep(-1.0, 0.0, inQuad.y));
        return len * wid * head;
    }
    if (type == WEATHER_TYPE_DUST) {
        // Low, wide soft blob.
        float d = length(inQuad * vec2(1.0, 1.3));
        return exp(-d * d * 2.2);
    }
    // WEATHER_TYPE_LEAVES: pointed ellipse, spinning about its centre
    // (seed for phase + speed variation).
    float speed = 1.2 + 2.2 * seedRand(seed ^ 0x99u);
    float ang = seedRand(seed) * 6.2831853 + t * speed;
    vec2 q = mat2(cos(ang), -sin(ang), sin(ang), cos(ang)) * inQuad;
    float d = length(q * vec2(1.0, 0.45));
    float body = smoothstep(1.0, 0.6, d);
    float tip = 0.55 + 0.45 * abs(q.y);  // pointed along its long axis
    return body * tip;
}

float typeBaseAlpha(uint type) {
    if (type == WEATHER_TYPE_SNOW)  return 0.85;
    if (type == WEATHER_TYPE_RAIN)  return 0.70;
    if (type == WEATHER_TYPE_DUST)  return 0.16;  // ground-hugging haze
    return 0.90;  // leaves
}

void main() {
    if (inAlpha <= 1e-3) discard;

    float t = float(sceneBuffer.time) * 1e-3;
    float alpha = inAlpha * shapeAlpha(inType, inSeed, t) * typeBaseAlpha(inType);
    if (alpha <= 1e-3) discard;

    // Soft-particle fade against the scene depth at this pixel: fully
    // transparent behind opaque geometry, fading over the last 0.5 m.
    float d = texelFetch(sampler2D(textures[nonuniformEXT(pc.depthIndex)],
                                   samplers[SAMPLER_NEAREST]),
                         ivec2(gl_FragCoord.xy), 0).r;
    float sceneZ = linearizeDepth(d);
    alpha *= clamp((sceneZ - inViewZ) / 0.5, 0.0, 1.0);
    if (alpha <= 1e-3) discard;

    // Cheap analytic light: billboards face the camera, so the "normal" is
    // the view direction; hemispheric ambient + sun dot on top of the tint.
    vec3 camPos = sceneBuffer.cameras[0].position.xyz;
    vec3 N = normalize(camPos - inWorldPos);
    vec3 sunDir = normalize(-sceneBuffer.directionalLight.direction.xyz);
    vec3 sunColor = sceneBuffer.directionalLight.color.rgb
                  * sceneBuffer.directionalLight.direction.w;
    float ndl = max(dot(N, sunDir), 0.0);
    float hemi = 0.5 + 0.5 * N.y;
    vec3 color = sceneBuffer.weather.tint.rgb
               * (vec3(0.35, 0.40, 0.50) * hemi + sunColor * (0.25 + 0.75 * ndl) * 0.6);

    // Scene fog (same block the composite pass applies) so distant flakes
    // sit in the haze instead of hovering crisp above it.
    if (sceneBuffer.fog.fogType != 0) {
        float dist = length(camPos - inWorldPos);
        float fogFactor = calculateFogFactor(sceneBuffer.fog.fogStartDistance,
                                             sceneBuffer.fog.fogType,
                                             sceneBuffer.fog.fogDensity,
                                             dist);
        if (sceneBuffer.fog.fogHeightEnabled != 0) {
            float h = inWorldPos.y - sceneBuffer.fog.fogHeightBase;
            float heightFactor = exp(-max(h, 0.0) * sceneBuffer.fog.fogHeightFalloff);
            fogFactor *= mix(1.0, heightFactor, sceneBuffer.fog.fogHeightDensity);
        }
        color = mix(color, sceneBuffer.fog.fogColor.rgb, clamp(fogFactor, 0.0, 1.0));
    }

    outColor = vec4(color, alpha);
}
