#include "cgltf/git/cgltf.h"  // IWYU pragma: keep
#include "ecs/Ecs.h"
#include "ecs/components/Skin.h"
#include "ecs/system/scene/SceneSystem.h"
#include "ecs/system/scene/SceneParser.h"
#include "ecs/system/scene/SceneParserTerrain.h"
#include "json/Json.h"
#include "ecs/system/animation/AnimatorComponent.h"
#include "ecs/system/light/LightComponent.h"
#include "ecs/system/light/LightSystem.h"
#include "ecs/system/mesh/MeshComponent.h"
#include "ecs/system/physics/PhysicsSystem.h"
#include "ecs/system/transform/TransformComponent.h"
#include "ecs/system/transform/TransformSystem.h"
#include "datamanager/DataManager.h"
#include "file/File.h"
#include "logger/Logger.h"
#include "meshoptimizer/git/src/meshoptimizer.h"
#include "platform/Platform.h"
#include "renderer/Renderer.h"
#include "renderer/material/Material.h"
#include "renderer/material/MaterialManager.h"
#include "renderer/texture/TextureManager.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "string/String.h"
#include "zstd/git/lib/zstd.h"  // IWYU pragma: keep
#include <limits.h>

namespace engine {
static void* decompressZstd(utils::String* gltfFileData, u32* sizeOut, const char* path);
static cgltf_data* parseGltfData(void* glbData, u32 glbSize, const char* pPath);
static cgltf_result decompressMeshopt(cgltf_data* data);
static Entity* parseNode(Scene* scene, cgltf_node* node, Entity* parent);
static void parseTransform(Scene* scene, cgltf_node* node, Entity* entity, Entity* parent);
static void parseMesh(Scene* scene, cgltf_node* node, Entity* entity);
static void parsePhysicsMesh(Scene* scene, cgltf_node* node, Entity* entity);
static void parseRigidBody(Scene* scene, cgltf_node* node, Entity* entity);
static void parseSkin(Scene* scene, cgltf_node* node, Entity* entity);
static void parseLight(Scene* scene, cgltf_node* node, Entity* entity);
u32 parseMaterial(cgltf_primitive* cgltfPrimitive);
static void parseAnimation(cgltf_animation* cgltfAnim);
static void parseAnimationEventsFromExtras(AnimationClip* clip, const char* extrasJson);
static void parseNodeSplatInfo(cgltf_node* node);
static u32 sceneLoadSplatTiles(const char* splatBaseDir, const char* groupKey,
                                u32 groupIndex) {
    utils::String prefix = {};
    utils::stringAppend(&prefix, splatBaseDir);
    utils::stringAppend(&prefix, groupKey);
    utils::stringAppend(&prefix, "/");
    utils::stringAppend(&prefix, groupKey);
    utils::stringAppend(&prefix, ".");

    u32 prefixLen = prefix.size;
    u32 loadedTiles = 0;

    std::vector<utils::String> allKtx2 = utils::dataManagerListFiles(".ktx2");
    for (u32 i = 0; i < allKtx2.size(); i++) {
        utils::String* filePath = &allKtx2[i];
        if (filePath->size < prefixLen + 6) continue;
        if (strncmp(filePath->data, prefix.data, prefixLen) != 0) continue;

        const char* afterPrefix = filePath->data + prefixLen;
        char* endPtr;
        u32 udimNumber = (u32)strtol(afterPrefix, &endPtr, 10);
        if (endPtr[0] != '.' || udimNumber < 1000 || udimNumber > 1099) continue;

        u32 gridRow = (udimNumber / 10) - 100;
        u32 gridCol = udimNumber % 10;
        u32 gridIndex = gridRow * 10 + gridCol;

        if (gridIndex >= SPLAT_UDIM_TILES) continue;

        Texture* tileTex = getTextureByName(filePath->data);
        if (!tileTex) continue;
        tileTex->refCount++;
        vulkanResourceSetTerrainSplatUdim(groupIndex, gridIndex, (u32)tileTex->id);
        loadedTiles++;
    }

    utils::stringDestroy(&prefix);

    utils::info("sceneParser: splat group '%s': loaded %u/%u UDIM tiles (scan)",
         groupKey, loadedTiles, SPLAT_UDIM_TILES);
    return loadedTiles;
}

struct SceneLoadRequest {
    const char* path;
    Scene* scene;
    SceneLoadCallback callback;
    void* userData;
};

static void sceneLoadOffThread(void* pRequest);

/// Map a glTF sampler to one of the engine's pre-built sampler indices.
/// Takes both filter mode and wrap mode into account.
static u32 mapGltfSampler(cgltf_sampler* sampler) {
    if (!sampler) return SAMPLER_LINEAR;
    bool nearest = sampler->mag_filter == cgltf_filter_type_nearest;
    bool clamp   = sampler->wrap_s == cgltf_wrap_mode_clamp_to_edge ||
                   sampler->wrap_t == cgltf_wrap_mode_clamp_to_edge;
    if (nearest && clamp) return SAMPLER_CLAMP_NEAREST;
    if (nearest) return SAMPLER_NEAREST;
    if (clamp) return SAMPLER_CLAMP_LINEAR;
    return SAMPLER_LINEAR;
}

static thread_local std::unordered_map<uintptr_t, u32> meshCache;
static thread_local std::unordered_map<uintptr_t, Entity*> nodeCache;

// SplatInfo mapping: splatmap texture name -> full splatInfo JSON object
static thread_local std::unordered_map<std::string, Json*> nodeSplatInfoMap;
static thread_local const char* currentModelPath;

/// Compute the world-space position, rotation and uniform scale for a cgltf
/// node by walking up the parent chain.  This mirrors the logic in
/// TransformSystem.transformSetWorld() but works on the raw glTF node data so
/// it can be called at parse time before the ECS world transforms have been
/// computed.
static void cgltfNodeWorldTransform(const cgltf_node* node,
                                    float worldPos[3],
                                    float worldRot[4],
                                    float* worldScale) {
    // Build a stack of ancestors (inclusive of `node` itself).
    const cgltf_node* stack[128];
    i32 depth = 0;
    for (const cgltf_node* n = node; n && depth < 128; n = n->parent) {
        stack[depth++] = n;
    }

    // Start from the root (last entry) and accumulate.
    worldPos[0] = 0.0f;
    worldPos[1] = 0.0f;
    worldPos[2] = 0.0f;
    // Identity quaternion
    worldRot[0] = 0.0f;
    worldRot[1] = 0.0f;
    worldRot[2] = 0.0f;
    worldRot[3] = 1.0f;
    *worldScale  = 1.0f;

    for (i32 i = depth - 1; i >= 0; i--) {
        const cgltf_node* n = stack[i];
        float localPos[3]   = {n->translation[0], n->translation[1], n->translation[2]};
        float localRot[4]   = {n->rotation[0], n->rotation[1], n->rotation[2], n->rotation[3]};
        float localScale = n->has_scale ? n->scale[0] : 1.0f; // uniform scale

        // child world pos = parent world pos + parentRot * (parentScale * childLocalPos)
        vec3 scaledLocal = {localPos[0] * (*worldScale),
                            localPos[1] * (*worldScale),
                            localPos[2] * (*worldScale)};
        vec3 rotatedLocal;
        glm_quat_rotatev(worldRot, scaledLocal, rotatedLocal);
        worldPos[0] += rotatedLocal[0];
        worldPos[1] += rotatedLocal[1];
        worldPos[2] += rotatedLocal[2];

        // child world rot = parentRot * childRot
        vec4 newRot;
        glm_quat_mul(worldRot, localRot, newRot);
        glm_quat_normalize(newRot);
        worldRot[0] = newRot[0];
        worldRot[1] = newRot[1];
        worldRot[2] = newRot[2];
        worldRot[3] = newRot[3];

        // child world scale = parentScale * childScale
        *worldScale *= localScale;
    }
}

/* ── Pre-baked Jolt shapes (loaded from .jolt.dat sidecar) ─────────────── */
struct PrecomputedJoltShape {
    const void* blob;
    u32 blobSize;
    u8 shapeTag;      // PHYSICS_BLOB_TAG_*
    u8 motionTag;     // 0=STATIC, 1=DYNAMIC
    float mass;
    float friction;
    float restitution;
};

static thread_local std::unordered_map<std::string, PrecomputedJoltShape> precomputedJoltShapes;
static thread_local void* precomputedJoltData; /* raw sidecar buffer */

static void loadPrecomputedJoltShapes(const char* scenePath);
static void freePrecomputedJoltShapes(void);

static thread_local cgltf_data* currentCgltfData;

Scene* sceneLoad(const char* path) {
    return sceneLoadCb(path, nullptr, nullptr);
}

Scene* sceneLoadCb(const char* path, SceneLoadCallback callback, void* userData) {
    Scene* scene = new Scene{};
    scene->asyncLoadPending = true;  // freed/handled by sceneLoadMainThread (or sceneDestroy's deferral)
    SceneLoadRequest* req = new SceneLoadRequest{
        .path     = path,
        .scene    = scene,
        .callback = callback,
        .userData = userData,
    };
    utils::threadPoolAddWork(nullptr, sceneLoadOffThread, req);
    return scene;
}

void sceneLoadMainThread(void* pScene) {
    Scene* scene  = static_cast<Scene*>(pScene);
    scene->asyncLoadPending = 0;

    // Destroyed while the async load was in flight: sceneDestroy() could not
    // free the scene (the worker thread was still parsing into it) and
    // deferred the free to here.  Discard it without registering it with the
    // ECS or invoking the load callback.
    if (scene->destroyRequested) {
        utils::info("sceneParser: scene '%s' was destroyed while loading; discarding",
             scene->name.data ? scene->name.data : "(unnamed)");
        rendererSceneDestroy(scene);
        sceneDestroy(scene);
        return;
    }

    // Re-activate all entities so their 2-second upload window starts NOW
    // (on the main thread) rather than when they were parsed on the worker
    // thread.  Under heavy CPU load the worker can take >2 s, which would
    // cause every transform to expire before postUpdate() ever uploads it.
    double now = utils::millies();
    for (auto& entry : scene->activeEntities) {
        entry.second = now;
    }

    ecs.scenes.push_back(scene);
    scene->ready = true;

    if (scene->loadCallback) {
        scene->loadCallback(scene, scene->loadCallbackUserData);
    }
}

void sceneLoadOffThread(void* pRequest) {
    SceneLoadRequest* req        = static_cast<SceneLoadRequest*>(pRequest);
    const char* path            = req->path;
    Scene* scene                = req->scene;
    scene->loadCallback         = req->callback;
    scene->loadCallbackUserData = req->userData;
    delete req;

    // The per-thread parse caches are keyed by raw cgltf node/mesh pointers.
    // After a previous load on this pool thread, the freed cgltf buffer can be
    // re-allocated to a NEW scene at the same address, which would make stale
    // entries hit and return entity IDs from the dead scene.  Reset them.
    meshCache.clear();
    nodeCache.clear();

    utils::info("sceneParser: loading scene %s", path);
    utils::String fileData = utils::dataManagerRead(path);
    u32 glbSize;
    void* glbData           = decompressZstd(&fileData, &glbSize, path);
    cgltf_data* cgltfData   = parseGltfData(glbData, glbSize, path);
    currentCgltfData        = cgltfData;
    currentModelPath        = path;
    utils::stringPrintf(&scene->name, path);

    /* Try loading pre-baked Jolt shapes sidecar (e.g. .jolt.dat) */
    loadPrecomputedJoltShapes(path);

    for (i32 i = 0, si = cgltfData->scene->nodes_count; i < si; i++) {
        cgltf_node* node = cgltfData->scene->nodes[i];
        parseNode(scene, node, nullptr);
    }

    // now that we have nodeCache, we can parse skin component
    // Iterate ALL nodes (not just root), since skinned meshes may be children
    for (i32 i = 0, si = cgltfData->nodes_count; i < si; i++) {
        cgltf_node* node = &cgltfData->nodes[i];
        uintptr_t     nodeKey = (uintptr_t)node;
        auto it = nodeCache.find(nodeKey);
        Entity* entity   = it != nodeCache.end() ? it->second : nullptr;
        if (entity && node->skin) {
            parseSkin(scene, node, entity);
        }
    }

    // Parse animations
    for (i32 i = 0, si = cgltfData->animations_count; i < si; i++) {
        parseAnimation(&cgltfData->animations[i]);
    }

    rendererSceneCreate(scene);
    utils::futureTaskAdd(0, sceneLoadMainThread, scene);

    freePrecomputedJoltShapes();

    // Free nodeSplatInfoMap
    for (const auto& entry : nodeSplatInfoMap) {
        json_decref(entry.second);
    }
    nodeSplatInfoMap.clear();
    currentModelPath = nullptr;

    free(glbData);
    utils::stringDestroy(&fileData);
    currentCgltfData = nullptr;
    cgltf_free(cgltfData);
}

Entity* parseNode(Scene* scene, cgltf_node* node, Entity* parent) {
    Entity* entity = createEntity(scene, node->name);
    uintptr_t nodeKey = (uintptr_t)node;
    nodeCache[nodeKey] = entity;

    if (parent) {
        entity->parent = parent;
        parent->children.push_back(entity);
    }

    // Store raw node extras on the scene so game code can inspect them later.
    if (node->extras.data) {
        Json* stored = jsonParse((char*)node->extras.data);
        if (stored) {
            scene->extras[entity->id] = stored;
        }
    }

    parseTransform(scene, node, entity, parent);

    // Parse splatInfo from node extras (for terrain splatmap textures)
    // Must happen before parseMesh so the splatInfo map is populated
    // when parseMaterial looks up detail textures.
    parseNodeSplatInfo(node);

    bool isPhysicsOnly = false;

    if (node->mesh) {
        if (node->extras.data) {
            Json* extras = jsonParse((char*)node->extras.data);
            if (extras) {
                json_t* noTex = json_object_get(extras, "notexture");
                if (noTex && json_is_true(noTex)) {
                    isPhysicsOnly = true;
                }
                jsonFree(extras);
            }
        }

        if (isPhysicsOnly) {
            parsePhysicsMesh(scene, node, entity);
        } else {
            parseMesh(scene, node, entity);
        }
    }

    // Parse rigid body from glTF extras (set by 1-blender-scene.py from
    // Blender's rigid body collision shape).  Must happen after parseMesh
    // so the Mesh component AABB is available for primitive shape sizing.
    parseRigidBody(scene, node, entity);

    if (node->light) {
        parseLight(scene, node, entity);
    }

    for (i32 i = 0, si = node->children_count; i < si; i++) {
        parseNode(scene, node->children[i], entity);
    }

    return entity;
}

void parseNodeSplatInfo(cgltf_node* node) {
    if (!node->extras.data) return;

    Json* extras = jsonParse((char*)node->extras.data);
    if (!extras) return;

    json_t* splatInfo = json_object_get(extras, "splatInfo");
    if (!splatInfo || !json_is_object(splatInfo)) {
        jsonFree(extras);
        return;
    }

    // Each key gets its own deep copy so cleanup is straightforward.
    const char* key;
    json_t* value;
    (void)value;
    json_object_foreach(splatInfo, key, value) {
        json_t* splatInfoCopy = json_deep_copy(splatInfo);
        if (!splatInfoCopy) continue;
        nodeSplatInfoMap[key] = splatInfoCopy;
        utils::debug("sceneParser: parsed splatInfo key '%s'", key);
    }

    jsonFree(extras);
}

void parseTransform(Scene* scene, cgltf_node* node, Entity* entity, Entity* parent) {
    if (parent) createComponent(scene, WorldTransform, entity->id);
    Transform* transform = createComponent(scene, Transform, entity->id);
    glm_vec3_copy(node->translation, transform->pos);
    glm_vec4_ucopy(node->rotation, transform->rot);
    transform->pos[3] = node->has_scale ? node->scale[0] : 1.0f;
    transformActivate(scene, entity->id);

    // Armatures are zeroed out before glTF export so that gltfpack doesn't
    // break them.  The original location is stored in extras as
    // "armatureLocation": [x, y, z] (already in glTF Y-up convention).
    if (node->extras.data) {
        Json* extras = jsonParse((char*)node->extras.data);
        if (extras) {
            json_t* armLoc = json_object_get(extras, "armatureLocation");
            if (armLoc && json_is_array(armLoc) && json_array_size(armLoc) == 3) {
                transform->pos[0] += (float)json_number_value(json_array_get(armLoc, 0));
                transform->pos[1] += (float)json_number_value(json_array_get(armLoc, 1));
                transform->pos[2] += (float)json_number_value(json_array_get(armLoc, 2));
                // debug("sceneParser: restored armature location for '%s': (%.2f, %.2f, %.2f)",
                // node->name ? node->name : "(unnamed)",
                // transform->pos[0], transform->pos[1], transform->pos[2]);
            }
            jsonFree(extras);
        }
    }

    // i disabled this assert because after gltfpack, scale values in .glb file
    // can be 0.9999994 for example. and that causes the assert to trigger. so
    // i'll just use the node->scale[0] and if i notice weird animations or
    // weird scales, ill check the models in blender to make sure they have
    // uniform scales. assert(node->scale[0] == node->scale[1] && node->scale[0]
    // == node->scale[2] && "only uniform scale is supported!");
}

void* decompressZstd(utils::String* gltfFileData, u32* sizeOut, const char* path) {
    u64 rSize = ZSTD_getFrameContentSize(gltfFileData->data, gltfFileData->size);
    if (rSize == ZSTD_CONTENTSIZE_ERROR) {
        utils::terminate("sceneParser: invalid zstd size: %s %lu %lu", path, rSize, gltfFileData->size);
    }
    void* rBuff = malloc(rSize);
    u64 dSize   = ZSTD_decompress(rBuff, rSize, gltfFileData->data, gltfFileData->size);

    if (ZSTD_isError(dSize) != 0U) {
        utils::terminate("sceneParser: zstd error: %s", ZSTD_getErrorName(dSize));
    }

    if (rSize != dSize) {
        utils::terminate("sceneParser: zstd size mismatch: %s", path);
    }
    *sizeOut = rSize;
    return rBuff;
}

static void* cgltfAlloc(void* _, cgltf_size size) {
    return malloc(size);
}

static void cgltfFree(void* _, void* ptr) {
    free(ptr);
}

cgltf_data* parseGltfData(void* glbData, u32 glbSize, const char* _) {
    cgltf_options options = {};
    options.memory.alloc_func = cgltfAlloc;
    options.memory.free_func  = cgltfFree;

    cgltf_data* cData   = nullptr;
    cgltf_result result = cgltf_parse(&options, glbData, glbSize, &cData);
    if (result != 0) {
        utils::terminate("sceneParser: cgltf_parse failed: %d", result);
    }
    result = cgltf_load_buffers(&options, cData, 0);
    if (result != 0) {
        utils::terminate("sceneParser: cgltf_load_buffers failed: %d", result);
    }
#ifndef NDEBUG
    result = cgltf_validate(cData);
    if (result != 0) {
        utils::terminate("sceneParser: cgltf_validate failed: %d", result);
    }
#endif

#ifndef NDEBUG
    utils::createDirectory("scripts/gltf-json-debug");
    utils::String fileName  = {};
    utils::String directory = {};
    utils::stringAppend(&fileName, _);
    std::vector<utils::String*> split = utils::stringSplit(&fileName, "/");
    utils::String* lastPart     = split[static_cast<i32>(split.size()) - 1];
    utils::stringAppend(lastPart, ".json");
    utils::stringAppend(&directory, "scripts/gltf-json-debug/");
    utils::stringAppend(&directory, lastPart->data);
    utils::fileWriteBinary(directory.data, (char*)cData->json, cData->json_size);

    utils::stringDestroy(&fileName);
    utils::stringDestroy(&directory);
    utils::stringArrayDestroy(split);

#endif

    if (decompressMeshopt(cData) != cgltf_result_success) {
        utils::terminate("sceneParser: decompressMeshopt failed!");
    }

    return cData;
}

cgltf_result decompressMeshopt(cgltf_data* data) {
    for (size_t i = 0; i < data->buffer_views_count; ++i) {
        if (!data->buffer_views[i].has_meshopt_compression) {
            continue;
        }
        cgltf_meshopt_compression* mc = &data->buffer_views[i].meshopt_compression;
        const unsigned char* source   = (const unsigned char*)mc->buffer->data;
        if (!source) {
            return cgltf_result_invalid_gltf;
        }
        source += mc->offset;
        void* result = malloc(mc->count * mc->stride);
        if (!result) {
            return cgltf_result_out_of_memory;
        }
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

        if (rc != 0) {
            return cgltf_result_io_error;
        }

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
            // case cgltf_meshopt_compression_filter_max_enum:
            // break;
            default:
                break;
        }
        data->buffer_views[i].data = result;
    }
    return cgltf_result_success;
}

/* ── Pre-baked Jolt shapes sidecar loading ──────────────────────────────── */

void loadPrecomputedJoltShapes(const char* scenePath) {
    precomputedJoltShapes.clear();
    precomputedJoltData   = nullptr;

    /* Derive sidecar path: "models/foo.dat" → "models/foo.jolt.dat" */
    utils::String sidecarPath = {};
    size_t pathLen     = strlen(scenePath);
    if (pathLen > 4 && utils::strequals(scenePath + pathLen - 4, ".dat")) {
        utils::stringAppendBinary(&sidecarPath, const_cast<char*>(scenePath), pathLen - 4);
    } else {
        utils::stringAppend(&sidecarPath, scenePath);
    }
    utils::stringAppend(&sidecarPath, ".jolt.dat");

    if (!utils::dataManagerFileExists(sidecarPath.data)) {
        utils::stringDestroy(&sidecarPath);
        return;
    }

    utils::info("sceneParser: loading pre-baked Jolt shapes from %s", sidecarPath.data);
    utils::String fileData = utils::dataManagerRead(sidecarPath.data);
    utils::stringDestroy(&sidecarPath);

    /* Decompress zstd */
    u32 rawSize;
    void* raw = decompressZstd(&fileData, &rawSize, "jolt shapes sidecar");
    utils::stringDestroy(&fileData);
    precomputedJoltData = raw;

    /* Parse binary format:
     *   u8[4]   magic "JBVH"
     *   u32     version (1 or 2)
     *   u32     entry_count
     *   Per entry (v1):
     *     u32     node_name_length
     *     char[]  node_name
     *     u32     shape_blob_size
     *     u8[]    shape_blob
     *   Per entry (v2):
     *     u32     node_name_length
     *     char[]  node_name
     *     u8      shape_type
     *     u8      motion_type
     *     float   mass
     *     float   friction
     *     float   restitution
     *     u32     shape_blob_size
     *     u8[]    shape_blob
     */
    const u8* p    = static_cast<const u8*>(raw);
    const u8* end = p + rawSize;

    if (rawSize < 12 || memcmp(p, "JBVH", 4) != 0) {
        utils::warn("sceneParser: invalid jolt shapes sidecar header");
        return;
    }
    p += 4;

    u32 version;
    memcpy(&version, p, 4);
    p += 4;
    if (version != 1 && version != 2) {
        utils::warn("sceneParser: unsupported jolt shapes sidecar version %u", version);
        return;
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

        /* Build null-terminated name for the strmap key */
        std::string name(reinterpret_cast<const char*>(p), nameLen);
        p += nameLen;

        PrecomputedJoltShape shape = {};

        if (version == 2) {
            /* v2: shapeTag, motionTag, mass, friction, restitution, then blob */
            if (p + 1 > end) { break; }
            shape.shapeTag = *p; p += 1;
            if (p + 1 > end) { break; }
            shape.motionTag = *p; p += 1;
            if (p + 12 > end) { break; }
            memcpy(&shape.mass, p, 4);         p += 4;
            memcpy(&shape.friction, p, 4);      p += 4;
            memcpy(&shape.restitution, p, 4);   p += 4;
        }

        if (p + 4 > end) {
            break;
        }
        u32 blobSize;
        memcpy(&blobSize, p, 4);
        p += 4;
        if (p + blobSize > end) {
            break;
        }

        shape.blob     = p;
        shape.blobSize = blobSize;
        p += blobSize;

        /* v1 fallback: assume static mesh (notexture / MESH) */
        if (version == 1) {
            shape.shapeTag = 0; /* NOTEXTURE — matches old behavior */
            shape.motionTag = 0;
            shape.mass        = 0.0f;
            shape.friction    = 0.5f;
            shape.restitution = 0.0f;
        }

        precomputedJoltShapes[name] = shape;
    }

    utils::info("sceneParser: loaded %u pre-baked Jolt shape entries", entryCount);
}

void freePrecomputedJoltShapes(void) {
    precomputedJoltShapes.clear();
    if (precomputedJoltData) {
        free(precomputedJoltData);
        precomputedJoltData = nullptr;
    }
}

static void sceneMergeBounds(Scene* scene, vec3 min, vec3 max) {
    if (!scene->hasBounds) {
        glm_vec3_copy(min, scene->aabbMin);
        glm_vec3_copy(max, scene->aabbMax);
        scene->hasBounds = true;
    } else {
        glm_vec3_minv(scene->aabbMin, min, scene->aabbMin);
        glm_vec3_maxv(scene->aabbMax, max, scene->aabbMax);
    }
}

void parseMesh(Scene* scene, cgltf_node* node, Entity* entity) {
    // Check if mesh is already cached
    uintptr_t meshKey = (uintptr_t)node->mesh;
    auto it = meshCache.find(meshKey);
    u32 meshEntityId = it != meshCache.end() ? it->second : 0;
    if (meshEntityId) {
        Mesh* mesh            = getComponent(scene, Mesh, meshEntityId);
        InstanceData instance = {entity->id};
        mesh->instances.push_back(instance);
        return;
    }

    Mesh* mesh            = createComponentT(scene, Mesh, entity->id);
    InstanceData instance = {.entity = entity->id};
    mesh->instances.push_back(instance);
    meshCache[meshKey] = entity->id;

    // Initialize AABB
    for (i32 i = 0; i < 3; i++) {
        mesh->aabbLocal[0][i] = FLT_MAX;
        mesh->aabbLocal[1][i] = -FLT_MAX;
    }

    // Process primitives
    for (u64 i = 0; i < node->mesh->primitives_count; i++) {
        cgltf_primitive* gltfPrimitive = &node->mesh->primitives[i];

        Primitive primitive                   = {};
        const cgltf_accessor* indicesAccessor = gltfPrimitive->indices;

        // Copy vertex attributes
        primitive.indexCount = indicesAccessor->count;
        primitive.indices.resize(primitive.indexCount);
        cgltf_accessor_unpack_indices(indicesAccessor,
                                      primitive.indices.data(),
                                      sizeof(u32),
                                      primitive.indexCount);

        for (u64 j = 0; j < gltfPrimitive->attributes_count; j++) {
            cgltf_attribute* attribute = &gltfPrimitive->attributes[j];
            if (attribute->type == cgltf_attribute_type_position) {
                const cgltf_accessor* position = attribute->data;
                primitive.vertexCount          = position->count;
                primitive.positions.resize(primitive.vertexCount * 3);
                cgltf_accessor_unpack_floats(position,
                                             primitive.positions.data(),
                                             primitive.vertexCount * 3);
                primitive.attributeMask |= (1 << cgltf_attribute_type_position);
                vec3 aabb[2] = {{position->min[0], position->min[1], position->min[2]},
                                {position->max[0], position->max[1], position->max[2]}};
                glm_aabb_merge(aabb, mesh->aabbLocal, mesh->aabbLocal);
            } else if (attribute->type == cgltf_attribute_type_color) {
                // Unpack COLOR_0 into floats (per-part colour).  VEC3 or VEC4;
                // the props pass only needs the first 3 (RGB).
                const cgltf_accessor* colorAccessor = attribute->data;
                size_t comps = cgltf_num_components(colorAccessor->type);
                primitive.colors.resize(primitive.vertexCount * comps);
                cgltf_accessor_unpack_floats(colorAccessor, primitive.colors.data(), primitive.vertexCount * comps);
                primitive.attributeMask |= (1 << cgltf_attribute_type_color);
            } else {
                cgltf_accessor* accessor = attribute->data;
                size_t attrSize          = primitive.vertexCount * accessor->stride;
                primitive.attributes[attribute->type].resize(attrSize);
                const u8* src = cgltf_buffer_view_data(accessor->buffer_view) + accessor->offset;
                memcpy(primitive.attributes[attribute->type].data(), src, attrSize);
                primitive.attributeMask |= (1 << attribute->type);
            }
        }

        primitive.materialId = parseMaterial(gltfPrimitive);

        mesh->primitives.push_back(primitive);
    }

    // Transform local AABB to world space before merging into scene bounds.
    // The frustum culler tests the scene AABB against world-space planes,
    // so the scene bounds must be in world space.
    float worldPos[3], worldRot[4], worldScale;
    cgltfNodeWorldTransform(node, worldPos, worldRot, &worldScale);

    mat4 worldMat;
    glm_translate_make(worldMat, worldPos);
    glm_quat_rotate(worldMat, worldRot, worldMat);
    glm_scale_uni(worldMat, worldScale);

    vec3 worldAabb[2];
    glm_aabb_transform(mesh->aabbLocal, worldMat, worldAabb);
    sceneMergeBounds(scene, worldAabb[0], worldAabb[1]);

    // DEBUG: log mesh AABB for character models
    if (entity->name && (strstr(entity->name, "eve") || strstr(entity->name, "armature"))) {
        vec3 aabbSize;
        glm_vec3_sub(mesh->aabbLocal[1], mesh->aabbLocal[0], aabbSize);
        utils::info("meshAABB: entity='%s' local=(%.3f,%.3f,%.3f) size=(%.3f,%.3f,%.3f) "
             "worldMin=(%.2f,%.2f,%.2f) worldMax=(%.2f,%.2f,%.2f) scale=%.4f",
             entity->name,
             mesh->aabbLocal[0][0], mesh->aabbLocal[0][1], mesh->aabbLocal[0][2],
             aabbSize[0], aabbSize[1], aabbSize[2],
             worldAabb[0][0], worldAabb[0][1], worldAabb[0][2],
             worldAabb[1][0], worldAabb[1][1], worldAabb[1][2],
             worldScale);
    }
}

void parsePhysicsMesh(Scene* scene, cgltf_node* node, Entity* entity) {
    // Compute world transform by walking the glTF parent chain.
    // At parse time the ECS WorldTransform hasn't been computed yet.
    float pos[3], rot[4], worldScale;
    cgltfNodeWorldTransform(node, pos, rot, &worldScale);

    // Try pre-baked Jolt shape first (avoids BVH construction at load time).
    if (node->name && !precomputedJoltShapes.empty()) {
        auto it = precomputedJoltShapes.find(node->name);
        PrecomputedJoltShape shape = it != precomputedJoltShapes.end() ? it->second : PrecomputedJoltShape{};
        if (shape.blob) {
            physicsCreateFromBlob(scene, entity->id,
                                  shape.shapeTag, shape.motionTag,
                                  shape.mass, shape.friction, shape.restitution,
                                  shape.blob, shape.blobSize,
                                  pos, rot, worldScale);
            return;
        }
    }

    // Fallback: build from raw vertex/index data
    if (node->mesh->primitives_count == 0) return;

    cgltf_primitive* gltfPrimitive        = &node->mesh->primitives[0];
    const cgltf_accessor* indicesAccessor = gltfPrimitive->indices;
    if (!indicesAccessor) return;

    u32 indexCount = (u32)indicesAccessor->count;
    std::vector<u32> indices(indexCount);
    cgltf_accessor_unpack_indices(indicesAccessor, indices.data(), sizeof(u32), indexCount);

    std::vector<float> positions;
    u32 vertexCount  = 0;

    for (u64 j = 0; j < gltfPrimitive->attributes_count; j++) {
        cgltf_attribute* attribute = &gltfPrimitive->attributes[j];
        if (attribute->type == cgltf_attribute_type_position) {
            const cgltf_accessor* posAccessor = attribute->data;
            vertexCount                       = (u32)posAccessor->count;
            positions.resize(vertexCount * 3);
            cgltf_accessor_unpack_floats(posAccessor, positions.data(), vertexCount * 3);
            break;
        }
    }

    if (positions.empty() || vertexCount == 0) {
        return;
    }

    // Apply world scale to vertex positions so the physics mesh matches
    // the visual mesh.  Jolt treats the vertex data as the shape's local
    // geometry and places the body at pos/rot, so scaling must be baked
    // into the vertices.
    if (worldScale != 1.0f) {
        for (u32 v = 0; v < vertexCount * 3; v++) {
            positions[v] *= worldScale;
        }
    }

    physicsCreateMesh(scene, entity->id, positions.data(), vertexCount * 3, indices.data(), indexCount, pos, rot);
}

void parseRigidBody(Scene* scene, cgltf_node* node, Entity* entity) {
    if (!node->extras.data) return;

    Json* extras = jsonParse((char*)node->extras.data);
    if (!extras) return;

    json_t* rbShape = json_object_get(extras, "rigidBodyShape");
    if (!rbShape || !json_is_string(rbShape)) {
        jsonFree(extras);
        return;
    }

    const char* shapeType = json_string_value(rbShape);

    // Read motion type: ACTIVE → dynamic, PASSIVE → static (default)
    bool isDynamic = false;
    json_t* rbType = json_object_get(extras, "rigidBodyType");
    if (rbType && json_is_string(rbType)) {
        isDynamic = utils::strequals(json_string_value(rbType), "ACTIVE");
    }

    // Read physics material properties (defaults match Blender's)
    float mass        = 1.0f;
    float friction    = 0.5f;
    float restitution = 0.0f;

    json_t* jMass = json_object_get(extras, "rigidBodyMass");
    if (jMass && json_is_number(jMass)) mass = (float)json_number_value(jMass);

    json_t* jFric = json_object_get(extras, "rigidBodyFriction");
    if (jFric && json_is_number(jFric)) friction = (float)json_number_value(jFric);

    json_t* jRest = json_object_get(extras, "rigidBodyRestitution");
    if (jRest && json_is_number(jRest)) restitution = (float)json_number_value(jRest);

    // Compute world transform by walking the glTF parent chain.
    // At parse time the ECS WorldTransform hasn't been computed yet.
    float pos[3], rot[4], worldScale;
    cgltfNodeWorldTransform(node, pos, rot, &worldScale);

    // For primitive shapes (SPHERE, BOX, CAPSULE, CYLINDER), derive
    // dimensions from the mesh AABB.  For CONVEX_HULL and MESH we need
    // actual vertex/index data from the mesh.
    float aabb[6]     = {};
    std::vector<float> positions;
    u32 positionCount = 0;
    std::vector<u32> indices;
    u32 indexCount    = 0;

    // Try getting AABB from the Mesh component first (fastest path).
    // For instanced meshes the current entity may not own the Mesh
    // component, so fall back to the cgltf position accessor min/max.
    Mesh* mesh = getComponent(scene, Mesh, entity->id);
    if (mesh) {
        aabb[0] = mesh->aabbLocal[0][0];
        aabb[1] = mesh->aabbLocal[0][1];
        aabb[2] = mesh->aabbLocal[0][2];
        aabb[3] = mesh->aabbLocal[1][0];
        aabb[4] = mesh->aabbLocal[1][1];
        aabb[5] = mesh->aabbLocal[1][2];
    } else if (node->mesh) {
        // Read AABB from the cgltf position accessor directly
        for (u64 j = 0; j < node->mesh->primitives[0].attributes_count; j++) {
            if (node->mesh->primitives[0].attributes[j].type == cgltf_attribute_type_position) {
                const cgltf_accessor* posAccessor = node->mesh->primitives[0].attributes[j].data;
                aabb[0]                           = posAccessor->min[0];
                aabb[1]                           = posAccessor->min[1];
                aabb[2]                           = posAccessor->min[2];
                aabb[3]                           = posAccessor->max[0];
                aabb[4]                           = posAccessor->max[1];
                aabb[5]                           = posAccessor->max[2];
                break;
            }
        }
    }

    // Apply world scale to AABB so primitive shape dimensions (half-extents,
    // radius, etc.) match the visual mesh.
    if (worldScale != 1.0f) {
        for (i32 i = 0; i < 6; i++) {
            aabb[i] *= worldScale;
        }
    }

    // For MESH shapes, try using a pre-baked Jolt blob first.
    // Only valid when scale == 1.0 — the blob was baked with unscaled vertices.
    // Try pre-baked Jolt blob first for all shape types.
    if (node->name && !precomputedJoltShapes.empty()) {
        auto it = precomputedJoltShapes.find(node->name);
        PrecomputedJoltShape shape = it != precomputedJoltShapes.end() ? it->second : PrecomputedJoltShape{};
        if (shape.blob) {
            physicsCreateFromBlob(scene, entity->id,
                                  shape.shapeTag, shape.motionTag,
                                  shape.mass, shape.friction, shape.restitution,
                                  shape.blob, shape.blobSize,
                                  pos, rot, worldScale);
            jsonFree(extras);
            return;
        }
    }

    // Fallback: build shape at runtime from glTF data
    bool needsGeometry = utils::strequals(shapeType, "CONVEX_HULL") || utils::strequals(shapeType, "MESH");
    if (needsGeometry && node->mesh && node->mesh->primitives_count > 0) {
        cgltf_primitive* prim = &node->mesh->primitives[0];

        if (prim->indices) {
            indexCount = (u32)prim->indices->count;
            indices.resize(indexCount);
            cgltf_accessor_unpack_indices(prim->indices, indices.data(), sizeof(u32), indexCount);
        }

        for (u64 j = 0; j < prim->attributes_count; j++) {
            if (prim->attributes[j].type == cgltf_attribute_type_position) {
                const cgltf_accessor* posAccessor = prim->attributes[j].data;
                u32 vertexCount                   = (u32)posAccessor->count;
                positionCount                     = vertexCount * 3;
                positions.resize(positionCount);
                cgltf_accessor_unpack_floats(posAccessor, positions.data(), positionCount);
                break;
            }
        }

        // Apply world scale to vertex positions so the physics geometry
        // matches the visual mesh.
        if (worldScale != 1.0f && !positions.empty()) {
            for (u32 v = 0; v < positionCount; v++) {
                positions[v] *= worldScale;
            }
        }
    }

    physicsCreateRigidBody(scene,
                           entity->id,
                           shapeType,
                           isDynamic,
                           mass,
                           friction,
                           restitution,
                           aabb,
                           positions.empty() ? nullptr : positions.data(),
                           positionCount,
                           indices.empty() ? nullptr : indices.data(),
                           indexCount,
                           pos,
                           rot);

    jsonFree(extras);
}

void parseSkin(Scene* scene, cgltf_node* node, Entity* entity) {
    if (!node->skin) {
        return;
    }

    cgltf_skin* cgltfSkin = node->skin;
    Skin* skin            = createComponentT(scene, Skin, entity->id);

    // Copy joints - map from cgltf_node pointers to entity IDs
    for (u64 i = 0; i < cgltfSkin->joints_count; i++) {
        cgltf_node* jointNode = cgltfSkin->joints[i];
        uintptr_t     jointKey = (uintptr_t)jointNode;
        auto it = nodeCache.find(jointKey);
        Entity* jointEntity   = it != nodeCache.end() ? it->second : nullptr;
        if (jointEntity) {
            skin->joints.push_back(jointEntity->id);
        }
    }

    // Copy inverse bind matrices
    if (cgltfSkin->inverse_bind_matrices) {
        cgltf_accessor* accessor = cgltfSkin->inverse_bind_matrices;
        skin->inverseBindMatrices.resize(accessor->count * 16);
        cgltf_accessor_unpack_floats(accessor,
                                     skin->inverseBindMatrices.data(),
                                     accessor->count * 16);
    }

    // Initialize jointMatrices array to the same size as joints
    // These will be filled later during animation/animation update
    skin->jointTransforms.resize(static_cast<i32>(skin->joints.size()));
    skin->jointBufferCursor = 0;

    // Compensate for the parent armature's base rotation and scale.
    //
    // In a typical Mixamo/Blender export the armature node carries a
    // 90-degree X rotation (Z-up → Y-up conversion) and a cm→m scale
    // (e.g. 0.01).  The vertex positions are already stored in metres
    // and Y-up, so applying the armature's full transform through the
    // parent chain makes the mesh 100x too small and incorrectly
    // oriented.
    //
    // Set the mesh entity's local transform to cancel the parent's
    // base rotation and scale.  At runtime the armature rotation is
    //   finalRot = yaw * baseRot
    // so the WorldTransform computation becomes
    //   worldRot = (yaw * baseRot) * inv(baseRot) = yaw
    //   worldScale = parentScale * (1/parentScale) = 1.0
    //   worldPos = parentPos                         (local pos is 0)
    // giving a correctly sized, correctly oriented character.
    //
    // Note: full GPU skinning is still needed for per-bone animation;
    // this only ensures the bind-pose mesh is visible.
    if (entity->parent) {
        Transform* parentTransform = getComponent(scene, Transform, entity->parent->id);
        Transform* transform       = getComponent(scene, Transform, entity->id);
        if (parentTransform && transform) {
            glm_quat_inv(parentTransform->rot, transform->rot);
            transform->pos[3] = 1.0f / parentTransform->pos[3];
        }
    }
}

u32 parseMaterial(cgltf_primitive* cgltfPrimitive) {
    cgltf_material* cgltfMaterial = cgltfPrimitive->material;

    const char* nameCheck = "default";
    if (cgltfMaterial && cgltfMaterial->name) {
        nameCheck = cgltfMaterial->name;
    }

    Material* existingMaterial = getMaterialByName(nameCheck);
    if (existingMaterial) {
        return existingMaterial->id;
    }

    Material* material = createMaterial(nameCheck);

    // Set defaults
    material->baseColor[0] = material->baseColor[1] = material->baseColor[2] =
        material->baseColor[3]                      = 1.0f;
    material->emissive[0] = material->emissive[1] = material->emissive[2] = material->emissive[3] =
        0.0f;
    material->rmas[0] = material->rmas[2] = material->rmas[3] = 1.0f;
    material->rmas[1]                                         = 0.0f;

    // Parse PBR properties
    if (cgltfMaterial->has_pbr_metallic_roughness) {
        material->baseColor[0] = cgltfMaterial->pbr_metallic_roughness.base_color_factor[0];
        material->baseColor[1] = cgltfMaterial->pbr_metallic_roughness.base_color_factor[1];
        material->baseColor[2] = cgltfMaterial->pbr_metallic_roughness.base_color_factor[2];
        material->baseColor[3] = cgltfMaterial->pbr_metallic_roughness.base_color_factor[3];

        material->rmas[0] = cgltfMaterial->pbr_metallic_roughness.roughness_factor;
        material->rmas[1] = cgltfMaterial->pbr_metallic_roughness.metallic_factor;
    }

    // Parse emissive
    if (cgltfMaterial->emissive_factor[0] != 0.0f || cgltfMaterial->emissive_factor[1] != 0.0f ||
        cgltfMaterial->emissive_factor[2] != 0.0f) {
        material->emissive[0] = cgltfMaterial->emissive_factor[0];
        material->emissive[1] = cgltfMaterial->emissive_factor[1];
        material->emissive[2] = cgltfMaterial->emissive_factor[2];
        material->emissive[3] = 1.0f;
        material->featureMask |= (1u << MAT_HAS_EMISSIVE_FACTOR);
    }

    // Parse emissive strength (KHR_materials_emissive_strength)
    if (cgltfMaterial->has_emissive_strength) {
        material->emissive[3] = cgltfMaterial->emissive_strength.emissive_strength;
    }

    // Parse alpha mode
    if (cgltfMaterial->alpha_mode == cgltf_alpha_mode_mask) {
        material->featureMask |= (1u << MAT_ALPHA_MASK);
        material->rmas[2] = cgltfMaterial->alpha_cutoff;
    } else if (cgltfMaterial->alpha_mode == cgltf_alpha_mode_blend) {
        material->featureMask |= (1u << MAT_ALPHA_BLEND);
    }
    if (cgltfMaterial->alpha_mode == cgltf_alpha_mode_opaque) {
        material->featureMask |= (1u << MAT_ALPHA_OPAQUE);
    }

    // Parse double sided
    if (cgltfMaterial->double_sided) {
        material->featureMask |= (1u << MAT_IS_DOUBLE_SIDED);
    }

    // Parse unlit (KHR_materials_unlit)
    if (cgltfMaterial->unlit) {
        material->featureMask |= (1u << MAT_IS_DOUBLE_SIDED);
    }

    // Parse Clearcoat (KHR_materials_clearcoat)
    if (cgltfMaterial->has_clearcoat) {
        material->clearcoatSheenTransmission[0] = cgltfMaterial->clearcoat.clearcoat_factor;
        material->clearcoatSheenTransmission[1] =
            cgltfMaterial->clearcoat.clearcoat_roughness_factor;
        material->featureMask |= (1u << MAT_HAS_CLEARCOAT);
    }

    // Parse Transmission (KHR_materials_transmission)
    if (cgltfMaterial->has_transmission) {
        material->clearcoatSheenTransmission[3] = cgltfMaterial->transmission.transmission_factor;
        material->featureMask |= (1u << MAT_HAS_TRANSMISSION);
    }

    // Parse Sheen (KHR_materials_sheen)
    if (cgltfMaterial->has_sheen) {
        material->sheenColorAnisotropy[0]       = cgltfMaterial->sheen.sheen_color_factor[0];
        material->sheenColorAnisotropy[1]       = cgltfMaterial->sheen.sheen_color_factor[1];
        material->sheenColorAnisotropy[2]       = cgltfMaterial->sheen.sheen_color_factor[2];
        material->clearcoatSheenTransmission[2] = cgltfMaterial->sheen.sheen_roughness_factor;
        material->featureMask |= (1u << MAT_HAS_SHEEN);
    }

    // Parse Specular (KHR_materials_specular)
    if (cgltfMaterial->has_specular) {
        material->specularAnisotropyRotation[0] = cgltfMaterial->specular.specular_factor;
        material->specularColorIor[0]           = cgltfMaterial->specular.specular_color_factor[0];
        material->specularColorIor[1]           = cgltfMaterial->specular.specular_color_factor[1];
        material->specularColorIor[2]           = cgltfMaterial->specular.specular_color_factor[2];
        material->featureMask |= (1u << MAT_HAS_SPECULAR);
    }

    // Parse IOR (KHR_materials_ior)
    if (cgltfMaterial->has_ior) {
        material->specularColorIor[3] = cgltfMaterial->ior.ior;
        material->featureMask |= (1u << MAT_HAS_IOR);
    }

    // Parse Volume (KHR_materials_volume)
    if (cgltfMaterial->has_volume) {
        material->volumeFactors[0] = cgltfMaterial->volume.thickness_factor;
        material->volumeFactors[1] = cgltfMaterial->volume.attenuation_distance;
        material->volumeColor[0]   = cgltfMaterial->volume.attenuation_color[0];
        material->volumeColor[1]   = cgltfMaterial->volume.attenuation_color[1];
        material->volumeColor[2]   = cgltfMaterial->volume.attenuation_color[2];
        material->featureMask |= (1u << MAT_HAS_VOLUME);
    }

    // Parse Anisotropy (KHR_materials_anisotropy)
    if (cgltfMaterial->has_anisotropy) {
        material->sheenColorAnisotropy[3]       = cgltfMaterial->anisotropy.anisotropy_strength;
        material->specularAnisotropyRotation[1] = cgltfMaterial->anisotropy.anisotropy_rotation;
        material->featureMask |= (1u << MAT_HAS_ANISOTROPY);
    }

    // Textures - Set default texture transforms (identity: offset=0,0
    // scale=1,1)
    material->baseColorOffsetScale[0] = material->baseColorOffsetScale[1] = 0.0f;
    material->baseColorOffsetScale[2] = material->baseColorOffsetScale[3] = 1.0f;
    material->rmaOffsetScale[0] = material->rmaOffsetScale[1] = 0.0f;
    material->rmaOffsetScale[2] = material->rmaOffsetScale[3] = 1.0f;
    material->normalOffsetScale[0] = material->normalOffsetScale[1] = 0.0f;
    material->normalOffsetScale[2] = material->normalOffsetScale[3] = 1.0f;
    material->emissionOffsetScale[0] = material->emissionOffsetScale[1] = 0.0f;
    material->emissionOffsetScale[2] = material->emissionOffsetScale[3] = 1.0f;
    material->occlusionOffsetScale[0] = material->occlusionOffsetScale[1] = 0.0f;
    material->occlusionOffsetScale[2] = material->occlusionOffsetScale[3] = 1.0f;

    if (cgltfMaterial->has_pbr_metallic_roughness &&
        cgltfMaterial->pbr_metallic_roughness.base_color_texture.texture) {
        cgltf_texture* cgltfTexture =
            cgltfMaterial->pbr_metallic_roughness.base_color_texture.texture;
        cgltf_image* image = cgltfTexture->image;
        if (!image) image = cgltfTexture->basisu_image;
        if (!image) { utils::warn("sceneParser: base color texture has no image for material '%s'", nameCheck); goto after_base_color; }

        Texture* texture = getTextureByName(image->name);
        if (texture) {
            texture->refCount++;
        } else {
            const u8* src = cgltf_buffer_view_data(image->buffer_view);
            texture       = createTextureFromData(image->name,
                                                  src,
                                                  image->buffer_view->size,
                                                  image->mime_type,
                                                  0);
        }

        material->colorTextureSampler = mapGltfSampler(cgltfTexture->sampler);
        material->colorTexture        = texture->id;

        material->featureMask |= (1u << MAT_HAS_TEXTURE_COLOR);
        if (cgltfMaterial->pbr_metallic_roughness.base_color_texture.has_transform) {
            material->baseColorOffsetScale[0] =
                cgltfMaterial->pbr_metallic_roughness.base_color_texture.transform.offset[0];
            material->baseColorOffsetScale[1] =
                cgltfMaterial->pbr_metallic_roughness.base_color_texture.transform.offset[1];
            material->baseColorOffsetScale[2] =
                cgltfMaterial->pbr_metallic_roughness.base_color_texture.transform.scale[0];
            material->baseColorOffsetScale[3] =
                cgltfMaterial->pbr_metallic_roughness.base_color_texture.transform.scale[1];
        }
    }
    after_base_color:
    if (cgltfMaterial->has_pbr_metallic_roughness &&
        cgltfMaterial->pbr_metallic_roughness.metallic_roughness_texture.texture) {
        cgltf_texture* cgltfTexture =
            cgltfMaterial->pbr_metallic_roughness.metallic_roughness_texture.texture;
        cgltf_image* image = cgltfTexture->image;
        if (!image) image = cgltfTexture->basisu_image;
        if (!image) { utils::warn("sceneParser: metallic roughness texture has no image for material '%s'", nameCheck); goto after_roughness; }
        // debug("parse rough image: %s", image->name);

        Texture* texture = getTextureByName(image->name);
        if (texture) {
            texture->refCount++;
        } else {
            const u8* src = cgltf_buffer_view_data(image->buffer_view);
            texture       = createTextureFromData(image->name,
                                                  src,
                                                  image->buffer_view->size,
                                                  image->mime_type,
                                                  1);
        }

        material->rmTextureSampler = mapGltfSampler(cgltfTexture->sampler);
        material->rmTexture        = texture->id;

        material->featureMask |= (1u << MAT_HAS_TEXTURE_ROUGHNESS_METALLIC);
        if (cgltfMaterial->pbr_metallic_roughness.metallic_roughness_texture.has_transform) {
            material->rmaOffsetScale[0] = cgltfMaterial->pbr_metallic_roughness
                                              .metallic_roughness_texture.transform.offset[0];
            material->rmaOffsetScale[1] = cgltfMaterial->pbr_metallic_roughness
                                              .metallic_roughness_texture.transform.offset[1];
            material->rmaOffsetScale[2] =
                cgltfMaterial->pbr_metallic_roughness.metallic_roughness_texture.transform.scale[0];
            material->rmaOffsetScale[3] =
                cgltfMaterial->pbr_metallic_roughness.metallic_roughness_texture.transform.scale[1];
        }
    }
    after_roughness:
    if (cgltfMaterial->normal_texture.texture) {
        cgltf_texture* cgltfTexture = cgltfMaterial->normal_texture.texture;
        cgltf_image* image          = cgltfTexture->image;
        if (!image) image = cgltfTexture->basisu_image;
        if (!image) { utils::warn("sceneParser: normal texture has no image for material '%s'", nameCheck); goto after_normal; }
        // debug("parse normal image: %s", image->name);

        Texture* texture = getTextureByName(image->name);
        if (texture) {
            texture->refCount++;
        } else {
            const u8* src = cgltf_buffer_view_data(image->buffer_view);
            texture       = createTextureFromData(image->name,
                                                  src,
                                                  image->buffer_view->size,
                                                  image->mime_type,
                                                  1);
        }

        material->normalTextureSampler = mapGltfSampler(cgltfTexture->sampler);
        material->normalTexture        = texture->id;

        material->rmas[3] = cgltfMaterial->normal_texture.scale;
        material->featureMask |= (1u << MAT_HAS_TEXTURE_NORMAL);
        if (cgltfMaterial->normal_texture.has_transform) {
            material->normalOffsetScale[0] = cgltfMaterial->normal_texture.transform.offset[0];
            material->normalOffsetScale[1] = cgltfMaterial->normal_texture.transform.offset[1];
            material->normalOffsetScale[2] = cgltfMaterial->normal_texture.transform.scale[0];
            material->normalOffsetScale[3] = cgltfMaterial->normal_texture.transform.scale[1];
        }
    }
    after_normal:
    if (cgltfMaterial->emissive_texture.texture) {
        cgltf_texture* cgltfTexture = cgltfMaterial->emissive_texture.texture;
        cgltf_image* image          = cgltfTexture->image;
        if (!image) image = cgltfTexture->basisu_image;
        if (!image) { utils::warn("sceneParser: emissive texture has no image for material '%s'", nameCheck); goto after_emissive; }
        // debug("parse emissive image: %s", image->name);

        Texture* texture = getTextureByName(image->name);
        if (texture) {
            texture->refCount++;
        } else {
            const u8* src = cgltf_buffer_view_data(image->buffer_view);
            texture       = createTextureFromData(image->name,
                                                  src,
                                                  image->buffer_view->size,
                                                  image->mime_type,
                                                  0);
        }

        material->emissiveTextureSampler = mapGltfSampler(cgltfTexture->sampler);
        material->emissiveTexture        = texture->id;

        material->featureMask |= (1u << MAT_HAS_TEXTURE_EMISSIVE);
        if (cgltfMaterial->emissive_texture.has_transform) {
            material->emissionOffsetScale[0] = cgltfMaterial->emissive_texture.transform.offset[0];
            material->emissionOffsetScale[1] = cgltfMaterial->emissive_texture.transform.offset[1];
            material->emissionOffsetScale[2] = cgltfMaterial->emissive_texture.transform.scale[0];
            material->emissionOffsetScale[3] = cgltfMaterial->emissive_texture.transform.scale[1];
        }
    }
    after_emissive:
    if (cgltfMaterial->occlusion_texture.texture) {
        cgltf_texture* cgltfTexture = cgltfMaterial->occlusion_texture.texture;
        cgltf_image* image          = cgltfTexture->image;
        if (!image) image = cgltfTexture->basisu_image;
        if (!image) { utils::warn("sceneParser: occlusion texture has no image for material '%s'", nameCheck); goto after_occlusion; }
        // debug("parse occ image: %s", image->name);

        Texture* texture = getTextureByName(image->name);
        if (texture) {
            texture->refCount++;
        } else {
            const u8* src = cgltf_buffer_view_data(image->buffer_view);
            texture       = createTextureFromData(image->name,
                                                  src,
                                                  image->buffer_view->size,
                                                  image->mime_type,
                                                  1);
        }

        material->occlusionTextureSampler = mapGltfSampler(cgltfTexture->sampler);
        material->occlusionTexture        = texture->id;

        material->featureMask |= (1u << MAT_HAS_TEXTURE_OCCLUSION);
        if (cgltfMaterial->occlusion_texture.has_transform) {
            material->occlusionOffsetScale[0] =
                cgltfMaterial->occlusion_texture.transform.offset[0];
            material->occlusionOffsetScale[1] =
                cgltfMaterial->occlusion_texture.transform.offset[1];
            material->occlusionOffsetScale[2] = cgltfMaterial->occlusion_texture.transform.scale[0];
            material->occlusionOffsetScale[3] = cgltfMaterial->occlusion_texture.transform.scale[1];
        }
    }
    after_occlusion:

    // Parse splatmap textures using nodeSplatInfoMap (for terrain)
    // Weight textures are UDIM tiles loaded into TerrainData (SceneBuffer).
    // Detail textures (albedo/normal per channel) stay per-material.
    {
        Json* fullSplatInfo = nullptr;
        if (static_cast<i32>(nodeSplatInfoMap.size()) > 0) {
            // Try matching the material name against splatInfo keys first
            auto it = nodeSplatInfoMap.find(nameCheck);
            fullSplatInfo = it != nodeSplatInfoMap.end() ? it->second : nullptr;
            // If not found, use the first available entry (splatInfo is shared)
            if (!fullSplatInfo) fullSplatInfo = nodeSplatInfoMap.begin()->second;
        }
        if (fullSplatInfo && json_is_object(fullSplatInfo)) {
            const char* groupKey;
            json_t* groupValue;
            u32 groupIndex = 0;

            // Derive UDIM tile directory from model path
            utils::String splatBaseDir = {};
            if (currentModelPath) {
                const char* lastSlash = strrchr(currentModelPath, '/');
                const char* filename  = lastSlash ? lastSlash + 1 : currentModelPath;
                const char* firstDot  = strchr(filename, '.');
                u32 stemLen = firstDot ? (u32)(firstDot - filename) : (u32)strlen(filename);
                char stem[256];
                memcpy(stem, filename, stemLen);
                stem[stemLen] = '\0';
                if (lastSlash) {
                    u32 dirLen = (u32)(lastSlash - currentModelPath + 1);
                    utils::stringAppendBinary(&splatBaseDir, const_cast<char*>(currentModelPath), dirLen);
                }
                utils::stringAppend(&splatBaseDir, stem);
                utils::stringAppend(&splatBaseDir, "/");
            }

            json_object_foreach(fullSplatInfo, groupKey, groupValue) {
                if (groupIndex >= MAX_SPLAT_GROUPS) break;
                if (!json_is_object(groupValue)) continue;

                // Load UDIM weight tiles (scan for actual files with arbitrary UDIM numbers)
                if (splatBaseDir.size > 0) {
                    sceneLoadSplatTiles(splatBaseDir.data, groupKey, groupIndex);
                }

                // Load detail textures for each channel (R, G, B, A)
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

                    // Load albedo: images/terrain/<detailName>/albedo.ktx2
                    utils::String albedoPath = {};
                    utils::stringPrintf(&albedoPath, "images/terrain/%s/albedo.ktx2", detailName);
                    Texture* albedoTex = getTextureByName(albedoPath.data);
                    if (albedoTex) {
                        albedoTex->refCount++;
                        material->splatAlbedoTextures[detailIdx] = albedoTex->id;
                    } else {
                        material->splatAlbedoTextures[detailIdx] = 0;
                    }
                    utils::stringDestroy(&albedoPath);

                    // Load normal: images/terrain/<detailName>/normal.ktx2
                    utils::String normalPath = {};
                    utils::stringPrintf(&normalPath, "images/terrain/%s/normal.ktx2", detailName);
                    Texture* normalTex = getTextureByName(normalPath.data);
                    if (normalTex) {
                        normalTex->refCount++;
                        material->splatNormalTextures[detailIdx] = normalTex->id;
                    } else {
                        material->splatNormalTextures[detailIdx] = 0;
                    }
                    utils::stringDestroy(&normalPath);
                }

                groupIndex++;
            }

            if (groupIndex > 0) {
                material->splatGroupCount = groupIndex;
                material->featureMask |= (1u << MAT_HAS_SPLATMAP);
                vulkanResourceSetTerrainSplatGroupCount(groupIndex);
                utils::info("sceneParser: material '%s' has %u splat groups (UDIM)",
                     nameCheck, groupIndex);
            }

            utils::stringDestroy(&splatBaseDir);
        }
    }

    rendererUploadMaterial(material);

    return material->id;
}

void parseLight(Scene* scene, cgltf_node* node, Entity* entity) {
    cgltf_light* gltfLight = node->light;
    if (!gltfLight) {
        return;
    }

    Light* light = createComponent(scene, Light, entity->id);
    glm_vec3_copy(gltfLight->color, light->color);
    /* GLB/glTF stores intensity in candela (cd = lm/sr) for point/spot lights
     * when exported with Blender's SPEC lighting mode (the default).
     * Convert to radiometric intensity (W/sr) by dividing by the luminous
     * efficacy K = 683 lm/W.  The shader then computes irradiance as
     * intensity / r², matching Blender's point-light formula. */
    light->intensity = gltfLight->intensity / 683.0f;
    light->range     = gltfLight->range;

    switch (gltfLight->type) {
        case cgltf_light_type_directional:
            light->lightType = LIGHT_DIRECTIONAL;
            break;
        case cgltf_light_type_point:
            light->lightType = LIGHT_POINT;
            break;
        case cgltf_light_type_spot:
            light->lightType      = LIGHT_SPOT;
            light->innerConeAngle = gltfLight->spot_inner_cone_angle;
            light->outerConeAngle = gltfLight->spot_outer_cone_angle;
            break;
        default:
            light->lightType = LIGHT_POINT;
            break;
    }

    // Restore light color from node extras (saved by 1-blender-scene.py)
    // because gltfpack strips the KHR_lights_punctual color field when
    // it equals white [1,1,1].
    if (node->extras.data) {
        Json* extras = jsonParse((char*)node->extras.data);
        if (extras) {
            json_t* lc = json_object_get(extras, "lightColor");
            if (lc && json_is_array(lc) && json_array_size(lc) == 3) {
                light->color[0] = (float)json_number_value(json_array_get(lc, 0));
                light->color[1] = (float)json_number_value(json_array_get(lc, 1));
                light->color[2] = (float)json_number_value(json_array_get(lc, 2));
                // debug("sceneParser: restored light color for '%s': (%.2f, %.2f, %.2f)",
                // node->name ? node->name : "(unnamed)",
                // light->color[0], light->color[1], light->color[2]);
            }
            jsonFree(extras);
        }
    }

    /* direction is derived from the node Transform at runtime — no baking */
    lightMarkDirty(scene, entity->id);

    // debug("sceneParser: light '%s' type=%d intensity=%.2f range=%.2f color=(%.2f, %.2f, %.2f)",
    //       node->name ? node->name : "(unnamed)",
    //       light->lightType,
    //       light->intensity,
    //       light->range,
    //       light->color[0], light->color[1], light->color[2]);
}

void parseAnimation(cgltf_animation* cgltfAnim) {
    AnimationClip* clip = animationGetOrCreate(cgltfAnim->name);

    // Find maximum duration
    float maxDuration = 0.0f;
    for (i32 i = 0; i < (i32)cgltfAnim->channels_count; i++) {
        cgltf_animation_channel* cgChannel = &cgltfAnim->channels[i];
        if (cgChannel->sampler->input && cgChannel->sampler->input->count > 0) {
            float lastTime = 0.0f;
            cgltf_accessor_read_float(cgChannel->sampler->input,
                                      cgChannel->sampler->input->count - 1,
                                      &lastTime,
                                      1);
            if (lastTime > maxDuration) {
                maxDuration = lastTime;
            }
        }
    }
    clip->duration = maxDuration;

    // Parse channels
    for (i32 i = 0; i < (i32)cgltfAnim->channels_count; i++) {
        cgltf_animation_channel* cgChannel = &cgltfAnim->channels[i];
        cgltf_animation_sampler* sampler   = cgChannel->sampler;
        cgltf_accessor* inputAccessor      = sampler->input;
        cgltf_accessor* outputAccessor     = sampler->output;

        // Validate accessors
        if (!inputAccessor || !outputAccessor || inputAccessor->count == 0) {
            utils::warn(
                "animationParser: invalid accessors for channel %d in "
                "animation '%s'",
                i,
                clip->name.data);
            continue;
        }

        // Get the target node's bone name
        const char* boneName = cgChannel->target_node->name;
        if (!boneName || !boneName[0]) {
            utils::warn(
                "animationParser: target node has no name for channel %d in "
                "animation '%s'",
                i,
                clip->name.data);
            continue;
        }

        // Find or create animation channel for this bone name
        AnimationChannel* animChannel = nullptr;
        for (size_t j = 0; j < clip->channels.size(); j++) {
            if (utils::strequals(clip->channels[j].jointName.data, boneName)) {
                animChannel = &clip->channels[j];
                break;
            }
        }

        if (!animChannel) {
            AnimationChannel newChannel = {};
            utils::stringPrintf(&newChannel.jointName, boneName);
            clip->channels.push_back(newChannel);
            animChannel = &clip->channels[static_cast<i32>(clip->channels.size()) - 1];
        }

        // Determine keyframe count and component count based on target path
        size_t keyCount                     = inputAccessor->count;
        size_t componentCount               = 0;
        std::vector<Keyframe>* targetArray        = nullptr;
        InterpolationType* interpolationPtr = nullptr;

        switch (cgChannel->target_path) {
            case cgltf_animation_path_type_translation:
                componentCount   = 3;
                targetArray      = &animChannel->positionKeys;
                interpolationPtr = &animChannel->positionInterpolation;
                break;
            case cgltf_animation_path_type_rotation:
                componentCount   = 4;
                targetArray      = &animChannel->rotationKeys;
                interpolationPtr = &animChannel->rotationInterpolation;
                break;
            case cgltf_animation_path_type_scale:
                componentCount   = 3;
                targetArray      = &animChannel->scaleKeys;
                interpolationPtr = &animChannel->scaleInterpolation;
                break;
            default:
                utils::warn("animationParser: unsupported animation path type %d", cgChannel->target_path);
                continue;
        }

        // Store interpolation type
        switch (sampler->interpolation) {
            case cgltf_interpolation_type_linear:
                *interpolationPtr = INTERPOLATION_LINEAR;
                break;
            case cgltf_interpolation_type_step:
                *interpolationPtr = INTERPOLATION_STEP;
                break;
            case cgltf_interpolation_type_cubic_spline:
                *interpolationPtr = INTERPOLATION_CUBIC_SPLINE;
                break;
            default:
                *interpolationPtr = INTERPOLATION_LINEAR;
                break;
        }

        // Allocate keyframes
        (*targetArray).resize(keyCount);

        // Read time values (input)
        std::vector<float> times(keyCount);
        cgltf_accessor_unpack_floats(inputAccessor, times.data(), keyCount);

        // Handle different interpolation types
        bool isCubicSpline      = (sampler->interpolation == cgltf_interpolation_type_cubic_spline);
        size_t outputMultiplier = isCubicSpline ? 3 : 1;
        std::vector<float> values(keyCount * componentCount * outputMultiplier);
        cgltf_accessor_unpack_floats(outputAccessor,
                                     values.data(),
                                     keyCount * componentCount * outputMultiplier);

        // Populate keyframes
        if (isCubicSpline) {
            // Cubic spline: output contains inTangent, value, outTangent
            // for each keyframe
            for (size_t k = 0; k < keyCount; k++) {
                Keyframe* kf = &(*targetArray)[k];
                kf->time     = times[k];

                size_t baseIdx = k * componentCount * 3;

                // inTangent
                for (size_t c = 0; c < componentCount; c++) {
                    kf->inTangent[c] = values[baseIdx + c];
                }
                for (size_t c = componentCount; c < 4; c++) {
                    kf->inTangent[c] = 0.0f;
                }

                // value
                for (size_t c = 0; c < componentCount; c++) {
                    kf->value[c] = values[baseIdx + componentCount + c];
                }
                for (size_t c = componentCount; c < 4; c++) {
                    kf->value[c] = 0.0f;
                }

                // outTangent
                for (size_t c = 0; c < componentCount; c++) {
                    kf->outTangent[c] = values[baseIdx + componentCount * 2 + c];
                }
                for (size_t c = componentCount; c < 4; c++) {
                    kf->outTangent[c] = 0.0f;
                }
            }
        } else {
            // Linear or step: output contains only values
            for (size_t k = 0; k < keyCount; k++) {
                Keyframe* kf = &(*targetArray)[k];
                kf->time     = times[k];

                // Copy component values into vec4
                for (size_t c = 0; c < componentCount; c++) {
                    kf->value[c] = values[k * componentCount + c];
                }
                // Zero out unused components (for vec3 stored in vec4)
                for (size_t c = componentCount; c < 4; c++) {
                    kf->value[c] = 0.0f;
                }
                // Initialize tangents to zero for non-cubic interpolation
                glm_vec4_zero(kf->inTangent);
                glm_vec4_zero(kf->outTangent);
            }
        }
    }

    // Parse events from extras
    if (cgltfAnim->extras.data) {
        char* extrasJson = (char*)cgltfAnim->extras.data;
        parseAnimationEventsFromExtras(clip, extrasJson);
    }

    //     debug(
    //         "animationParser: loaded animation '%s' with %zu channels, "
    //         "duration: %.2fs",
    //         clip->name.data,
    //         static_cast<i32>(clip->channels.size()),
    //         clip->duration);
}

void parseAnimationEventsFromExtras(AnimationClip* clip, const char* extrasJson) {
    // Parse JSON from extras
    // Expected format: {"leftFoot": 14, "rightFoot": 28}
    // Frame numbers are converted to time: time = frame /
    // DEFAULT_FRAME_RATE

    Json* extras = jsonParse(extrasJson);
    if (!extras) {
        utils::warn("animationParser: failed to parse events JSON for '%s': %s",
             clip->name.data,
             extrasJson);
        return;
    }

    const char* key;
    json_t* value;

    json_object_foreach(extras, key, value) {
        if (json_is_integer(value)) {
            json_int_t frame = json_integer_value(value);
            float eventTime  = (float)frame / DEFAULT_ANIMATION_FRAME_RATE;

            AnimationEventDef event = {};
            utils::stringAppend(&event.name, key);
            event.time = eventTime;

            clip->events.push_back(event);

            utils::info(
                "animationParser: added event '%s' at time %.3fs (frame "
                "%lld) to '%s'",
                key,
                eventTime,
                frame,
                clip->name.data);
        }
    }

    jsonFree(extras);
}
}  // namespace engine
