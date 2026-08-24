#pragma once

#include "ecs/system/scene/SceneSystem.h"

typedef struct TerrainHeightfield {
    float* heights;     // gridW * gridH, row-major [y * gridW + x]
    u32    gridW;
    u32    gridH;
    float  spacing;     // meters per cell
    float  minX, minZ;  // world origin
    float  sizeX, sizeZ;
} TerrainHeightfield;

typedef struct TerrainChunk {
    float* positions;     // xyz interleaved (3 floats per vertex)
    float* normals;       // xyz interleaved (3 floats per vertex, nullable)
    float* uvs;           // uv interleaved (2 floats per vertex, nullable)
    float* tangents;      // xyzw interleaved (4 floats per vertex, nullable)
    u32* indices;
    u32 vertexCount;
    u32 indexCount;
    vec3 boundsMin;       // per-chunk bounding box
    vec3 boundsMax;
    u32 materialId;       // material ID for splatmap lookup
    bool visible;
    String name;          // glTF node name (used for Jolt shape lookup)
    JoltMesh* joltMesh;   // pre-baked physics shape (nullable)
} TerrainChunk;

typedef struct Terrain {
    String name;
    Array(TerrainChunk) chunks;
    vec3 boundsMin;       // overall bounding box (all chunks)
    vec3 boundsMax;
    TerrainHeightfield heightfield;  // CPU-side, filled by readback
    void* backendData;    // VulkanTerrain* (set by renderer)
    bool visible;
} Terrain;

/// Load pre-baked Jolt shapes from a .jolt.dat sidecar.
/// The sidecar path is derived from the terrain path: "models/foo.dat" →
/// "models/foo.jolt.dat".  Returns true if the sidecar was loaded successfully.
bool terrainLoadJoltShapes(const char* terrainPath);

/// O(1) height lookup — bilinear interpolation from heightfield grid.
/// Returns terrain height at world (wx, wz), or 0.0f if out of bounds.
float terrainGetHeight(Terrain* terrain, float wx, float wz);

REGISTER_COMPONENT(Terrain);
