#include "SceneSystem.h"
#include "ecs/Ecs.h"
#include "ecs/components/Skin.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/mesh/MeshComponent.h"
#include "renderer/Renderer.h"
#include "thread/Thread.h"

static int ComponentTypeCounter = 0;
static Array(Scene*) visibleScenes;

static bool aabbOutsideFrustum(const vec3 min, const vec3 max, vec4* planes) {
    for (i32 i = 0; i < 6; i++) {
        vec4 p        = {planes[i][0], planes[i][1], planes[i][2], planes[i][3]};
        vec3 positive = {
            p[0] >= 0.0f ? max[0] : min[0],
            p[1] >= 0.0f ? max[1] : min[1],
            p[2] >= 0.0f ? max[2] : min[2],
        };
        if ((p[0] * positive[0]) + (p[1] * positive[1]) + (p[2] * positive[2]) + p[3] < 0.0f) {
            return true;
        }
    }
    return false;
}

static void sceneSystemAdded(void) {}

static void sceneSystemRemoved(void) {
    arrayFree(visibleScenes);
}

static void sceneSystemPostUpdate(void) {
    arrayClear(visibleScenes);

    Entity* cameraEntity = cameraGetEntity();
    Camera* camera      = cameraEntity ? getComponent(cameraEntity->scene, Camera, cameraEntity->id)
                                       : nullptr;
    vec4* frustumPlanes = camera ? (vec4*)camera->cameraUbo.frustumPlanes : nullptr;

    int culledCount = 0;
    foreach (Scene* scene, ecs.scenes) {
        /* Scene bounds are currently static and computed once by SceneParser.
         * If a scene later needs dynamic or effectively unbounded contents,
         * revisit that policy instead of rebuilding bounds here every frame. */
        bool visible = true;
        if (!scene->alwaysVisible && frustumPlanes && scene->hasBounds) {
            visible = !aabbOutsideFrustum(scene->aabbMin, scene->aabbMax, frustumPlanes);
        }

        scene->visible = visible;
        if (visible) {
            arrayPut(visibleScenes, scene);
        } else {
            culledCount++;
        }
    }

    rendererSetVisibleScenes(visibleScenes, static_cast<u32>(arraySize(visibleScenes)));
}

struct System sceneSystem = {
    .name       = "sceneSystem",
    .added      = sceneSystemAdded,
    .removed    = sceneSystemRemoved,
    .postUpdate = sceneSystemPostUpdate,
};

void* F_sceneCreateComponent(Scene* scene, u32 entity, u64* typeIdPtr, u64 size) {
    THREAD_LOCK;
    if (*typeIdPtr == 0) *typeIdPtr = ++ComponentTypeCounter;
    THREAD_UNLOCK;

    u64 id = *typeIdPtr;
    if (id >= arraySize(scene->components)) {
        arraySetSizeZeroed(scene->components, id + 1);
    }

    if (!scene->components[id]) {
        scene->components[id] = ssNew(size);
    }

    return ssNewItem(scene->components[id], entity);
}

void* F_sceneAddComponent(Scene* scene, u32 entity, u64* typeIdPtr, u64 size, void* data) {
    void* component = F_sceneCreateComponent(scene, entity, typeIdPtr, size);
    memcpy(component, data, size);
    return component;
}

void* F_sceneGetComponent(Scene* scene, u32 entity, u64* typeIdPtr) {
    if (*typeIdPtr == 0) return nullptr;
    u64 id = *typeIdPtr;
    if (id >= arraySize(scene->components)) return nullptr;
    if (scene->components[id] == nullptr) return nullptr;
    return ssGetDataByValue(scene->components[id], entity);
}

SparseSet* F_sceneGetComponents(Scene* scene, u64* typeIdPtr) {
    if (*typeIdPtr == 0) return nullptr;
    u64 id = *typeIdPtr;
    if (id >= arraySize(scene->components)) return nullptr;
    if (scene->components[id] == nullptr) return nullptr;
    return scene->components[id];
}

void F_sceneRemoveComponent(Scene* scene, u32 entity, u64* typeIdPtr) {
    if (*typeIdPtr == 0) return;
    int id = *typeIdPtr;
    if (scene->components[id] == nullptr) return;

    ssRemoveByValue(scene->components[id], entity);
}

static u32 globalEntityCounter = 0;

Entity* createEntity(Scene* scene, const char* name) {
    u32 newEntityId = 0;
    if (arraySize(scene->entityFreeList)) {
        newEntityId = arrayPop(scene->entityFreeList);
    } else {
        globalEntityCounter++;
        newEntityId = globalEntityCounter;
    }

    Entity* entity = static_cast<Entity*>(memoryAlloc(sizeof(Entity)));
    *entity        = Entity{
        .id       = newEntityId,
        .scene    = scene,
        .parent   = nullptr,
        .children = nullptr,
        .name     = nullptr,
    };

    if (name && name[0]) {
        size_t len = strlen(name);
        char* copy = static_cast<char*>(memoryAlloc(len + 1));
        memcpy(copy, name, len + 1);
        entity->name = copy;
    }

    arrayPut(scene->entities, entity);
    mapPut(scene->entityMap, newEntityId, entity);
    return entity;
}

Entity* getEntity(Scene* scene, u32 entityId) {
    return mapGet(scene->entityMap, entityId);
}

Entity* sceneFindEntity(Scene* scene, const char* name) {
    foreach (Entity* e, scene->entities) {
        if (e->name && strequals(e->name, name)) return e;
    }
    return nullptr;
}

Entity* entityFindDescendant(Entity* root, const char* name) {
    foreach (Entity* child, root->children) {
        if (child->name && strequals(child->name, name)) return child;
        Entity* found = entityFindDescendant(child, name);
        if (found) return found;
    }
    return nullptr;
}

void destroyEntity(Entity* entity) {
    Scene* scene = entity->scene;
    foreach (struct SparseSet* item, scene->components) {
        if (item && ssContainsValue(item, entity->id)) {
            ssRemoveByValue(item, entity->id);
        }
    }

    if (entity->name) {
        memoryFree(const_cast<char*>(entity->name));
    }

    if (entity->parent) {
        for (i32 i = 0, si = arraySize(entity->parent->children); i < si; i++) {
            if (entity->parent->children[i] == entity) {
                arrayDeleteSlow(entity->parent->children, i);
                break;
            }
        }
    }

    arrayFree(entity->children);
    mapRemove(scene->entityMap, entity->id);
    arrayPut(scene->entityFreeList, entity->id);

    for (i32 i = 0, si = arraySize(scene->entities); i < si; i++) {
        if (scene->entities[i] == entity) {
            arrayDeleteSlow(scene->entities, i);
            break;
        }
    }

    memoryFree(entity);
}

void sceneDestroy(Scene* scene) {
    if (!scene) return;
    // An async scene load may still be in flight: the worker thread is still
    // parsing into this scene (or its completion task is queued).  Freeing it
    // now would be a use-after-free, so hand the free over to the completion
    // task (sceneLoadMainThread), which discards the scene instead of
    // registering it.  Both the deferral and the completion task run on the
    // main thread, so this check cannot race with load completion.
    if (scene->asyncLoadPending) {
        scene->destroyRequested = 1;
        warn("sceneSystem: scene destroy deferred until the in-flight async load completes");
        return;
    }
    SparseSet* meshSet = getComponents(scene, Mesh);
    if (meshSet) {
        for (u32 i = 0; i < meshSet->size; i++) {
            Mesh* m  = static_cast<Mesh*>(ssGetDataByIndex(meshSet, i));
            for (i32 j = 0, sj = arraySize(m->primitives); j < sj; j++) {
                Primitive* p = &m->primitives[j];
                arrayFree(p->indices);
                arrayFree(p->positions);
                for (i32 k = 0; k < cgltf_attribute_type_max_enum; k++) {
                    arrayFree(p->attributes[k]);
                }
            }
            arrayFree(m->primitives);
            arrayFree(m->instances);
        }
    }

    SparseSet* skinSet = getComponents(scene, Skin);
    if (skinSet) {
        for (u32 i = 0; i < skinSet->size; i++) {
            Skin* s  = static_cast<Skin*>(ssGetDataByIndex(skinSet, i));
            arrayFree(s->joints);
            arrayFree(s->inverseBindMatrices);
            arrayFree(s->jointTransforms);
        }
    }

    foreach (SparseSet* ss, scene->components) {
        if (ss) ssDestroy(ss);
    }
    arrayFree(scene->components);
    arrayFree(scene->entityFreeList);

    foreach (Entity* e, scene->entities) {
        if (e->name) memoryFree(const_cast<char*>(e->name));
        arrayFree(e->children);
        memoryFree(e);
    }
    arrayFree(scene->entities);
    mapFree(scene->entityMap);

    mapFree(scene->activeEntities);
    arrayFree(scene->activeEntityRemoveList);
    for (i32 i = 0, si = mapSize(scene->extras); i < si; i++) {
        jsonFree(scene->extras[i].value);
    }
    mapFree(scene->extras);
    stringDestroy(&scene->name);

    for (i32 i = 0, si = static_cast<i32>(arraySize(ecs.scenes)); i < si; i++) {
        if (ecs.scenes[i] == scene) {
            arrayDeleteSlow(ecs.scenes, i);
            break;
        }
    }

    memoryFree(scene);
}

Entity* searchEntity(const char* name) {
    foreach (auto item, ecs.scenes) {
        Entity* entity = sceneFindEntity(item, name);
        if (entity) return entity;
    }
    return nullptr;
}

Array(Scene*) sceneSystemGetVisibleScenes(void) {
    return visibleScenes;
}
