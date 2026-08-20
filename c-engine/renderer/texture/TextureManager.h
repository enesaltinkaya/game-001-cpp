#pragma once

struct Texture {
    Image image;
    String name;
    void* backendImg;
    int id;  // also poolIndex into global vulkan texture array
    bool nearest;
    u32 refCount;
};

// Primary lookup: returns existing texture, or tries to load from pak by name, or nullptr.
Texture* getTextureByName(const char* name);
Texture* getTextureById(u32 id);

// For embedded glTF textures (data not in paks).
Texture* createTextureFromData(const char* name, const u8* data, u64 size, const char* mime, bool nonColor);

void textureManagerInit();
void textureManagerDestroy();
