#include "ecs/Ecs.h"
#include "ecs/system/System.h"
#include "ecs/system/mesh/MeshComponent.h"
#include "renderer/Renderer.h"

static void initMesh(u32 entity, void* pMesh);
static void destroyMesh(u32 entity, void* pMesh);

static void added(void);
static void postUpdate(void);

struct System meshSystem = {
    .name       = "mesh",
    .added      = added,
    .postUpdate = postUpdate,
};

void added(void) {}

void postUpdate(void) {
    // SparseSet* ss = components(Mesh);
    // for (i32 i = 0, si = ss->size; i < si; i++) {
    //     Mesh* mesh = ssGetDataByIndex(ss, i);
    //     info("%d", mesh->primitives[0].indexCount);
    // }
}

void initMesh(u32 entity, void* pMesh) {
}

void destroyMesh(u32 entity, void* pMesh) {
    (void)entity;
    Mesh* mesh  = static_cast<Mesh*>(pMesh);
    for (i32 i = 0, si = arraySize(mesh->primitives); i < si; i++) {
        Primitive* primitive = &mesh->primitives[i];
        arrayFree(primitive->indices);
        arrayFree(primitive->positions);
        foreach (auto item, primitive->attributes) {
            arrayFree(item);
        }
    }
    arrayFree(mesh->primitives);
    arrayFree(mesh->instances);
}
