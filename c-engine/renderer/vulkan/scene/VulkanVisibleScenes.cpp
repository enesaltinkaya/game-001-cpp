#include "VulkanVisibleScenes.h"
#include "ecs/system/scene/Scene.h"

namespace engine {
static std::vector<const Scene*> storedVisibleScenes;
static u32 storedVisibleSceneCount;

void vulkanSetVisibleScenes(Scene** scenes, u32 count) {
    storedVisibleScenes.clear();
    for (u32 i = 0; i < count; i++) {
        storedVisibleScenes.push_back(scenes[i]);
    }
    storedVisibleSceneCount = count;
}

const struct Scene** vulkanGetVisibleScenes(u32* outCount) {
    if (outCount) *outCount = storedVisibleSceneCount;
    return storedVisibleScenes.data();
}
}  // namespace engine
