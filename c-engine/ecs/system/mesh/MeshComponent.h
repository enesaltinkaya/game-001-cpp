#pragma once

#include "cglm/git/include/cglm/types.h"
#include "cgltf/git/cgltf.h"
#include "ecs/system/scene/SceneSystem.h"

namespace engine {
struct MeshData {
    u32 positionOffset;
    u32 normalOffset;
    u32 tangentOffset;
    u32 uvOffset;
    u32 colorOffset;
    u32 jointsOffset;
    u32 weightsOffset;
    u32 customOffset;
};

struct InstanceData {
    u32 entity;
};

struct Primitive {
    std::vector<u32> indices;
    std::vector<float> positions;
    std::vector<float> colors;  // unpacked COLOR_0 (per-part colour; white = tintable)
    std::vector<u8> attributes[cgltf_attribute_type_max_enum];
    u32 attributeMask;

    u32 indexCount;
    u32 vertexCount;

    u32 materialId;
};

REGISTER_COMPONENT(Mesh);

struct Mesh {
    std::vector<Primitive> primitives;
    std::vector<InstanceData> instances;
    vec3 aabbLocal[2];  // min-max
    vec3 aabbWorld[2];  // updated when transform changes
};
}  // namespace engine
