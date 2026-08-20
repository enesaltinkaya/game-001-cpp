#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_ARB_shading_language_include : enable

// ── Weather particle vertex expansion ─────────────────────────────────
// No vertex buffer: vkCmdDraw(6, maxParticles) with gl_VertexIndex making
// the quad corners.  The particle's posSeed arrives through a single
// instance-rate binding (stride 16, props-pass pattern); the simulation
// compute wrote it earlier this frame.
//
//   snow / dust / leaves → camera-facing quad (invView right/up),
//   rain                 → velocity-stretched streak: long axis along the
//                          analytic velocity (fall + wind shear), short
//                          axis perpendicular in screen space.
//
// Size = per-type base × seed jitter × WeatherData.params.w, clamped to
// ≥ 2 px so TAA/FSR see stable thin geometry.  Density-roulette-disabled
// and fully-faded particles collapse to a zero-area quad (discarded before
// rasterization).

layout(location = 0) in vec4 inPosSeed;  // instance rate: xyz world pos, w packed meta

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

// Shared with azgaar_weather.frag / weather_update.comp.
#define WEATHER_TYPE_SNOW    0u
#define WEATHER_TYPE_RAIN    1u
#define WEATHER_TYPE_DUST    2u
#define WEATHER_TYPE_LEAVES  3u

#define META_TYPE_MASK     0xFFu
#define META_SEED_SHIFT    8u
#define META_SEED_MASK     0x7FFFFFu
#define META_DISABLED_BIT  0x80000000u

// Must match WeatherDrawPushConstants in VulkanAzgaarWeatherPass.c.
layout(push_constant) uniform Push {
    uint  depthIndex;  // scene depth in the bindless sampled pool (frag only)
    uint  width;
    uint  height;
    float nearZ;
    float farZ;
    float projM00;
    float projM11;
} pc;

layout(location = 0) out vec2 outQuad;         // corner in [-1, 1] (shape coords)
layout(location = 1) flat out uint outType;
layout(location = 2) flat out uint outSeed;
layout(location = 3) out float outViewZ;       // positive view distance (soft fade)
layout(location = 4) out float outAlpha;       // density + far fade + global opacity
layout(location = 5) flat out vec3 outWorldPos;

// Two-triangle quad corner table (gl_VertexIndex → [-1,1]²).
const vec2 corners[6] = vec2[6](
    vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0),
    vec2(1.0, -1.0),  vec2(1.0, 1.0),  vec2(-1.0, 1.0)
);

// Deterministic per-particle scalar in [0,1) from the seed (same chain as
// the compute shader's pcg1d, kept stable frame to frame).
uint pcg1d(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float seedRand(uint seed) {
    return float(pcg1d(seed) >> 8) * (1.0 / 16777216.0);
}

// Analytic terminal fall speed — mirrors weather_update.comp so streak
// orientation matches the simulated motion.
float fallSpeed(uint type, uint seed) {
    float r = seedRand(seed);
    if (type == WEATHER_TYPE_SNOW)   return mix(1.2, 2.0, r);
    if (type == WEATHER_TYPE_RAIN)   return mix(9.0, 11.0, r);
    if (type == WEATHER_TYPE_DUST)   return mix(0.3, 0.8, r);
    if (type == WEATHER_TYPE_LEAVES) return mix(0.8, 1.5, r);
    return 1.0;
}

void main() {
    uint meta     = floatBitsToUint(inPosSeed.w);
    uint type     = meta & META_TYPE_MASK;
    uint seed     = (meta >> META_SEED_SHIFT) & META_SEED_MASK;
    bool disabled = (meta & META_DISABLED_BIT) != 0u;

    vec3 cam = sceneBuffer.cameras[0].position.xyz;
    vec3 pos = inPosSeed.xyz;
    float dist = length(pos - cam);

    // Estimated world velocity (fall + wind; turbulence is decorative and
    // skipped here) — used only for the rain streak orientation.
    vec3 v = vec3(0.0, -fallSpeed(type, seed) * max(sceneBuffer.weather.look.y, 0.0), 0.0)
           + vec3(sceneBuffer.weather.wind.xy * sceneBuffer.weather.wind.z, 0.0);

    // Quad axes + per-type size.  Everything collapses to a degenerate
    // behind-camera / disabled / fully-faded quad at the end.
    vec3 axisA;  // "long" axis (streak direction or camera right)
    vec3 axisB;  // "short" axis (screen-perpendicular or camera up)
    float sizeA;
    float sizeB;

    mat4 invView = sceneBuffer.cameras[0].invView;
    vec3 camRight = normalize(vec3(invView[0]));
    vec3 camUp    = normalize(vec3(invView[1]));

    float r = seedRand(seed ^ 0x51u);
    if (type == WEATHER_TYPE_RAIN) {
        vec3 longAxis = v;
        longAxis.y = min(longAxis.y, -1e-3);  // guard: always falling
        longAxis = normalize(longAxis);
        vec3 viewDir = (pos - cam) / max(dist, 1e-4);
        vec3 shortAxis = cross(longAxis, viewDir);
        float sl = length(shortAxis);
        shortAxis = (sl > 1e-4) ? shortAxis / sl : camRight;
        axisA = longAxis;
        axisB = shortAxis;
        sizeA = mix(0.5, 0.9, r);              // streak length (m)
        sizeB = 0.06;                          // streak width (m; ≥ 2 px clamp below)
    } else {
        axisA = camRight;
        axisB = camUp;
        if (type == WEATHER_TYPE_SNOW)       sizeA = mix(0.04, 0.10, r);
        else if (type == WEATHER_TYPE_DUST)  sizeA = mix(0.15, 0.45, r);
        else                                 sizeA = mix(0.10, 0.18, r);  // leaves
        sizeB = sizeA;
    }
    sizeA *= max(sceneBuffer.weather.params.w, 0.0);
    sizeB *= max(sceneBuffer.weather.params.w, 0.0);

    // TAA stability: clamp the short axis (and disc size) to ≥ 2 px of
    // apparent size at the particle's distance.
    float worldPerPx = 2.0 * dist / (max(pc.projM11, 1e-4) * float(pc.height));
    float minPx = 2.0 * worldPerPx;
    sizeB = max(sizeB, minPx);
    if (type != WEATHER_TYPE_RAIN) sizeA = max(sizeA, minPx);

    // Per-particle alpha: global opacity × far fade toward the box edge
    // (look.z → params.x).  Disabled particles keep simulating but vanish.
    float alpha = max(sceneBuffer.weather.look.x, 0.0);
    alpha *= 1.0 - smoothstep(sceneBuffer.weather.look.z, max(sceneBuffer.weather.params.x, sceneBuffer.weather.look.z + 1.0), dist);
    if (disabled) alpha = 0.0;

    vec2 c = corners[gl_VertexIndex % 6];
    vec3 worldPos = pos + axisA * (c.x * sizeA) + axisB * (c.y * sizeB);

    vec4 clip = sceneBuffer.cameras[0].viewProjection * vec4(worldPos, 1.0);
    if (clip.w <= 1e-3 || alpha <= 1e-3) {
        // Degenerate: outside the clip volume so the primitive is culled
        // before rasterization — zero fill cost.
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
        outViewZ = 1e9;
    } else {
        gl_Position = clip;
        outViewZ = clip.w;  // RH projection: clip.w == positive view distance
    }

    outQuad    = c;
    outType    = type;
    outSeed    = seed;
    outAlpha   = alpha;
    outWorldPos = pos;
}
