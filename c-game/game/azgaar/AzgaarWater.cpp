#include "azgaar/AzgaarWater.h"
#include "azgaar/AzgaarWorld.h"
#include "azgaar/AzgaarWeather.h"
#include "renderer/vulkan/pass/azgaar_water/VulkanAzgaarWaterPass.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/scene/VulkanScene.h"
#include "renderer/Renderer.h"
#include "Utils.h"
#include <math.h>

namespace game {
static bool waterInitialized = false;
static float lastCamX = 0.0f;
static float lastCamZ = 0.0f;
static float cellSize = 0.0f;

// Build a grid mesh: AZGAAR_WATER_GRID_DIVS x AZGAAR_WATER_GRID_DIVS quads.
// Vertices span [-0.5, +0.5] in X/Z (local space), Y=0.
// The vertex shader recenters on the camera.
static void buildGridMesh(engine::SceneVertex* vertices, u32* indices,
                           u32* outVertexCount, u32* outIndexCount) {
    u32 divs = AZGAAR_WATER_GRID_DIVS;
    u32 vertsX = divs + 1;
    u32 vertsZ = divs + 1;

    u32 vertexCount = vertsX * vertsZ;
    u32 indexCount = divs * divs * 6;  // 2 triangles per quad

    float invDivs = 1.0f / static_cast<float>(divs);

    for (u32 iz = 0; iz < vertsZ; iz++) {
        float v = static_cast<float>(iz) * invDivs;
        // Store local-space positions in [-0.5, +0.5]. The vertex shader
        // scales these by AZGAAR_WATER_GRID_SIZE so that world vertices land
        // on multiples of CELL_SIZE and the camera-snap keeps the wave field
        // anchored to a stable world grid (pre-scaling here would double the
        // scale and make the wave/ripple phase jump whenever the camera snaps).
        float localZ = (static_cast<float>(iz) * invDivs - 0.5f);
        for (u32 ix = 0; ix < vertsX; ix++) {
            float u = static_cast<float>(ix) * invDivs;
            float localX = (static_cast<float>(ix) * invDivs - 0.5f);
            u32 idx = iz * vertsX + ix;

            vertices[idx].position[0] = localX;
            vertices[idx].position[1] = 0.0f;
            vertices[idx].position[2] = localZ;

            vertices[idx].normal[0] = 0.0f;
            vertices[idx].normal[1] = 1.0f;
            vertices[idx].normal[2] = 0.0f;

            vertices[idx].tangent[0] = 0.0f;
            vertices[idx].tangent[1] = 0.0f;
            vertices[idx].tangent[2] = 0.0f;
            vertices[idx].tangent[3] = 0.0f;

            vertices[idx].uv[0] = u;
            vertices[idx].uv[1] = v;

            vertices[idx].joints = 0;
            vertices[idx].weights = 0;
        }
    }

    u32 i = 0;
    for (u32 iz = 0; iz < divs; iz++) {
        for (u32 ix = 0; ix < divs; ix++) {
            u32 v0 = iz * vertsX + ix;
            u32 v1 = iz * vertsX + (ix + 1);
            u32 v2 = (iz + 1) * vertsX + ix;
            u32 v3 = (iz + 1) * vertsX + (ix + 1);

            // Triangle 1
            indices[i++] = v0;
            indices[i++] = v2;
            indices[i++] = v1;
            // Triangle 2
            indices[i++] = v1;
            indices[i++] = v2;
            indices[i++] = v3;
        }
    }

    *outVertexCount = vertexCount;
    *outIndexCount = indexCount;
}

void azgaarWaterInit(const AzgaarWorld* world) {
    if (!world) return;

    float seaLevel = azgaarSeaLevelMeters(world);

    // Ripple direction from the map's authored wind (settings winds[0],
    // degrees; workstream F).  FMG leaves it 0 on unauthored maps — keep the
    // old constant in that case.
    float windRad = world->winds[0] != 0.0f
                        ? world->winds[0] * static_cast<float>(M_PI) / 180.0f
                        : 0.5f;

    // Upload initial water params
    engine::VulkanWaterData params = {
        .surfaceY              = {seaLevel,
                                  AZGAAR_WATER_GRID_SIZE,
                                  static_cast<float>(AZGAAR_WATER_GRID_DIVS),
                                  0.0f},
        .shallowColor          = {0.05f, 0.25f, 0.45f, 5.0f},
        .deepColor             = {0.01f, 0.05f, 0.15f, 40.0f},
        .foamColor             = {0.95f, 0.95f, 1.0f, 0.3f},
        .waveDirAmp            = {
            {0.8f, 0.6f, 0.4f, 40.0f},
            {0.5f, -0.5f, 0.2f, 20.0f},
            {0.3f, 0.7f, 0.1f, 15.0f},
            {0.9f, 0.1f, 0.15f, 10.0f},
        },
        .waveSpeedSteep       = {
            // x = wave speed. Kept slow so the swell doesn't look frantic
            // when viewed up close at the shoreline.
            {0.6f, 0.8f, 0.0f, 0.0f},
            {0.4f, 0.5f, 0.0f, 0.0f},
            {0.75f, 0.3f, 0.0f, 0.0f},
            {1.0f, 0.2f, 0.0f, 0.0f},
        },
        .fresnelPower         = 5.0f,
        .fresnelScale         = 0.6f,
        .normalStrength       = 1.5f,
        .rippleScale          = 0.02f,
        .windAngle            = windRad,
        .sunSpecularPower     = 64.0f,
        .sunSpecularIntensity = 1.5f,
        .enabled              = 1.0f,
    };
    engine::vulkanResourceSetWaterParams(&params);

    // Build and upload the grid mesh
    u32 divs = AZGAAR_WATER_GRID_DIVS;
    u32 vertsX = divs + 1;
    u32 vertexCount = vertsX * (divs + 1);
    u32 indexCount = divs * divs * 6;

    std::vector<engine::SceneVertex> vertices(vertexCount);
    std::vector<u32> indices(indexCount);

    u32 vCount = 0, iCount = 0;
    buildGridMesh(vertices.data(), indices.data(), &vCount, &iCount);

    vulkanAzgaarWaterSetMesh(vertices.data(), vCount, indices.data(), iCount);

    waterInitialized = true;
    cellSize = AZGAAR_WATER_GRID_SIZE / static_cast<float>(divs);
    lastCamX = lastCamZ = 0.0f;
}

void azgaarWaterUpdate(float camX, float camZ) {
    if (!waterInitialized) return;

    // The grid is recentered in the vertex shader, so no CPU update needed
    // for the mesh.  We just track the camera position for potential
    // future use (e.g. LOD, foam based on distance from shore).
    lastCamX = camX;
    lastCamZ = camZ;

    // Wind coherence (workstream F / plans/azgaar-weather-gpu-particles.md
    // D8): when the weather module is live, the ripple direction follows
    // the same gusty wind that drives the weather particles and the props
    // sway — one source so flakes, grass and waves stay coherent.
    float dirX, dirZ, speed;
    if (azgaarWeatherGetWind(&dirX, &dirZ, &speed)) {
        engine::VulkanWaterData params = engine::vulkanResourceGetWaterData();
        float angle            = atan2f(dirZ, dirX);
        if (angle != params.windAngle) {
            params.windAngle = angle;
            engine::vulkanResourceSetWaterParams(&params);
        }
    }
}

void azgaarWaterDestroy(void) {
    if (!waterInitialized) return;
    engine::vulkanAzgaarWaterClear();
    waterInitialized = false;
}
}  // namespace game
