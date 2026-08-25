#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_ARB_shading_language_include : enable

// ── Azgaar water depth/velocity vertex shader ───────────────────────────
// Replicates the animated displacement from azgaar_water.vert so that the
// depth/velocity pre-pass can generate correct motion vectors for the
// animated water surface. The grid is authored in local space (-0.5..+0.5)
// and recentered on the camera each frame, snapped to cell size.
//
// Motion vectors must account for BOTH camera motion (current vs previous
// NoJitter projection) AND the time-based wave animation: the "previous"
// clip position must use the wave state at the previous frame's time
// (sceneBuffer.prevTime), not the current time.
//
// This pre-pass runs in its OWN render pass (no depth attachment — see
// VulkanDepthPass.cpp), after the scene depth pass, so the scene depth
// buffer can be sampled as a texture exactly like azgaar_water.vert:
//   * the swell is attenuated by water depth (identical depthCalm curve),
//     so the motion vectors match the surface the color pass displays;
//   * vertices over dry land (terrain above the undisturbed sea level)
//     are collapsed outside the clip volume, so no motion vector is ever
//     written over the beach or over the player character standing on it.
// The fragment shader applies the color pass' per-pixel early-out as well.

layout(location = 0) in vec3 inPosition;   // local grid pos (-0.5..0.5)
layout(location = 1) in vec3 inNormal;     // (0,1,0)
layout(location = 2) in vec4 inTangent;    // unused
layout(location = 3) in vec2 inUV;

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

layout(location = 0) out vec4 outClipCurrent;
layout(location = 1) out vec3 outViewNormal;
layout(location = 2) out vec4 outClipPrev;

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

// ── Water-depth sampling (mirrors azgaar_water.vert terrainHeightAt) ───
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

// Sum-of-sines wave state (height + normal) at world XZ for a given time
// (seconds). Identical to the displacement in azgaar_water.vert.
void waterWaveAt(vec2 worldXZ, float t, out float h, out vec3 n) {
    h = 0.0;
    n = vec3(0.0, 1.0, 0.0);
    for (int i = 0; i < 4; i++) {
        vec2 dir = sceneBuffer.water.waveDirAmp[i].xy;
        float amp = sceneBuffer.water.waveDirAmp[i].z;
        float wavelength = sceneBuffer.water.waveDirAmp[i].w;
        float speed = sceneBuffer.water.waveSpeedSteep[i].x;
        float steepness = sceneBuffer.water.waveSpeedSteep[i].y;

        if (wavelength < 1e-3 || amp < 1e-4) continue;

        float k = 2.0 * 3.14159265358979323846 / wavelength;
        float phase = k * dot(worldXZ, dir) - t * speed;
        float s = sin(phase);
        float c = cos(phase);

        float q = min(steepness, 0.9 / (amp * k + 1e-6));
        float qa = q * amp;

        h += qa * s;
        float dk = k * amp * c;
        n.x += -dir.x * dk;
        n.z += -dir.y * dk;
        n.y += -q * k * amp * s;
    }
    n = normalize(n);
}

void main() {
    // 1. Camera-snapped grid recentering
    vec2 camPos = sceneBuffer.cameras[0].position.xz;
    vec2 snappedCam = floor(camPos / AZGAAR_WATER_CELL_SIZE + 0.5) * AZGAAR_WATER_CELL_SIZE;

    vec2 localXZ = inPosition.xz * AZGAAR_WATER_GRID_SIZE;
    vec2 worldXZ = localXZ + snappedCam;

    // 2. Sea level
    float surfaceY = sceneBuffer.water.surfaceY.x;

    // 3. Wave state at current time AND previous frame's time, so the
    // motion vector captures wave animation, not just camera motion.
    float tCur  = float(sceneBuffer.time) / 1000.0;
    float tPrev = float(sceneBuffer.prevTime) / 1000.0;

    float hCur;  vec3 nCur;
    float hPrev; vec3 nPrev;
    waterWaveAt(worldXZ, tCur, hCur, nCur);
    waterWaveAt(worldXZ, tPrev, hPrev, nPrev);

    // 4. Depth-based wave attenuation (identical to azgaar_water.vert) —
    // the motion vectors must track the CALMED surface the color pass
    // actually displays near the shore, not the full open-ocean swell.
    float terrainY = terrainHeightAt(worldXZ, surfaceY);
    float waterDepth = max(surfaceY - terrainY, 0.0);
    float depthCalm  = smoothstep(0.0, 8.0, waterDepth);   // 0 shallow → 1 deep
    float waveScale  = mix(0.10, 1.0, depthCalm);

    nCur  = vec3(0.0, 1.0, 0.0) + (nCur  - vec3(0.0, 1.0, 0.0)) * waveScale;
    nPrev = vec3(0.0, 1.0, 0.0) + (nPrev - vec3(0.0, 1.0, 0.0)) * waveScale;
    nCur  = normalize(nCur);
    nPrev = normalize(nPrev);
    hCur  *= waveScale;
    hPrev *= waveScale;

    // 5. Dry-land vertex cull: where the terrain is at or above the
    // undisturbed sea level there is no water, so no motion vector must be
    // written (the old in-pass depth-tested draw used GREATER_OR_EQUAL and
    // stamped the swell's motion vectors over the dry beach and the player
    // character standing on it, making them swim with the waves in TAA/FSR).
    // Collapsing the vertex outside the clip volume discards its fragments.
    // The fragment shader' per-pixel early-out (azgaar_water_depth.frag)
    // covers the remaining mixed triangles at the waterline.
    if (terrainY >= surfaceY) {
        gl_Position    = vec4(0.0, 0.0, -2.0, 1.0);  // outside NDC → clipped
        outClipCurrent = gl_Position;
        outClipPrev    = gl_Position;
        outViewNormal  = vec3(0.0, 1.0, 0.0);
        return;
    }

    vec3 worldPosCur  = vec3(worldXZ.x, surfaceY + hCur,  worldXZ.y);
    vec3 worldPosPrev = vec3(worldXZ.x, surfaceY + hPrev, worldXZ.y);

    gl_Position      = sceneBuffer.cameras[0].viewProjection * vec4(worldPosCur, 1.0);
    outClipCurrent   = sceneBuffer.cameras[0].viewProjectionNoJitter * vec4(worldPosCur, 1.0);
    outClipPrev      = sceneBuffer.cameras[0].prevViewProjectionNoJitter * vec4(worldPosPrev, 1.0);
    outViewNormal    = normalize((sceneBuffer.cameras[0].view * vec4(nCur, 0.0)).xyz);
}