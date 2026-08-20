#include "MeshSystem.h"
#include "ecs/Ecs.h"
#include "ecs/system/System.h"
#include "ecs/system/mesh/MeshComponent.h"
#include "renderer/Renderer.h"

namespace engine {


MeshSystem meshSystem;

MeshSystem::MeshSystem() : System("mesh") {}

void MeshSystem::added() {}

void MeshSystem::postUpdate() {
    // SparseSet* ss = components(Mesh);
    // for (i32 i = 0, si = ss->size; i < si; i++) {
    //     Mesh* mesh = ssGetDataByIndex(ss, i);
    //     info("%d", mesh->primitives[0].indexCount);
    // }
}
}  // namespace engine
