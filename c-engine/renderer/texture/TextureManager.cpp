#include "TextureManager.h"
#include "renderer/Renderer.h"

static StrMap(Texture*) textureMap;
static Map(u32, Texture*) textureIdMap;
static Thread textureLock = {.mutex = PTHREAD_MUTEX_INITIALIZER};

static void ensureTextureMapInit(void) {
    if (!textureMap) {
        sh_new_strdup(textureMap);
    }
}

typedef struct TextureCreateInfo {
    const char* path;
    const u8* data;
    u64 size;
    const char* mime;
    const char* name;
    char nonColor, genMips;
} TextureCreateInfo;

#define createTexture(...) internalCreateTexture((TextureCreateInfo){.genMips = 1, __VA_ARGS__})

static Texture* internalCreateTexture(TextureCreateInfo info) {
    const char* key = info.path ? info.path : info.name;
    // debug("loading texture: %s", info.path);

    // Check if already loaded
    threadLock(&textureLock);
    ensureTextureMapInit();
    if (strmapContainsKey(textureMap, key)) {
        Texture* existing = strmapGet(textureMap, key);
        existing->refCount++;
        threadUnlock(&textureLock);
        return existing;
    }

    // Reserve slot so other threads see this key is being loaded
    Texture* texture = static_cast<Texture*>(memoryAlloc(sizeof(Texture)));
    strmapPut(textureMap, key, texture);
    threadUnlock(&textureLock);

    // Heavy work: decode image + GPU upload (no lock needed)
    if (info.path) {
        texture->image = imageLoad(info.path);
        stringPrintf(&texture->name, info.path);
    } else {
        texture->image = imageLoadFromData(info.data, info.size, info.mime);
        stringPrintf(&texture->name, info.name);
    }

    rendererUploadTexture(texture, info.nonColor, info.genMips);

    // Register by id
    threadLock(&textureLock);
    if (mapContainsKey(textureIdMap, texture->id)) {
        terminate("textureId %d for \"%s\" exists", texture->id, key);
    }

    mapPut(textureIdMap, texture->id, texture);
    texture->refCount++;
    threadUnlock(&textureLock);

    return texture;
}

Texture* getTextureByName(const char* name) {
    threadLock(&textureLock);
    if (strmapContainsKey(textureMap, name)) {
        Texture* t = strmapGet(textureMap, name);
        threadUnlock(&textureLock);
        return t;
    }
    threadUnlock(&textureLock);

    // Try loading from pak
    if (dataManagerFileExists(name)) {
        return createTexture(.path = name, .genMips = 0);
    }

    return NULL;
}

Texture* getTextureById(u32 id) {
    threadLock(&textureLock);
    Texture* t = mapContainsKey(textureIdMap, id) ? mapGet(textureIdMap, id) : NULL;
    threadUnlock(&textureLock);
    return t;
}

Texture* createTextureFromData(const char* name,
                               const u8* data,
                               u64 size,
                               const char* mime,
                               char nonColor) {
    return createTexture(.name     = name,
                         .data     = data,
                         .size     = size,
                         .mime     = mime,
                         .nonColor = nonColor);
}

void textureManagerDestroy(void) {
    for (i32 i = 0, si = strmapSize(textureMap); i < si; i++) {
        Texture* texture = textureMap[i].value;
        rendererDestroyTexture(texture);
        stringDestroy(&texture->name);
        memoryFree(texture);
    }
    strmapFree(textureMap);
    mapFree(textureIdMap);
}

static void loadTextureWork(void* arg) {
    const char* path  = static_cast<const char*>(arg);
    createTexture(.path = path, .genMips = 0);
}

void textureManagerInit() {
    double elapsed = elapsedBegin();

    Array(String) ktxFiles = dataManagerListFiles(".ktx2");
    info("textureManager: found %d ktx2 files in paks", arraySize(ktxFiles));

    for (i32 i = 0, s = arraySize(ktxFiles); i < s; i++) {
        threadPoolAddWork(NULL, loadTextureWork, ktxFiles[i].data);
    }

    threadPoolWait(NULL);

    for (i32 i = 0, s = arraySize(ktxFiles); i < s; i++) {
        stringDestroy(&ktxFiles[i]);
    }
    arrayFree(ktxFiles);

    elapsed = elapsedEnd(elapsed);
    info("textureManager: preloaded %d textures in %.02f ms", strmapSize(textureMap), elapsed);
}
