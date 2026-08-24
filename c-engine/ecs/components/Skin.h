#pragma once
#include "ecs/system/scene/SceneSystem.h"
#include "ecs/system/transform/TransformComponent.h"

namespace engine {
REGISTER_COMPONENT(Skin);
typedef struct Skin {
    std::vector<u32> joints;
    std::vector<float> inverseBindMatrices;  // flat, 16 floats per matrix

    // OPTIMIZATION: Store joint transforms as Transform structs (2 vec4s = 32 bytes)
    // instead of mat4 (16 floats = 64 bytes) - 50% memory reduction!
    std::vector<Transform> jointTransforms;
    u32 jointBufferCursor;
} Skin;
}  // namespace engine
