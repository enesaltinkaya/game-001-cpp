#pragma once

#include "ecs/system/scene/Scene.h"
#include "ecs/system/System.h"
#include <memory>  // std::destroy_at

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

// Tracked variants for non-trivial component types (those containing std::
// members or owning pointers).  The sparse-set storage invokes the per-type
// destructor when an element is removed or the set is destroyed/removed, so
// the component's heap members are actually freed (plain createComponent()
// only frees the raw bytes and would leak anything owned by the component).
// All creation sites for a given type MUST use the same variant.
void* F_sceneCreateComponent(Scene* scene, u32 entity, u64* typeIdPtr, u64 size);
// Like F_sceneCreateComponent but registers C++ element callbacks with the
// per-scene component set so non-trivial components are properly destroyed.
// Both callbacks may be nullptr (then identical to F_sceneCreateComponent).
void* F_sceneCreateComponentDtor(Scene* scene, u32 entity, u64* typeIdPtr, u64 size,
                                 void (*destroy)(void*), void (*swapIn)(void*, void*));

// Per-type callbacks used by createComponentT().  Components are constructed
// IN-PLACE inside the sparse set's raw byte buffer (ssNewItem), so they must
// be torn down with std::destroy_at()/placement-new — never operator delete.
// Both must be namespace-scope function templates (a local function inside a
// template is ill-formed when its body depends on the template parameter).
template <typename T>
void trackedComponentDestroy(void* pComponent) {
    std::destroy_at(static_cast<T*>(pComponent));
}

template <typename T>
void trackedComponentSwapIn(void* pDst, void* pSrc) {
    T* dst = static_cast<T*>(pDst);
    T* src = static_cast<T*>(pSrc);
    if (dst == src) return;
    dst->~T();
    ::new (dst) T(std::move(*src));  // leaves src moved-from but valid
}

template <typename T>
T* createComponentTImpl(Scene* scene, u32 entity, u64* typeId) {
    return static_cast<T*>(engine::F_sceneCreateComponentDtor(scene, entity, typeId, sizeof(T),
                                                              &trackedComponentDestroy<T>,
                                                              &trackedComponentSwapIn<T>));
}

template <typename T>
T* createComponentTDtorImpl(Scene* scene, u32 entity, u64* typeId, void (*destroyFn)(void*),
                            void (*swapInFn)(void*, void*)) {
    return static_cast<T*>(
        engine::F_sceneCreateComponentDtor(scene, entity, typeId, sizeof(T), destroyFn, swapInFn));
}

#define createComponentT(scene, Type, entity) ::engine::createComponentTImpl<Type>(scene, entity, &Type##_id)
#define createComponentTDtor(scene, Type, entity, destroyFn, swapInFn) \
    ::engine::createComponentTDtorImpl<Type>(scene, entity, &Type##_id, destroyFn, swapInFn)

void* F_sceneAddComponent(Scene* scene, u32 entity, u64* typeIdPtr, u64 size, void* data);
void* F_sceneGetComponent(Scene* scene, u32 entity, u64* typeIdPtr);
utils::SparseSet* F_sceneGetComponents(Scene* scene, u64* typeIdPtr);
void F_sceneRemoveComponent(Scene* scene, u32 entity, u64* typeIdPtr);

Entity* createEntity(Scene* scene, const char* name);
Entity* getEntity(Scene* scene, u32 entityId);
Entity* sceneFindEntity(Scene* scene, const char* name);
Entity* entityFindDescendant(Entity* root, const char* name);
void destroyEntity(Entity* entity);
// Free everything owned by a scene (component sets, entities, extras, name)
// without deleting the Scene struct itself or touching ecs.scenes.  This is
// what sceneDestroy() uses internally and what callers use for statically
// allocated scenes (e.g. ecs.defaultScene).
void sceneCleanupContents(Scene* scene);
void sceneDestroy(Scene* scene);
Entity* searchEntity(const char* name);
std::vector<Scene*> sceneSystemGetVisibleScenes(void);
}  // namespace engine
