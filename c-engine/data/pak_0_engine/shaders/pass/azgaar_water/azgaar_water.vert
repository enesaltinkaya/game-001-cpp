#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_ARB_shading_language_include : enable

// ── Azgaar water vertex shader ─────────────────────────────────────────
// Renders a camera-following grid that stretches to the horizon (hidden by
// fog).  The grid is authored in local space (-0.5..+0.5 in X/Z) and
// recentered on the camera each frame in the vertex shader, snapped to an
// integer multiple of the cell size so the wave field does not swim.
//
// Wave displacement is a sum-of-sines (Gerstner-like) computed from
// sceneBuffer.water params.  Analytic normals are accumulated and passed
// to the fragment shader for lighting.
//
// Near the beach the Gerstner crests/troughs would dip the surface below
// the (shallow) terrain and poke dry patches into the water ("holes").  The
// wave amplitude is therefore attenuated by water depth: sampled from the
// scene depth buffer, the swell fades out over the last few metres so the
// near-shore surface stays nearly flat while open water keeps full swells.

layout(location = 0) in vec3 inPosition;   // local grid pos (-0.5..0.5)
layout(location = 1) in vec3 inNormal;     // (0,1,0)
layout(location = 2) in vec4 inTangent;    // unused
layout(location = 3) in vec2 inUV;

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outWorldNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) out float outWaveHeightFactor;  // [0,1] for crest foam

// Must match the fragment shader's WaterPushConstants (same push-constant
// payload is pushed once per draw and read by both stages).
layout(push_constant) uniform WaterPushConstants {
    uint  depthIndex;
    uint  width;
    uint  height;
    float nearZ;
    float farZ;
    float projM00;
    float projM11;
    float projM20;
    float projM21;
} pc;

// ── Water-depth sampling (mirrors azgaar_water.frag terrainPosAtPixel) ──
// Project the undisturbed surface point to the screen, fetch the scene depth
// there and recover the terrain height, so the wave amplitude can be damped
// as the seabed approaches.  A sub-pixel TAA jitter offset between the
// unjittered projection and the jittered depth is negligible for this
// smoothly-varying attenuation.
float terrainHeightAt(vec2 xz, float y) {
    vec4 clip = sceneBuffer.cameras[0].viewProjection * vec4(xz.x, y, xz.y, 1.0);
    if (clip.w <= 0.0) return y;  // behind the camera — leave undisturbed
    vec2 ndcV = clip.xy / clip.w;
    vec2 uvV  = vec2(ndcV.x * 0.5 + 0.5, 0.5 - ndcV.y * 0.5);  // y down (0=top)
    vec2 pxV  = clamp(uvV * vec2(pc.width, pc.height), vec2(0.0), vec2(float(pc.width - 1), float(pc.height - 1)));
    float d   = texelFetch(sampler2D(textures[nonuniformEXT(pc.depthIndex)],
                                     samplers[SAMPLER_NEAREST]),
                           ivec2(pxV), 0).r;
    float linZ = (pc.nearZ * pc.farZ) / (pc.nearZ + (pc.farZ - pc.nearZ) * d);
    vec2 uvF   = (pxV + 0.5) / vec2(pc.width, pc.height);
    vec2 ndcF  = vec2(uvF.x * 2.0 - 1.0, 1.0 - uvF.y * 2.0);
    vec3 viewPos = vec3((ndcF.x + pc.projM20) * linZ / pc.projM00,
                        (ndcF.y + pc.projM21) * linZ / pc.projM11,
                        -linZ);
    return (sceneBuffer.cameras[0].invView * vec4(viewPos, 1.0)).xyz.y;
}

// Grid parameters (must match AzgaarWater.c CPU side)
#define AZGAAR_WATER_GRID_SIZE    8192.0
#define AZGAAR_WATER_GRID_DIVS    512.0
#define AZGAAR_WATER_CELL_SIZE    (AZGAAR_WATER_GRID_SIZE / AZGAAR_WATER_GRID_DIVS)

void main() {
    // ── 1. Camera-centered grid recentering ───────────────────────────
    vec2 camPos = sceneBuffer.cameras[0].position.xz;
    // Snap camera to cell size to avoid swimming / aliasing of the wave field.
    // This matches the depth/velocity pre-pass and keeps the wave phase stable
    // across frames. The grid jumps discretely, but the visual result is stable.
    vec2 snappedCam = floor(camPos / AZGAAR_WATER_CELL_SIZE + 0.5) * AZGAAR_WATER_CELL_SIZE;

    // Local grid spans [-0.5, +0.5] → scale to [-GRID_SIZE/2, +GRID_SIZE/2]
    vec2 localXZ = inPosition.xz * AZGAAR_WATER_GRID_SIZE;
    vec2 worldXZ = localXZ + snappedCam;

    // ── 2. Sea level Y from uniform ────────────────────────────────────
    float surfaceY = sceneBuffer.water.surfaceY.x;

    // ── 3. Sum-of-sines vertex displacement ────────────────────────────
    float time = float(sceneBuffer.time) / 1000.0;
    float waveHeight = 0.0;
    vec3 waveNormal = vec3(0.0, 1.0, 0.0);
    float totalAmp = 0.0;

    for (int i = 0; i < 4; i++) {
        vec2 dir = sceneBuffer.water.waveDirAmp[i].xy;
        float amp = sceneBuffer.water.waveDirAmp[i].z;
        float wavelength = sceneBuffer.water.waveDirAmp[i].w;
        float speed = sceneBuffer.water.waveSpeedSteep[i].x;
        float steepness = sceneBuffer.water.waveSpeedSteep[i].y;

        if (wavelength < 1e-3 || amp < 1e-4) continue;

        float k = 2.0 * PI / wavelength;
        float phase = k * dot(worldXZ, dir) - time * speed;
        float s = sin(phase);
        float c = cos(phase);

        // Gerstner-style horizontal + vertical displacement
        float q = min(steepness, 0.9 / (amp * k + 1e-6));
        float qa = q * amp;

        waveHeight += qa * s;
        // Analytic normal derivative
        float dk = k * amp * c;
        waveNormal.x += -dir.x * dk;
        waveNormal.z += -dir.y * dk;
        waveNormal.y += -q * k * amp * s;  // dz contribution

        // Normalise the crest metric by the actual peak displacement (q-scaled
        // amplitude), not the raw amplitude, so outWaveHeightFactor is a true
        // [0,1] crest value (1.0 at a full peak) instead of sitting near 0.5.
        totalAmp += qa;
    }

    // ── 4. Depth-based wave attenuation (calm near the beach) ─────────
    // Sample the terrain height below this vertex and damp the swell as the
    // seabed approaches, so the near-shore surface stays nearly flat and the
    // Gerstner crests/troughs no longer poke dry "holes" into the shallow
    // water.  Open water (deep) keeps the full swell.
    float terrainY = terrainHeightAt(worldXZ, surfaceY);
    float waterDepth = max(surfaceY - terrainY, 0.0);
    float depthCalm  = smoothstep(0.0, 8.0, waterDepth);   // 0 shallow → 1 deep
    float waveScale  = mix(0.10, 1.0, depthCalm);

    // Scale the accumulated normal perturbation (waveNormal = (0,1,0)+perturb)
    // by the same factor so the ripple shading calms down with the swell.
    waveNormal = vec3(0.0, 1.0, 0.0) + (waveNormal - vec3(0.0, 1.0, 0.0)) * waveScale;
    waveNormal = normalize(waveNormal);
    waveHeight *= waveScale;

    float worldY = surfaceY + waveHeight;
    vec3 worldPos = vec3(worldXZ.x, worldY, worldXZ.y);

    gl_Position = sceneBuffer.cameras[0].viewProjection * vec4(worldPos, 1.0);

    outWorldPos = worldPos;
    outWorldNormal = waveNormal;
    outUV = inUV;
    // Normalised wave height factor for crest foam (0 = flat, 1 = peak)
    outWaveHeightFactor = clamp(waveHeight / max(totalAmp, 1e-4) * 0.5 + 0.5, 0.0, 1.0);
}
