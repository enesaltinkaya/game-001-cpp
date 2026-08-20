#pragma once

#include "ecs/system/scene/Scene.h"
#include "ecs/system/System.h"

namespace engine {
class SceneSystem : public System {
public:
    SceneSystem();
    void added() override;
    void removed() override;
    void postUpdate() override;
};

extern SceneSystem sceneSystem;

#if defined(_MSC_VER)
#define REGISTER_COMPONENT(Type) __declspec(selectany) u64 Type##_id = 0
#elif defined(__MINGW32__) || defined(__MINGW64__)
#define REGISTER_COMPONENT(Type) __declspec(selectany) u64 Type##_id = 0
#else
#define REGISTER_COMPONENT(Type) __attribute__((weak)) u64 Type##_id = 0
#endif

#define createComponent(scene, Type, entity) (Type*)engine::F_sceneCreateComponent(scene, entity, &Type##_id, sizeof(struct Type))
#define addComponent(scene, Type, entity, data) (Type*)engine::F_sceneAddComponent(scene, entity, &Type##_id, sizeof(struct Type), data)
#define getComponent(scene, Type, entity) (Type*)engine::F_sceneGetComponent(scene, entity, &Type##_id)
#define getComponents(scene, Type) engine::F_sceneGetComponents(scene, &Type##_id)
#define removeComponent(scene, Type, entity) (Type*)engine::F_sceneRemoveComponent(scene, entity, &Type##_id)

void* F_sceneCreateComponent(Scene* scene, u32 entity, u64* typeIdPtr, u64 size);
void* F_sceneAddComponent(Scene* scene, u32 entity, u64* typeIdPtr, u64 size, void* data);
void* F_sceneGetComponent(Scene* scene, u32 entity, u64* typeIdPtr);
utils::SparseSet* F_sceneGetComponents(Scene* scene, u64* typeIdPtr);
void F_sceneRemoveComponent(Scene* scene, u32 entity, u64* typeIdPtr);

Entity* createEntity(Scene* scene, const char* name);
Entity* getEntity(Scene* scene, u32 entityId);
Entity* sceneFindEntity(Scene* scene, const char* name);
Entity* entityFindDescendant(Entity* root, const char* name);
void destroyEntity(Entity* entity);
void sceneDestroy(Scene* scene);
Entity* searchEntity(const char* name);
std::vector<Scene*> sceneSystemGetVisibleScenes(void);
}  // namespace engine
