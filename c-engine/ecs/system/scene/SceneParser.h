#pragma once

namespace engine {
struct Scene;

typedef void (*SceneLoadCallback)(struct Scene* scene, void* userData);

struct Scene* sceneLoad(const char* path);
struct Scene* sceneLoadCb(const char* path, SceneLoadCallback callback, void* userData);
}  // namespace engine
