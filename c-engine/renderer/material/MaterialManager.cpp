#include "Utils.h"
#include "Material.h"
#include "renderer/Renderer.h"

static StrMap(Material*) materialMap;
static Map(u32, Material*) materialIdMap;

static u32 materialCounter;
static Thread lock = {.mutex = PTHREAD_MUTEX_INITIALIZER};

Material* createMaterial(const char* name) {
    threadLock(&lock);
    if (strmapContainsKey(materialMap, name)) {
        terminate("material \"%s\" is already initialized!", name);
    }

    Material* material = static_cast<Material*>(memoryAlloc(sizeof(Material)));
    *material          = Material{};
    material->id       = materialCounter;
    strmapPut(materialMap, name, material);
    mapPut(materialIdMap, material->id, material);
    materialCounter++;
    // info("created material: %s", name);
    threadUnlock(&lock);
    return material;
}

Material* getMaterialByName(const char* name) {
    if (!strmapContainsKey(materialMap, name)) {
        return nullptr;
    }
    return strmapGet(materialMap, name);
}

Material* getMaterialById(const u32 id) {
    if (!mapContainsKey(materialIdMap, id)) {
        return nullptr;
    }
    return mapGet(materialIdMap, id);
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
    for (i32 i = 0, si = strmapSize(materialMap); i < si; i++) {
        memoryFree(materialMap[i].value);
    }
    mapFree(materialIdMap);
    strmapFree(materialMap);
}

void destroyMaterial(const u32 id) {
    Material* material = getMaterialById(id);
    material->refCount--;
    if (material->refCount == 0) {
    }
    warn("destroy material: %u", id);
}
