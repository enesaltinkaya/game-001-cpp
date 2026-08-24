#pragma once
#include "ecs/system/System.h"
#include "rmlui/wrapper/src/crmlui.h"

namespace engine {
class VulkanRmluiPass : public System {
public:
    VulkanRmluiPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void postUpdate() override;
};

extern VulkanRmluiPass vulkanRmluiPass;

void rmlBeginFrame(void);
void rmlEndFrame(void);
void rmlRenderGeometry(uintptr_t rmlGeometryHandle, float translationX, float translationY, uintptr_t texture);
uintptr_t rmlCompileGeometry(RmlVertex* vertices, int vertexCount, const int* indices, int indexCount);
void rmlReleaseGeometry(uintptr_t rmlGeometryHandle);
uintptr_t rmlLoadTexture(int* outX, int* outY, const char* path);
uintptr_t rmlGenerateTexture(const unsigned char* data, size_t _, int x, int y);
void rmlReleaseTexture(uintptr_t textureHandleOut);
void rmlEnableScissorRegion(char enable);
void rmlSetScissorRegion(int x, int y, int width, int height);
void rmlSetTransform(void* transform);
void rmlSetViewport(int width, int height);
}  // namespace engine
