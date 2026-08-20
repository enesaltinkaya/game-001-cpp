#include "VulkanTerrain.h"
#include "renderer/vulkan/scene/VulkanScene.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "string/String.h"
#include "Utils.h"

void vulkanTerrainUpload(Terrain* terrain) {
    if (!terrain || terrain->chunks == NULL) return;

    VulkanTerrain* vt = memoryAlloc(sizeof(VulkanTerrain));
    *vt = (VulkanTerrain){0};
    vt->terrain = terrain;
    terrain->backendData = vt;

    // Count totals
    u32 totalVertices = 0;
    u32 totalIndices  = 0;
    for (u32 i = 0; i < arraySize(terrain->chunks); i++) {
        TerrainChunk* chunk = &terrain->chunks[i];
        totalVertices += chunk->vertexCount;
        totalIndices  += chunk->indexCount;
    }

    if (totalVertices == 0) return;

    // Pack all chunks into a single VBO (SceneVertex format) and IBO
    SceneVertex* tempVerts = memoryAlloc(totalVertices * sizeof(SceneVertex));
    u32* tempIdxs          = memoryAlloc(totalIndices * sizeof(u32));

    SceneVertex* currVert = tempVerts;
    u32* currIdx          = tempIdxs;
    u32 vertexOffset      = 0;
    u32 indexOffset       = 0;

    vt->chunkCount = arraySize(terrain->chunks);
    vt->chunks     = memoryAlloc(vt->chunkCount * sizeof(TerrainChunkDraw));

    for (u32 i = 0; i < vt->chunkCount; i++) {
        TerrainChunk* chunk = &terrain->chunks[i];

        // Pack vertices into SceneVertex format
        for (u32 v = 0; v < chunk->vertexCount; v++) {
            SceneVertex sv = {0};
            sv.position[0] = chunk->positions[v * 3 + 0];
            sv.position[1] = chunk->positions[v * 3 + 1];
            sv.position[2] = chunk->positions[v * 3 + 2];

            if (chunk->normals) {
                sv.normal[0] = chunk->normals[v * 3 + 0];
                sv.normal[1] = chunk->normals[v * 3 + 1];
                sv.normal[2] = chunk->normals[v * 3 + 2];
            }
            if (chunk->tangents) {
                sv.tangent[0] = chunk->tangents[v * 4 + 0];
                sv.tangent[1] = chunk->tangents[v * 4 + 1];
                sv.tangent[2] = chunk->tangents[v * 4 + 2];
                sv.tangent[3] = chunk->tangents[v * 4 + 3];
            }
            if (chunk->uvs) {
                sv.uv[0] = chunk->uvs[v * 2 + 0];
                sv.uv[1] = chunk->uvs[v * 2 + 1];
            }

            *currVert++ = sv;
        }

        // Copy indices (already local to this chunk)
        memcpy(currIdx, chunk->indices, chunk->indexCount * sizeof(u32));
        currIdx += chunk->indexCount;

        // Store draw params
        vt->chunks[i].indexCount   = chunk->indexCount;
        vt->chunks[i].firstIndex   = indexOffset;
        vt->chunks[i].vertexOffset = (i32)vertexOffset;
        vt->chunks[i].materialId   = chunk->materialId;
        memcpy(vt->chunks[i].boundsMin, chunk->boundsMin, sizeof(vec3));
        memcpy(vt->chunks[i].boundsMax, chunk->boundsMax, sizeof(vec3));

        vertexOffset += chunk->vertexCount;
        indexOffset  += chunk->indexCount;
    }

    // Create GPU buffers
    vt->vertexBuffer = vulkanCreateGpuBuffer(
        strtmp("TerrainVBO"),
        totalVertices * sizeof(SceneVertex),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    vt->indexBuffer = vulkanCreateGpuBuffer(
        strtmp("TerrainIBO"),
        totalIndices * sizeof(u32),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    // Upload
    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanCopy(.cmd = cmd, .source.data = tempVerts, .target.buf = &vt->vertexBuffer,
               .size = totalVertices * sizeof(SceneVertex));
    vulkanCopy(.cmd = cmd, .source.data = tempIdxs, .target.buf = &vt->indexBuffer,
               .size = totalIndices * sizeof(u32));
    vulkanTransientEnd(cmd, 1);

    memoryFree(tempVerts);
    memoryFree(tempIdxs);
    vt->uploaded = true;

    info("vulkanTerrain: uploaded %u chunks (%u verts, %u indices)",
         vt->chunkCount, totalVertices, totalIndices);
}

void vulkanTerrainDestroy(VulkanTerrain* vt) {
    if (!vt) return;
    vulkanDestroyBuffer(&vt->vertexBuffer, VK_NULL_HANDLE);
    vulkanDestroyBuffer(&vt->indexBuffer, VK_NULL_HANDLE);
    if (vt->heightfieldTexture.img) {
        vulkanRemoveImageFromPool(&vt->heightfieldTexture);
        vulkanDestroyImage(&vt->heightfieldTexture, VK_NULL_HANDLE);
    }
    if (vt->chunks) memoryFree(vt->chunks);
    // Free CPU heightfield
    if (vt->terrain && vt->terrain->heightfield.heights) {
        memoryFree(vt->terrain->heightfield.heights);
        vt->terrain->heightfield.heights = NULL;
    }
    memoryFree(vt);
}
