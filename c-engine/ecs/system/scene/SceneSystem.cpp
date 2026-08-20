#include "SceneSystem.h"
#include "SceneSystem.h"
#include "ecs/Ecs.h"
#include "ecs/components/Skin.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/mesh/MeshComponent.h"
#include "renderer/Renderer.h"
#include "thread/Thread.h"

namespace engine {
static int ComponentTypeCounter = 0;
static std::vector<Scene*> visibleScenes;

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

void SceneSystem::added() {}

void SceneSystem::removed() {
}

void SceneSystem::postUpdate() {
    visibleScenes.clear();

    Entity* cameraEntity = cameraGetEntity();
    Camera* camera      = cameraEntity ? getComponent(cameraEntity->scene, Camera, cameraEntity->id)
                                       : nullptr;
    vec4* frustumPlanes = camera ? (vec4*)camera->cameraUbo.frustumPlanes : nullptr;

    for (Scene* scene : ecs.scenes) {
        /* Scene bounds are currently static and computed once by SceneParser.
         * If a scene later needs dynamic or effectively unbounded contents,
         * revisit that policy instead of rebuilding bounds here every frame. */
        bool visible = true;
        if (!scene->alwaysVisible && frustumPlanes && scene->hasBounds) {
            visible = !aabbOutsideFrustum(scene->aabbMin, scene->aabbMax, frustumPlanes);
        }

        scene->visible = visible;
        if (visible) {
            visibleScenes.push_back(scene);
        }
    }

    rendererSetVisibleScenes(visibleScenes.data(), static_cast<u32>(static_cast<i32>(visibleScenes.size())));
}

SceneSystem sceneSystem;

SceneSystem::SceneSystem() : System("sceneSystem") {}

void* F_sceneCreateComponent(Scene* scene, u32 entity, u64* typeIdPtr, u64 size) {
    THREAD_LOCK;
    if (*typeIdPtr == 0) *typeIdPtr = ++ComponentTypeCounter;
    THREAD_UNLOCK;

    u64 id = *typeIdPtr;
    if (id >= scene->components.size()) {
        scene->components.resize(id + 1);
    }

    if (!scene->components[id]) {
        scene->components[id] = utils::ssNew(size);
    }

    return utils::ssNewItem(scene->components[id], entity);
}

void* F_sceneAddComponent(Scene* scene, u32 entity, u64* typeIdPtr, u64 size, void* data) {
    void* component = F_sceneCreateComponent(scene, entity, typeIdPtr, size);
    memcpy(component, data, size);
    return component;
}

void* F_sceneGetComponent(Scene* scene, u32 entity, u64* typeIdPtr) {
    if (*typeIdPtr == 0) return nullptr;
    u64 id = *typeIdPtr;
    if (id >= scene->components.size()) return nullptr;
    if (scene->components[id] == nullptr) return nullptr;
    return utils::ssGetDataByValue(scene->components[id], entity);
}

utils::SparseSet* F_sceneGetComponents(Scene* scene, u64* typeIdPtr) {
    if (*typeIdPtr == 0) return nullptr;
    u64 id = *typeIdPtr;
    if (id >= scene->components.size()) return nullptr;
    if (scene->components[id] == nullptr) return nullptr;
    return scene->components[id];
}

void F_sceneRemoveComponent(Scene* scene, u32 entity, u64* typeIdPtr) {
    if (*typeIdPtr == 0) return;
    int id = *typeIdPtr;
    if (scene->components[id] == nullptr) return;

    utils::ssRemoveByValue(scene->components[id], entity);
}

static u32 globalEntityCounter = 0;

Entity* createEntity(Scene* scene, const char* name) {
    u32 newEntityId = 0;
    if (!scene->entityFreeList.empty()) {
        newEntityId = scene->entityFreeList.back();
        scene->entityFreeList.pop_back();
    } else {
        globalEntityCounter++;
        newEntityId = globalEntityCounter;
    }

    Entity* entity = new Entity{
        .id       = newEntityId,
        .scene    = scene,
        .parent   = nullptr,
        .name     = nullptr,
    };

    if (name && name[0]) {
        size_t len = strlen(name);
        char* copy = static_cast<char*>(malloc(len + 1));
        memcpy(copy, name, len + 1);
        entity->name = copy;
    }

    scene->entities.push_back(entity);
    scene->entityMap[newEntityId] = entity;
    return entity;
}

Entity* getEntity(Scene* scene, u32 entityId) {
    auto it = scene->entityMap.find(entityId);
    return it != scene->entityMap.end() ? it->second : nullptr;
}

Entity* sceneFindEntity(Scene* scene, const char* name) {
    for (Entity* e : scene->entities) {
        if (e->name && utils::strequals(e->name, name)) return e;
    }
    return nullptr;
}

Entity* entityFindDescendant(Entity* root, const char* name) {
    for (Entity* child : root->children) {
        if (child->name && utils::strequals(child->name, name)) return child;
        Entity* found = entityFindDescendant(child, name);
        if (found) return found;
    }
    return nullptr;
}

void destroyEntity(Entity* entity) {
    Scene* scene = entity->scene;
    for (struct utils::SparseSet* item : scene->components) {
        if (item && utils::ssContainsValue(item, entity->id)) {
            utils::ssRemoveByValue(item, entity->id);
        }
    }

    if (entity->name) {
        free(const_cast<char*>(entity->name));
    }

    if (entity->parent) {
        for (i32 i = 0, si = static_cast<i32>(entity->parent->children.size()); i < si; i++) {
            if (entity->parent->children[i] == entity) {
                entity->parent->children.erase(entity->parent->children.begin() + i);
                break;
            }
        }
    }

    scene->entityMap.erase(entity->id);
    scene->entityFreeList.push_back(entity->id);

    for (i32 i = 0, si = static_cast<i32>(scene->entities.size()); i < si; i++) {
        if (scene->entities[i] == entity) {
            scene->entities.erase(scene->entities.begin() + i);
            break;
        }
    }

    delete entity;
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
        utils::warn("sceneSystem: scene destroy deferred until the in-flight async load completes");
        return;
    }
    for (utils::SparseSet* ss : scene->components) {
        if (ss) utils::ssDestroy(ss);
    }

    for (Entity* e : scene->entities) {
        if (e->name) free(const_cast<char*>(e->name));
        delete e;
    }

    for (const auto& entry : scene->extras) {
        jsonFree(entry.second);
    }
    utils::stringDestroy(&scene->name);

    for (i32 i = 0, si = static_cast<i32>(static_cast<i32>(ecs.scenes.size())); i < si; i++) {
        if (ecs.scenes[i] == scene) {
            ecs.scenes.erase(ecs.scenes.begin() + i);
            break;
        }
    }

    delete scene;
}

Entity* searchEntity(const char* name) {
    for (auto item : ecs.scenes) {
        Entity* entity = sceneFindEntity(item, name);
        if (entity) return entity;
    }
    return nullptr;
}

std::vector<Scene*> sceneSystemGetVisibleScenes(void) {
    return visibleScenes;
}
}  // namespace engine
