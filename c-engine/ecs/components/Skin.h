#pragma once
#include "ecs/system/scene/SceneSystem.h"
#include "ecs/system/transform/TransformComponent.h"

REGISTER_COMPONENT(Skin);
typedef struct Skin {
    Array(u32) joints;
    Array(mat4) inverseBindMatrices;

    // OPTIMIZATION: Store joint transforms as Transform structs (2 vec4s = 32 bytes)
    // instead of mat4 (16 floats = 64 bytes) - 50% memory reduction!
    Array(Transform) jointTransforms;
    u32 jointBufferCursor;
} Skin;
