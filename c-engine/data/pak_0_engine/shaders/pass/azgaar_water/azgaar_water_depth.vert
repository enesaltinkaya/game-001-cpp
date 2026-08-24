#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
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

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inUV;

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

layout(location = 0) out vec4 outClipCurrent;
layout(location = 1) out vec3 outViewNormal;
layout(location = 2) out vec4 outClipPrev;

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
    float hPrev;  vec3 nPrev;
    waterWaveAt(worldXZ, tCur, hCur, nCur);
    waterWaveAt(worldXZ, tPrev, hPrev, nPrev);

    vec3 worldPosCur  = vec3(worldXZ.x, surfaceY + hCur,  worldXZ.y);
    vec3 worldPosPrev = vec3(worldXZ.x, surfaceY + hPrev, worldXZ.y);

    gl_Position      = sceneBuffer.cameras[0].viewProjection * vec4(worldPosCur, 1.0);
    outClipCurrent   = sceneBuffer.cameras[0].viewProjectionNoJitter * vec4(worldPosCur, 1.0);
    outClipPrev      = sceneBuffer.cameras[0].prevViewProjectionNoJitter * vec4(worldPosPrev, 1.0);
    outViewNormal    = normalize((sceneBuffer.cameras[0].view * vec4(nCur, 0.0)).xyz);
}
