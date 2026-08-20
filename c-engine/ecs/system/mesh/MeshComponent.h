#pragma once

#include "cglm/git/include/cglm/types.h"
#include "cgltf/git/cgltf.h"
#include "ecs/system/scene/SceneSystem.h"

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

typedef struct InstanceData {
    u32 entity;
} InstanceData;

typedef struct Primitive {
    Array(u32) indices;
    Array(float) positions;
    Array(float) colors;  // unpacked COLOR_0 (per-part colour; white = tintable)
    Array(u8) attributes[cgltf_attribute_type_max_enum];
    u32 attributeMask;

    u32 indexCount;
    u32 vertexCount;

    u32 materialId;
} Primitive;

REGISTER_COMPONENT(Mesh);

typedef struct Mesh {
    Array(Primitive) primitives;
    Array(InstanceData) instances;
    vec3 aabbLocal[2];  // min-max
    vec3 aabbWorld[2];  // updated when transform changes
} Mesh;
