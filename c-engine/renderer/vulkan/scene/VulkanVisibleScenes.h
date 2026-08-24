#pragma once

namespace engine {
struct Scene;

/// Called by the renderer bridge once per frame after scene-level frustum
/// culling.  Stores a shallow copy of the pointer array so that render
/// passes can iterate only the visible (non-culled) scenes.
void vulkanSetVisibleScenes(struct Scene** scenes, u32 count);

/// Returns the current visible-scene list (valid for the current frame).
const struct Scene** vulkanGetVisibleScenes(u32* outCount);
}  // namespace engine
