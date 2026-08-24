#include "ecs/system/terrain/TerrainParser.h"
#include "ecs/system/terrain/Terrain.h"
#include "ecs/Ecs.h"
#include "renderer/vulkan/scene/VulkanTerrain.h"
#include "renderer/material/Material.h"
#include "renderer/material/MaterialManager.h"
#include "renderer/texture/TextureManager.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/Renderer.h"
#include "json/Json.h"
#include "cgltf/git/cgltf.h"
#include "meshoptimizer/git/src/meshoptimizer.h"
#include "zstd/git/lib/zstd.h"
#include "logger/Logger.h"
#include "datamanager/DataManager.h"
#include "string/String.h"
#include "Utils.h"

#include <limits.h>

// SplatInfo parsing (same logic as SceneParser)
static thread_local StrMap(Json*) terrainSplatInfoMap;
static const char* terrainModelPath;

static void terrainParseNodeSplatInfo(cgltf_node* node);
static u32 terrainParseMaterial(cgltf_primitive* prim);
static void terrainLoadSplatInfoSidecar(const char* path);
static u32 terrainCreateSplatMaterial(const char* path);
static u32 terrainLoadSplatTiles(const char* splatBaseDir, const char* groupKey,
                                  u32 groupIndex);

// ── Pre-baked Jolt shapes (loaded from .jolt.dat sidecar) ─────────────────
typedef struct {
    const void* blob;
    u32 blobSize;
} PrecomputedJoltShape;

static void loadTerrainJoltShapes(const char* terrainPath, Array(TerrainChunk)* chunks);

static void* terrainAlloc(void* _, cgltf_size size) {
    return memoryAlloc(size);
}

static void terrainFree(void* _, void* ptr) {
    memoryFree(ptr);
}

static void* terrainDecompressZstd(String* fileData, u32* sizeOut, const char* path) {
    u64 rSize = ZSTD_getFrameContentSize(fileData->data, fileData->size);
    if (rSize == ZSTD_CONTENTSIZE_ERROR) {
        terminate("terrainParser: invalid zstd size: %s", path);
    }
    void* rBuff = memoryAlloc(rSize);
    u64 dSize   = ZSTD_decompress(rBuff, rSize, fileData->data, fileData->size);

    if (ZSTD_isError(dSize) != 0U) {
        terminate("terrainParser: zstd error: %s", ZSTD_getErrorName(dSize));
    }

    if (rSize != dSize) {
        terminate("terrainParser: zstd size mismatch: %s", path);
    }
    *sizeOut = rSize;
    return rBuff;
}

static cgltf_result terrainDecompressMeshopt(cgltf_data* data) {
    for (size_t i = 0; i < data->buffer_views_count; ++i) {
        if (!data->buffer_views[i].has_meshopt_compression) continue;

        cgltf_meshopt_compression* mc = &data->buffer_views[i].meshopt_compression;
        const unsigned char* source   = (const unsigned char*)mc->buffer->data;
        if (!source) return cgltf_result_invalid_gltf;
        source += mc->offset;

        void* result = memoryAlloc(mc->count * mc->stride);
        if (!result) return cgltf_result_out_of_memory;

        i32 rc = -1;
        switch (mc->mode) {
            case cgltf_meshopt_compression_mode_invalid:
                break;
            case cgltf_meshopt_compression_mode_attributes:
                rc = meshopt_decodeVertexBuffer(result, mc->count, mc->stride, source, mc->size);
                break;
            case cgltf_meshopt_compression_mode_triangles:
                rc = meshopt_decodeIndexBuffer(result, mc->count, mc->stride, source, mc->size);
                break;
            case cgltf_meshopt_compression_mode_indices:
                rc = meshopt_decodeIndexSequence(result, mc->count, mc->stride, source, mc->size);
                break;
            case cgltf_meshopt_compression_mode_max_enum:
                break;
        }
        if (rc != 0) return cgltf_result_io_error;

        switch (mc->filter) {
            case cgltf_meshopt_compression_filter_none:
                break;
            case cgltf_meshopt_compression_filter_octahedral:
                meshopt_decodeFilterOct(result, mc->count, mc->stride);
                break;
            case cgltf_meshopt_compression_filter_quaternion:
                meshopt_decodeFilterQuat(result, mc->count, mc->stride);
                break;
            case cgltf_meshopt_compression_filter_exponential:
                meshopt_decodeFilterExp(result, mc->count, mc->stride);
                break;
            default:
                break;
        }
        data->buffer_views[i].data = result;
    }
    return cgltf_result_success;
}

static cgltf_data* terrainParseGltfData(void* glbData, u32 glbSize, const char* path) {
    cgltf_options options = {
        .memory.alloc_func = terrainAlloc,
        .memory.free_func  = terrainFree,
    };

    cgltf_data* cData   = NULL;
    cgltf_result result = cgltf_parse(&options, glbData, glbSize, &cData);
    if (result != 0) {
        terminate("terrainParser: cgltf_parse failed: %d", result);
    }

    result = cgltf_load_buffers(&options, cData, NULL);
    if (result != 0) {
        terminate("terrainParser: cgltf_load_buffers failed: %d", result);
    }

    if (terrainDecompressMeshopt(cData) != cgltf_result_success) {
        terminate("terrainParser: decompressMeshopt failed: %s", path);
    }

    return cData;
}

Terrain* terrainLoad(const char* path) {
    return terrainLoadCb(path, NULL, NULL);
}

Terrain* terrainLoadCb(const char* path, TerrainLoadCallback callback, void* userData) {
    info("terrainParser: loading terrain %s", path);

    // Read and decompress the terrain file
    String fileData = dataManagerRead(path);
    u32 glbSize;
    void* glbData = terrainDecompressZstd(&fileData, &glbSize, path);
    stringDestroy(&fileData);

    // Parse glTF (includes meshopt decompression)
    cgltf_data* cgltfData = terrainParseGltfData(glbData, glbSize, path);
    terrainModelPath = path;

    // Create the Terrain object
    Terrain* terrain = memoryAlloc(sizeof(Terrain));
    *terrain = (Terrain){0};
    stringPrintf(&terrain->name, path);

    terrain->boundsMin[0] = FLT_MAX;
    terrain->boundsMin[1] = FLT_MAX;
    terrain->boundsMin[2] = FLT_MAX;
    terrain->boundsMax[0] = -FLT_MAX;
    terrain->boundsMax[1] = -FLT_MAX;
    terrain->boundsMax[2] = -FLT_MAX;

    u32 totalVerts = 0;
    u32 totalIdxs  = 0;

    // Try loading splatInfo from sidecar file first (the terrain-chunker
    // strips node extras, so the sidecar is the primary source for chunked terrain)
    terrainLoadSplatInfoSidecar(path);

    // The terrain-chunker produces GLB without materials.  Create a single
    // shared terrain splat material for all chunks upfront.
    u32 sharedMaterialId = terrainCreateSplatMaterial(path);

    // Extract per-chunk data from each mesh node
    for (i32 i = 0, si = cgltfData->scene->nodes_count; i < si; i++) {
        cgltf_node* node = cgltfData->scene->nodes[i];
        if (!node->mesh) continue;

        // Parse splatInfo from node extras (may override sidecar if present)
        terrainParseNodeSplatInfo(node);

        for (u64 pi = 0; pi < node->mesh->primitives_count; pi++) {
            cgltf_primitive* prim = &node->mesh->primitives[pi];

            // Find attribute accessors
            cgltf_accessor* posAcc  = NULL;
            cgltf_accessor* normAcc = NULL;
            cgltf_accessor* uvAcc   = NULL;
            cgltf_accessor* tanAcc  = NULL;

            for (u64 j = 0; j < prim->attributes_count; j++) {
                cgltf_attribute* attr = &prim->attributes[j];
                switch (attr->type) {
                    case cgltf_attribute_type_position: posAcc  = attr->data; break;
                    case cgltf_attribute_type_normal:   normAcc = attr->data; break;
                    case cgltf_attribute_type_texcoord: uvAcc   = attr->data; break;
                    case cgltf_attribute_type_tangent:  tanAcc  = attr->data; break;
                    default: break;
                }
            }

            if (!posAcc) continue;

            TerrainChunk chunk = {0};
            chunk.vertexCount = (u32)posAcc->count;
            chunk.visible     = true;
            // Use the primitive's material if present, otherwise fall back to
            // the shared splat material created from the sidecar.
            chunk.materialId  = prim->material
                ? terrainParseMaterial(prim)
                : sharedMaterialId;
           // Store node name for Jolt shape lookup
            if (node->name) {
                stringPrintf(&chunk.name, "%s", node->name);
            }

            // Extract positions
            chunk.positions = memoryAlloc(chunk.vertexCount * 3 * sizeof(float));
            cgltf_accessor_unpack_floats(posAcc, chunk.positions, chunk.vertexCount * 3);

            // Extract normals
            if (normAcc) {
                chunk.normals = memoryAlloc(chunk.vertexCount * 3 * sizeof(float));
                cgltf_accessor_unpack_floats(normAcc, chunk.normals, chunk.vertexCount * 3);
            }

            // Extract UVs
            if (uvAcc) {
                chunk.uvs = memoryAlloc(chunk.vertexCount * 2 * sizeof(float));
                cgltf_accessor_unpack_floats(uvAcc, chunk.uvs, chunk.vertexCount * 2);
            }

            // Extract tangents
            if (tanAcc) {
                chunk.tangents = memoryAlloc(chunk.vertexCount * 4 * sizeof(float));
                cgltf_accessor_unpack_floats(tanAcc, chunk.tangents, chunk.vertexCount * 4);
            }

            // Extract indices
            if (prim->indices) {
                chunk.indexCount = (u32)prim->indices->count;
                chunk.indices    = memoryAlloc(chunk.indexCount * sizeof(u32));
                cgltf_accessor_unpack_indices(prim->indices, chunk.indices, sizeof(u32), chunk.indexCount);
            }

            // Compute per-chunk bounds
            chunk.boundsMin[0] = FLT_MAX;
            chunk.boundsMin[1] = FLT_MAX;
            chunk.boundsMin[2] = FLT_MAX;
            chunk.boundsMax[0] = -FLT_MAX;
            chunk.boundsMax[1] = -FLT_MAX;
            chunk.boundsMax[2] = -FLT_MAX;

            for (u32 v = 0; v < chunk.vertexCount; v++) {
                float px = chunk.positions[v * 3];
                float py = chunk.positions[v * 3 + 1];
                float pz = chunk.positions[v * 3 + 2];
                if (px < chunk.boundsMin[0]) chunk.boundsMin[0] = px;
                if (py < chunk.boundsMin[1]) chunk.boundsMin[1] = py;
                if (pz < chunk.boundsMin[2]) chunk.boundsMin[2] = pz;
                if (px > chunk.boundsMax[0]) chunk.boundsMax[0] = px;
                if (py > chunk.boundsMax[1]) chunk.boundsMax[1] = py;
                if (pz > chunk.boundsMax[2]) chunk.boundsMax[2] = pz;
            }

            // Update overall terrain bounds
            for (int d = 0; d < 3; d++) {
                if (chunk.boundsMin[d] < terrain->boundsMin[d]) terrain->boundsMin[d] = chunk.boundsMin[d];
                if (chunk.boundsMax[d] > terrain->boundsMax[d]) terrain->boundsMax[d] = chunk.boundsMax[d];
            }

            totalVerts += chunk.vertexCount;
            totalIdxs  += chunk.indexCount;

            arrayPut(terrain->chunks, chunk);
        }
    }

    // Add terrain to the global terrains array
    arrayPut(ecs.terrains, terrain);

    // Load pre-baked Jolt shapes from sidecar (e.g. "models/foo.jolt.dat")
    loadTerrainJoltShapes(path, &terrain->chunks);

    info("terrainParser: loaded terrain '%s' (%u chunks, %u verts, %u idxs)",
          path, (u32)arraySize(terrain->chunks), totalVerts, totalIdxs);
    info("terrainParser: world bounds min=(%f,%f,%f) max=(%f,%f,%f)",
         terrain->boundsMin[0], terrain->boundsMin[1], terrain->boundsMin[2],
         terrain->boundsMax[0], terrain->boundsMax[1], terrain->boundsMax[2]);

    // Cleanup glTF data and splatmap map
    for (i32 i = 0, si = strmapSize(terrainSplatInfoMap); i < si; i++) {
        json_decref(terrainSplatInfoMap[i].value);
    }
    strmapFree(terrainSplatInfoMap);
    terrainSplatInfoMap = NULL;
    terrainModelPath   = NULL;

    memoryFree(glbData);
    cgltf_free(cgltfData);

    if (callback) callback(terrain, userData);
    return terrain;
}

void terrainDestroy(Terrain* terrain) {
    if (!terrain) return;

    if (terrain->backendData) {
        rendererWaitIdle("terrain destroy");
        vulkanTerrainDestroy((VulkanTerrain*)terrain->backendData);
        terrain->backendData = NULL;
    }

    for (u32 i = 0; i < arraySize(terrain->chunks); i++) {
        TerrainChunk* chunk = &terrain->chunks[i];
        if (chunk->joltMesh) {
            joltMeshDestroy(chunk->joltMesh);
            chunk->joltMesh = NULL;
        }
        if (chunk->positions) memoryFree(chunk->positions);
        if (chunk->normals)   memoryFree(chunk->normals);
        if (chunk->uvs)       memoryFree(chunk->uvs);
        if (chunk->tangents)  memoryFree(chunk->tangents);
        if (chunk->indices)   memoryFree(chunk->indices);
        stringDestroy(&chunk->name);
    }
    arrayFree(terrain->chunks);
    stringDestroy(&terrain->name);

    for (i32 i = 0, si = (i32)arraySize(ecs.terrains); i < si; i++) {
        if (ecs.terrains[i] == terrain) {
            arrayDeleteSlow(ecs.terrains, i);
            break;
        }
    }

    memoryFree(terrain);
}

// ── Pre-baked Jolt shapes sidecar loading ─────────────────────────────────

static StrMap(PrecomputedJoltShape) terrainJoltShapes;
static void* terrainJoltData;

bool terrainLoadJoltShapes(const char* terrainPath) {
    terrainJoltShapes = NULL;
    terrainJoltData   = NULL;

    /* Derive sidecar path: "models/foo.dat" → "models/foo.jolt.dat" */
    String sidecarPath = {0};
    size_t pathLen     = strlen(terrainPath);
    if (pathLen > 4 && strequals(terrainPath + pathLen - 4, ".dat")) {
        stringAppendBinary(&sidecarPath, (void*)terrainPath, pathLen - 4);
    } else {
        stringAppend(&sidecarPath, terrainPath);
    }
    stringAppend(&sidecarPath, ".jolt.dat");

    if (!dataManagerFileExists(sidecarPath.data)) {
        stringDestroy(&sidecarPath);
        return false;
    }

    info("terrainParser: loading pre-baked Jolt shapes from %s", sidecarPath.data);
    String fileData = dataManagerRead(sidecarPath.data);
    stringDestroy(&sidecarPath);

    /* Decompress zstd */
    u32 rawSize;
    void* raw = terrainDecompressZstd(&fileData, &rawSize, "jolt shapes sidecar");
    stringDestroy(&fileData);
    terrainJoltData = raw;

    /* Parse binary format:
     *   u8[4]   magic "JBVH"
     *   u32     version
     *   u32     entry_count
     *   Per entry:
     *     u32     node_name_length
     *     char[]  node_name
     *     u32     shape_blob_size
     *     u8[]    shape_blob
     */
    const u8* p   = raw;
    const u8* end = p + rawSize;

    if (rawSize < 12 || memcmp(p, "JBVH", 4) != 0) {
        warn("terrainParser: invalid jolt shapes sidecar header");
        return false;
    }
    p += 4;

    u32 version;
    memcpy(&version, p, 4);
    p += 4;
    if (version != 1 && version != 2) {
        warn("terrainParser: unsupported jolt shapes sidecar version %u", version);
        return false;
    }

    u32 entryCount;
    memcpy(&entryCount, p, 4);
    p += 4;

    for (u32 e = 0; e < entryCount; e++) {
        if (p + 4 > end) break;
        u32 nameLen;
        memcpy(&nameLen, p, 4);
        p += 4;
        if (p + nameLen > end) break;

        char* name = memoryAlloc(nameLen + 1);
        memcpy(name, p, nameLen);
        name[nameLen] = '\0';
        p += nameLen;

        if (version == 2) {
            if (p + 14 > end) { memoryFree(name); break; }
            p += 14;
        }

        if (p + 4 > end) {
            memoryFree(name);
            break;
        }
        u32 blobSize;
        memcpy(&blobSize, p, 4);
        p += 4;
        if (p + blobSize > end) {
            memoryFree(name);
            break;
        }

        PrecomputedJoltShape shape = {.blob = p, .blobSize = blobSize};
        p += blobSize;

        strmapPut(terrainJoltShapes, name, shape);
        memoryFree(name);
    }

    info("terrainParser: loaded %u pre-baked Jolt shape entries", entryCount);
    return true;
}

static void loadTerrainJoltShapes(const char* terrainPath, Array(TerrainChunk)* chunks) {
    if (!terrainLoadJoltShapes(terrainPath)) return;

    u32 matched = 0;
    for (u32 i = 0; i < arraySize(*chunks); i++) {
        TerrainChunk* chunk = &(*chunks)[i];
        if (chunk->name.data && chunk->name.size > 0) {
            PrecomputedJoltShape shape = strmapGet(terrainJoltShapes, chunk->name.data);
            if (shape.blob) {
                float zero[3] = {0, 0, 0};
                float identity[4] = {0, 0, 0, 1};
                chunk->joltMesh = joltCreateMeshShapeFromBlob(
                    shape.blob, shape.blobSize, zero, identity,
                    0xFFFFFFFFFFFFFFFFULL /* terrain sentinel */);
                if (chunk->joltMesh) {
                    matched++;
                } else {
                    warn("terrainParser: failed to create Jolt mesh for chunk '%s'", chunk->name.data);
                }
            }
        }
    }

    if (matched > 0) {
        info("terrainParser: created %u Jolt physics shapes for terrain", matched);
    }

    /* Free sidecar data */
    strmapFree(terrainJoltShapes);
    terrainJoltShapes = NULL;
    if (terrainJoltData) {
        memoryFree(terrainJoltData);
        terrainJoltData = NULL;
    }
}

// ── Splatmap parsing for terrain ──────────────────────────────────────

static u32 terrainLoadSplatTiles(const char* splatBaseDir, const char* groupKey,
                                  u32 groupIndex) {
   // Build the path prefix for this group's tiles:
    //   "<splatBaseDir><groupKey>/<groupKey>."
    String prefix = {0};
    stringAppend(&prefix, splatBaseDir);
    stringAppend(&prefix, groupKey);
    stringAppend(&prefix, "/");
    stringAppend(&prefix, groupKey);
    stringAppend(&prefix, ".");

    u32 prefixLen = prefix.size;
    u32 loadedTiles = 0;

    Array(String) allKtx2 = dataManagerListFiles(".ktx2");
    for (u32 i = 0; i < arraySize(allKtx2); i++) {
        String* filePath = &allKtx2[i];
        if (filePath->size < prefixLen + 6) { // need at least ".ktx2" after prefix
            continue;
        }
        if (strncmp(filePath->data, prefix.data, prefixLen) != 0) {
            continue;
        }

        // Extract UDIM number from "<groupKey>.<udimNumber>.ktx2"
        // After the prefix, the remainder is "<udimNumber>.ktx2"
        const char* afterPrefix = filePath->data + prefixLen;
        char* endPtr;
        u32 udimNumber = (u32)strtol(afterPrefix, &endPtr, 10);
        if (endPtr[0] != '.' || udimNumber < 1000 || udimNumber > 1099) {
            continue;
        }

      // Compute grid index from UDIM number's last two digits:
         // UDIM 10xy -> Blender grid row x, column (y-1) -> array index gridRow*10+gridCol
         // Row is reversed (9-row) to account for Blender-to-engine Y/Z axis swap.
         // Column uses (udim % 100 - 1) % 10 so UDIM 1001 -> col 0, 1010 -> col 9, 1011 -> col 0.
         u32 blenderRow = (udimNumber / 10) - 100;      // e.g. 1023 -> 2
         u32 gridRow = 9 - blenderRow;                  // e.g. 9-2 = 7
         u32 gridCol = (udimNumber % 100 - 1) % 10;     // e.g. 1023 -> 3, 1001 -> 0
         u32 gridIndex = gridRow * 10 + gridCol;        // e.g. 7*10+3 = 73

        if (gridIndex >= SPLAT_UDIM_TILES) {
            continue;
        }

        Texture* tileTex = getTextureByName(filePath->data);
        if (!tileTex) {
            continue;
        }
        tileTex->refCount++;
        vulkanResourceSetTerrainSplatUdim(groupIndex, gridIndex, (u32)tileTex->id);
        loadedTiles++;
    }

   arrayFree(allKtx2);
    stringDestroy(&prefix);

    return loadedTiles;
}

static void terrainLoadSplatInfoSidecar(const char* path) {
    // Derive sidecar path: "models/terrain/foo.dat" →
    // "models/terrain/foo.terrain-splatinfo.splatinfo.json"
    size_t pathLen = strlen(path);
    String sidecarPath = {0};
    if (pathLen > 4 && strequals(path + pathLen - 4, ".dat")) {
        stringAppendBinary(&sidecarPath, (void*)path, pathLen - 4);
    } else {
        stringAppend(&sidecarPath, path);
    }
    stringAppend(&sidecarPath, ".terrain-splatinfo.splatinfo.json");

    if (!dataManagerFileExists(sidecarPath.data)) {
        stringDestroy(&sidecarPath);
        return;
    }

    String fileData = dataManagerRead(sidecarPath.data);
    stringDestroy(&sidecarPath);
    if (!fileData.data || fileData.size == 0) return;

    // Null-terminate for JSON parser
    stringAppend(&fileData, "\0");

    Json* splatInfo = jsonParse(fileData.data);
    stringDestroy(&fileData);
    if (!splatInfo || !json_is_object(splatInfo)) {
        if (splatInfo) jsonFree(splatInfo);
        return;
    }

    const char* key;
    json_t* value;
    (void)value;
    json_object_foreach(splatInfo, key, value) {
        // Store the full splatInfo object under each group key
        // (same convention as node-extras parsing)
        json_t* copy = json_deep_copy(splatInfo);
        if (!copy) continue;
        strmapPut(terrainSplatInfoMap, key, copy);
    }

    jsonFree(splatInfo);
}

static void terrainParseNodeSplatInfo(cgltf_node* node) {
    if (!node->extras.data) return;

    Json* extras = jsonParse((char*)node->extras.data);
    if (!extras) return;

    json_t* splatInfo = json_object_get(extras, "splatInfo");
    if (!splatInfo || !json_is_object(splatInfo)) {
        jsonFree(extras);
        return;
    }

    const char* key;
    json_t* value;
    (void)value;
    json_object_foreach(splatInfo, key, value) {
        json_t* copy = json_deep_copy(splatInfo);
        if (!copy) continue;
        strmapPut(terrainSplatInfoMap, key, copy);
        debug("terrainParser: parsed splatInfo key '%s'", key);
    }

    jsonFree(extras);
}

/// Create a single shared terrain splat material from terrainSplatInfoMap.
/// Used when the chunked GLB has no materials (terrain-chunker strips them).
static u32 terrainCreateSplatMaterial(const char* path) {
    if (strmapSize(terrainSplatInfoMap) == 0) return 0;

    Material* material = getMaterialByName("terrain_splat");
    if (!material) material = createMaterial("terrain_splat");
    material->baseColor[0] = material->baseColor[1] = material->baseColor[2] =
        material->baseColor[3] = 1.0f;
    material->rmas[0] = material->rmas[2] = material->rmas[3] = 1.0f;
    material->rmas[1] = 0.0f;

    // Use the first splatInfo entry (contains all groups)
    Json* fullSplatInfo = terrainSplatInfoMap[0].value;
    if (!fullSplatInfo || !json_is_object(fullSplatInfo)) {
        rendererUploadMaterial(material);
        return material->id;
    }

    const char* groupKey;
    json_t* groupValue;
    u32 groupIndex = 0;

    String splatBaseDir = {0};
    if (path) {
        const char* lastSlash = strrchr(path, '/');
        const char* filename  = lastSlash ? lastSlash + 1 : path;
        const char* firstDot  = strchr(filename, '.');
        u32 stemLen = firstDot ? (u32)(firstDot - filename) : (u32)strlen(filename);
        char stem[256];
        memcpy(stem, filename, stemLen);
        stem[stemLen] = '\0';
        if (lastSlash) {
            u32 dirLen = (u32)(lastSlash - path + 1);
            stringAppendBinary(&splatBaseDir, (void*)path, dirLen);
        }
        stringAppend(&splatBaseDir, stem);
        stringAppend(&splatBaseDir, "/");
    }

    json_object_foreach(fullSplatInfo, groupKey, groupValue) {
        if (groupIndex >= MAX_SPLAT_GROUPS) break;
        if (!json_is_object(groupValue)) continue;

        // Load UDIM weight tiles (scan for actual files with arbitrary UDIM numbers)
        if (splatBaseDir.size > 0) {
            terrainLoadSplatTiles(splatBaseDir.data, groupKey, groupIndex);
        }

        // Load detail textures
        const char* channelNames[] = {"red", "green", "blue", "alpha"};
        for (u32 ch = 0; ch < MAX_SPLAT_CHANNELS; ch++) {
            u32 detailIdx = groupIndex * MAX_SPLAT_CHANNELS + ch;
            const char* detailName =
                json_string_value(json_object_get(groupValue, channelNames[ch]));

            if (!detailName) {
                material->splatAlbedoTextures[detailIdx] = 0;
                material->splatNormalTextures[detailIdx] = 0;
                continue;
            }

            String albedoPath = {0};
            stringPrintf(&albedoPath, "images/terrain/%s/albedo.ktx2", detailName);
            Texture* albedoTex = getTextureByName(albedoPath.data);
            if (albedoTex) {
                albedoTex->refCount++;
            } else {
                albedoTex = getTextureByName(albedoPath.data);
            }
           material->splatAlbedoTextures[detailIdx] = albedoTex ? (u32)albedoTex->id : 0;
            stringDestroy(&albedoPath);

            String normalPath = {0};
            stringPrintf(&normalPath, "images/terrain/%s/normal.ktx2", detailName);
            Texture* normalTex = getTextureByName(normalPath.data);
            if (normalTex) {
                normalTex->refCount++;
            } else {
                normalTex = getTextureByName(normalPath.data);
            }
           material->splatNormalTextures[detailIdx] = normalTex ? (u32)normalTex->id : 0;
            stringDestroy(&normalPath);
        }

        groupIndex++;
    }

    if (groupIndex > 0) {
        material->splatGroupCount = groupIndex;
        material->featureMask |= (1u << MAT_HAS_SPLATMAP);
       vulkanResourceSetTerrainSplatGroupCount(groupIndex);
    }

   stringDestroy(&splatBaseDir);
    rendererUploadMaterial(material);

    return material->id;
}

static u32 terrainParseMaterial(cgltf_primitive* prim) {
    if (!prim->material) return 0;

    const char* name = prim->material->name ? prim->material->name : "terrain_default";
    Material* existing = getMaterialByName(name);
    if (existing) return existing->id;

    Material* material = createMaterial(name);
    material->baseColor[0] = material->baseColor[1] = material->baseColor[2] =
        material->baseColor[3] = 1.0f;
    material->rmas[0] = material->rmas[2] = material->rmas[3] = 1.0f;
    material->rmas[1] = 0.0f;

    // Parse standard PBR textures (simplified — terrain materials rarely have them)
    if (prim->material->has_pbr_metallic_roughness) {
        material->rmas[0] = prim->material->pbr_metallic_roughness.roughness_factor;
        material->rmas[1] = prim->material->pbr_metallic_roughness.metallic_factor;
    }

    // Parse splatmap textures from nodeSplatInfoMap
    {
        Json* fullSplatInfo = NULL;
        if (strmapSize(terrainSplatInfoMap) > 0) {
            fullSplatInfo = strmapGet(terrainSplatInfoMap, name);
            if (!fullSplatInfo) fullSplatInfo = terrainSplatInfoMap[0].value;
        }
        if (fullSplatInfo && json_is_object(fullSplatInfo)) {
            const char* groupKey;
            json_t* groupValue;
            u32 groupIndex = 0;

            String splatBaseDir = {0};
            if (terrainModelPath) {
                const char* lastSlash = strrchr(terrainModelPath, '/');
                const char* filename  = lastSlash ? lastSlash + 1 : terrainModelPath;
                const char* firstDot  = strchr(filename, '.');
                u32 stemLen = firstDot ? (u32)(firstDot - filename) : (u32)strlen(filename);
                char stem[256];
                memcpy(stem, filename, stemLen);
                stem[stemLen] = '\0';
                if (lastSlash) {
                    u32 dirLen = (u32)(lastSlash - terrainModelPath + 1);
                    stringAppendBinary(&splatBaseDir, (void*)terrainModelPath, dirLen);
                }
                stringAppend(&splatBaseDir, stem);
                stringAppend(&splatBaseDir, "/");
            }

            json_object_foreach(fullSplatInfo, groupKey, groupValue) {
                if (groupIndex >= MAX_SPLAT_GROUPS) break;
                if (!json_is_object(groupValue)) continue;

                // Load UDIM weight tiles (scan for actual files with arbitrary UDIM numbers)
                if (splatBaseDir.size > 0) {
                    terrainLoadSplatTiles(splatBaseDir.data, groupKey, groupIndex);
                }

                // Load detail textures
                const char* channelNames[] = {"red", "green", "blue", "alpha"};
                for (u32 ch = 0; ch < MAX_SPLAT_CHANNELS; ch++) {
                    u32 detailIdx = groupIndex * MAX_SPLAT_CHANNELS + ch;
                    const char* detailName =
                        json_string_value(json_object_get(groupValue, channelNames[ch]));

                    if (!detailName) {
                        material->splatAlbedoTextures[detailIdx] = 0;
                        material->splatNormalTextures[detailIdx] = 0;
                        continue;
                    }

                    String albedoPath = {0};
                    stringPrintf(&albedoPath, "images/terrain/%s/albedo.ktx2", detailName);
                    Texture* albedoTex = getTextureByName(albedoPath.data);
                    if (albedoTex) {
                        albedoTex->refCount++;
                    } else {
                        albedoTex = getTextureByName(albedoPath.data);
                    }
                  material->splatAlbedoTextures[detailIdx] = albedoTex ? albedoTex->id : 0;
                     stringDestroy(&albedoPath);

                    String normalPath = {0};
                    stringPrintf(&normalPath, "images/terrain/%s/normal.ktx2", detailName);
                    Texture* normalTex = getTextureByName(normalPath.data);
                    if (normalTex) {
                        normalTex->refCount++;
                    } else {
                        normalTex = getTextureByName(normalPath.data);
                    }
                  material->splatNormalTextures[detailIdx] = normalTex ? normalTex->id : 0;
                     stringDestroy(&normalPath);
                }

                groupIndex++;
            }

            if (groupIndex > 0) {
                material->splatGroupCount = groupIndex;
                material->featureMask |= (1u << MAT_HAS_SPLATMAP);
              vulkanResourceSetTerrainSplatGroupCount(groupIndex);
            }

            stringDestroy(&splatBaseDir);
        }
    }

    rendererUploadMaterial(material);
    return material->id;
}
