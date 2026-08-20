#pragma once

#include "container/SparseSet/SparseSet.h"
#include "json/Json.h"
#include <unordered_map>

namespace engine {
struct Entity {
    u32 id = 0;
    struct Scene* scene = nullptr;
    struct Entity* parent = nullptr;
    std::vector<struct Entity*> children = {};
    const char* name = nullptr; // optional, nullptr if unnamed. owned copy.
};

struct Scene {
    void* backendScene;
    utils::String name;  // also stores source path (e.g. "models/terrain/foo.dat") for sidecar loading
    std::unordered_map<u32, Json*> extras;

    std::vector<utils::SparseSet*> components;

    // for adding new entities
    u32 entityCounter;
    std::vector<u32> entityFreeList;

    // all entities in this scene (stable heap pointers)
    std::vector<Entity*> entities;
    std::unordered_map<u32, Entity*> entityMap; // id -> Entity* for O(1) lookup

    // for transform updates
    std::unordered_map<u32, double> activeEntities;
    std::vector<u32> activeEntityRemoveList;


    vec3 aabbMin;
    vec3 aabbMax;
    bool hasBounds;
    bool alwaysVisible; // skip scene-level frustum culling entirely
    bool visible;
    bool ready; // true once fully loaded and added to ecs.scenes

    // Async-load lifecycle (set by sceneLoadCb, handled by sceneLoadMainThread
    // and sceneDestroy — all on the main thread, so no synchronization is
    // needed).  asyncLoadPending is true from sceneLoadCb() until the worker's
    // completion task has run.  If sceneDestroy() is called while a load is
    // still in flight it cannot free the scene (the worker thread is still
    // parsing into it), so it sets destroyRequested and defers the free to
    // the completion task, which discards the scene instead of registering it.
    bool asyncLoadPending;
    bool destroyRequested;

    // Optional callback invoked on the main thread once the scene is ready.
    void (*loadCallback)(struct Scene* scene, void* userData);
    void* loadCallbackUserData;
};

}  // namespace engine
