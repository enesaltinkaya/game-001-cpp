#pragma once

#include "container/SparseSet/SparseSet.h"
#include "json/Json.h"
#include "container/Map.h"

struct Entity {
    u32 id;
    struct Scene* scene;
    struct Entity* parent;
    Array(struct Entity*) children;
    const char* name; // optional, nullptr if unnamed. owned copy.
};

struct Scene {
    void* backendScene;
    String name;  // also stores source path (e.g. "models/terrain/foo.dat") for sidecar loading
    Map(u32, Json*) extras;

    Array(SparseSet*) components;

    // for adding new entities
    u32 entityCounter;
    Array(u32) entityFreeList;

    // all entities in this scene (stable heap pointers)
    Array(Entity*) entities;
    Map(u32, Entity*) entityMap; // id -> Entity* for O(1) lookup

    // for transform updates
    Map(u32, double) activeEntities;
    Array(u32) activeEntityRemoveList;


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

