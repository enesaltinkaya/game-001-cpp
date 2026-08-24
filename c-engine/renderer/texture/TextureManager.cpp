#include "TextureManager.h"
#include "renderer/Renderer.h"

namespace engine {
static std::unordered_map<std::string, Texture*> textureMap;
static std::unordered_map<u32, Texture*> textureIdMap;
static utils::Thread textureLock = {.mutex = PTHREAD_MUTEX_INITIALIZER};

struct TextureCreateInfo {
    const char* path = nullptr;
    const u8* data = nullptr;
    u64 size = 0;
    const char* mime = nullptr;
    const char* name = nullptr;
    bool nonColor = false;
    bool genMips = true;
};

#define createTexture(...) internalCreateTexture(TextureCreateInfo{__VA_ARGS__})

static Texture* internalCreateTexture(TextureCreateInfo info) {
    const char* key = info.path ? info.path : info.name;
    // debug("loading texture: %s", info.path);

    // Check if already loaded
    utils::threadLock(&textureLock);
    auto it = textureMap.find(key);
    if (it != textureMap.end()) {
        Texture* existing = it->second;
        existing->refCount++;
        utils::threadUnlock(&textureLock);
        return existing;
    }

    // Reserve slot so other threads see this key is being loaded
    Texture* texture = new Texture{};
    textureMap[key] = texture;
    utils::threadUnlock(&textureLock);

    // Heavy work: decode image + GPU upload (no lock needed)
    if (info.path) {
        texture->image = utils::imageLoad(info.path);
        utils::stringPrintf(&texture->name, info.path);
    } else {
        texture->image = utils::imageLoadFromData(info.data, info.size, info.mime);
        utils::stringPrintf(&texture->name, info.name);
    }

    rendererUploadTexture(texture, info.nonColor, info.genMips);

    // Register by id
    utils::threadLock(&textureLock);
    if (textureIdMap.contains(texture->id)) {
        utils::terminate("textureId %d for \"%s\" exists", texture->id, key);
    }

    textureIdMap[texture->id] = texture;
    texture->refCount++;
    utils::threadUnlock(&textureLock);

    return texture;
}

Texture* getTextureByName(const char* name) {
    utils::threadLock(&textureLock);
    auto it = textureMap.find(name);
    if (it != textureMap.end()) {
        Texture* t = it->second;
        utils::threadUnlock(&textureLock);
        return t;
    }
    utils::threadUnlock(&textureLock);

    // Try loading from pak
    if (utils::dataManagerFileExists(name)) {
        return createTexture(.path = name, .genMips = 0);
    }

    return nullptr;
}

Texture* getTextureById(u32 id) {
    utils::threadLock(&textureLock);
    auto it = textureIdMap.find(id);
    Texture* t = it != textureIdMap.end() ? it->second : nullptr;
    utils::threadUnlock(&textureLock);
    return t;
}

Texture* createTextureFromData(const char* name,
                                const u8* data,
                                u64 size,
                                const char* mime,
                                bool nonColor) {
    return createTexture(.data     = data,
                         .size     = size,
                         .mime     = mime,
                         .name     = name,
                         .nonColor = nonColor);
}

void textureManagerDestroy(void) {
    for (const auto& entry : textureMap) {
        Texture* texture = entry.second;
        rendererDestroyTexture(texture);
        utils::stringDestroy(&texture->name);
        delete texture;
    }
}

static void loadTextureWork(void* arg) {
    const char* path  = static_cast<const char*>(arg);
    createTexture(.path = path, .genMips = 0);
}

void textureManagerInit() {
    double elapsed = utils::elapsedBegin();

    std::vector<utils::String> ktxFiles = utils::dataManagerListFiles(".ktx2");
    utils::info("textureManager: found %d ktx2 files in paks", static_cast<i32>(ktxFiles.size()));

    for (i32 i = 0, s = static_cast<i32>(ktxFiles.size()); i < s; i++) {
        utils::threadPoolAddWork(NULL, loadTextureWork, ktxFiles[i].data);
    }

    utils::threadPoolWait(NULL);

    for (i32 i = 0, s = static_cast<i32>(ktxFiles.size()); i < s; i++) {
        utils::stringDestroy(&ktxFiles[i]);
    }

    elapsed = utils::elapsedEnd(elapsed);
    utils::info("textureManager: preloaded %d textures in %.02f ms", static_cast<i32>(textureMap.size()), elapsed);
}
}  // namespace engine
