#include "VulkanVisibleScenes.h"
#include "ecs/system/scene/Scene.h"

static Array(Scene*) storedVisibleScenes;
static u32 storedVisibleSceneCount;

void vulkanSetVisibleScenes(Scene** scenes, u32 count) {
    arrayClear(storedVisibleScenes);
    for (u32 i = 0; i < count; i++) {
        arrayPut(storedVisibleScenes, scenes[i]);
    }
    storedVisibleSceneCount = count;
}

Scene** vulkanGetVisibleScenes(u32* outCount) {
    if (outCount) *outCount = storedVisibleSceneCount;
    return storedVisibleScenes;
}
