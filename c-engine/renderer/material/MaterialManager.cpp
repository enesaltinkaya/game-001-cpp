#include "Utils.h"
#include "Material.h"
#include "renderer/Renderer.h"

namespace engine {
static std::unordered_map<std::string, Material*> materialMap;
static std::unordered_map<u32, Material*> materialIdMap;

static u32 materialCounter;
static utils::Thread lock = {.mutex = PTHREAD_MUTEX_INITIALIZER};

Material* createMaterial(const char* name) {
    utils::threadLock(&lock);
    if (materialMap.contains(name)) {
        utils::terminate("material \"%s\" is already initialized!", name);
    }

    Material* material = new Material{};
    material->id       = materialCounter;
    materialMap[name] = material;
    materialIdMap[material->id] = material;
    materialCounter++;
    // info("created material: %s", name);
    utils::threadUnlock(&lock);
    return material;
}

Material* getMaterialByName(const char* name) {
    auto it = materialMap.find(name);
    return it != materialMap.end() ? it->second : nullptr;
}

Material* getMaterialById(const u32 id) {
    auto it = materialIdMap.find(id);
    return it != materialIdMap.end() ? it->second : nullptr;
}

void createDefaultMaterial() {
    Material* material     = createMaterial("default");
    material->baseColor[0] = material->baseColor[1] = material->baseColor[2] = material->baseColor[3] = 1;
    material->rmas[0] = material->rmas[2] = material->rmas[3] = 1;
    material->rmas[1]                                         = 0;
    material->featureMask |= (1u << MAT_ALPHA_OPAQUE);
    material->refCount++;
    
    rendererUploadMaterial(material);
}

void cleanupMaterials() {
    for (const auto& entry : materialMap) {
        delete entry.second;
    }
}

void destroyMaterial(const u32 id) {
    Material* material = getMaterialById(id);
    material->refCount--;
    if (material->refCount == 0) {
    }
    utils::warn("destroy material: %u", id);
}
}  // namespace engine
