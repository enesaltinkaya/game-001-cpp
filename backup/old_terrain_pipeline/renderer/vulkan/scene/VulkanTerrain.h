#pragma once

#include "renderer/vulkan/resources/VulkanBuffer.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "ecs/system/terrain/Terrain.h"

// Per-chunk draw info for CPU-side frustum culling + direct draw
typedef struct TerrainChunkDraw {
    u32 indexCount;
    u32 firstIndex;
    i32 vertexOffset;
    vec3 boundsMin;
    vec3 boundsMax;
    u32 materialId;
} TerrainChunkDraw;

// GPU-resident terrain data
typedef struct VulkanTerrain {
    VulkanBuffer vertexBuffer;   // SceneVertex[]
    VulkanBuffer indexBuffer;    // u32[]
    VulkanImage  heightfieldTexture;  // R32_SFLOAT, lives until destroy
    TerrainChunkDraw* chunks;    // per-chunk draw params (CPU side)
    u32 chunkCount;
    bool uploaded;
    Terrain* terrain;            // back-reference for visibility flags
} VulkanTerrain;

void vulkanTerrainUpload(Terrain* terrain);
void vulkanTerrainDestroy(VulkanTerrain* vt);
