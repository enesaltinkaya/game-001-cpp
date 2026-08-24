#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_ARB_shading_language_include : enable

// ── Weather particle coverage mask ──────────────────────────────────
// Drawn with the same instanced vertex expansion as the main weather
// draw (same particle buffer, same push constants) but outputs a
// per-pixel coverage value (0..1) into an R8 target instead of shading.
//
// Why: the TAA pass reprojects the previous frame's scene colour, which
// still contains each particle at its OLD position.  With a static
// camera the TAA anti-ghost rejection is gated off (it keys on camera
// motion), so a moving particle leaves a ~20-frame chain of ghosts
// behind it — leaves read as meteor streaks.  The weather pass therefore
// writes this frame's coverage each frame (ping-pong pair), and the TAA
// pass drops the temporal weight where a particle covers the pixel NOW
// (maskCur) or covered the reprojected history position LAST frame
// (maskPrev).  The particle head stays crisp (it is the current frame's
// coverage); the fading tail is removed entirely.
//
// shapeAlpha() must match azgaar_weather.frag — keep in sync.

layout(location = 0) in vec2 inQuad;
layout(location = 1) in flat uint inType;
layout(location = 2) in flat uint inSeed;
layout(location = 3) in float inViewZ;
layout(location = 4) in float inAlpha;
layout(location = 5) flat in vec3 inWorldPos;

layout(location = 0) out vec4 outColor;  // x = coverage [0, 1]

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

// Must match WeatherDrawPushConstants in VulkanAzgaarWeatherPass.c (the mask
// draw reuses the main draw's push constants).
layout(push_constant) uniform Push {
    uint  depthIndex;
    uint  width;
    uint  height;
    float nearZ;
    float farZ;
    float projM00;
    float projM11;
} pc;

// Same linearization as azgaar_weather.frag.
float linearizeDepth(float d) {
    return (pc.nearZ * pc.farZ) / max(pc.nearZ + (pc.farZ - pc.nearZ) * d, 1e-7);
}

#define WEATHER_TYPE_SNOW    0u
#define WEATHER_TYPE_RAIN    1u
#define WEATHER_TYPE_DUST    2u
#define WEATHER_TYPE_LEAVES  3u

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

void main() {
    if (inAlpha <= 1e-3) discard;
    float t = float(sceneBuffer.time) * 1e-3;
    // Coverage = pure SHAPE, deliberately WITHOUT inAlpha (opacity / far
    // fade) and typeBaseAlpha: the mask marks "a particle occupies this
    // pixel", not how dark it is.  Coupling coverage to opacity would let
    // distant / cross-fading leaves (inAlpha < ~0.8) drop below the TAA
    // rejection threshold and blend ~70% stale history into their head —
    // a one-frame-offset ghost behind every far leaf.  Fully faded or
    // disabled particles never get here (the vertex shader collapses them
    // to zero-area quads).
    float cov = shapeAlpha(inType, inSeed, t);
    // Same soft-particle depth fade as azgaar_weather.frag: only mark pixels
    // the particle is actually VISIBLE in.  An occluded particle must not
    // mark coverage — TAA would drop history on the occluder, and the
    // composite pass suppresses its screen-space fog on masked pixels,
    // which would leave an un-fogged hole in the geometry behind the
    // hidden particle.
    float d = texelFetch(sampler2D(textures[nonuniformEXT(pc.depthIndex)],
                                   samplers[SAMPLER_NEAREST]),
                         ivec2(gl_FragCoord.xy), 0).r;
    float sceneZ = linearizeDepth(d);
    cov *= clamp((sceneZ - inViewZ) / 0.5, 0.0, 1.0);
    if (cov <= 1e-3) discard;
    outColor = vec4(cov, 0.0, 0.0, 1.0);
}
