#pragma once

#include "renderer/vulkan/resources/VulkanBuffer.h"
#include "ecs/system/transform/TransformComponent.h"
#include "renderer/Renderer.h"

struct TransformUpload {
    Transform transform;
    u32 entity;
};

// Interleaved vertex for the scene VBO
struct SceneVertex {
    float position[3];   // 12 bytes
    float normal[3];     // 12 bytes
    float tangent[4];    // 16 bytes
    float uv[2];         // 8 bytes
    uint32_t joints;     // 4 bytes — 4x uint8 joint indices
    uint32_t weights;    // 4 bytes — 4x unorm8 bone weights
};                        // 56 bytes

// GPU-side draw instance (one per SceneDraw, lives on GPU)
struct GpuDrawInstance {
    u32 firstIndex;
    u32 indexCount;
    i32 vertexOffset;
    u32 entity;
    u32 materialId;
    u32 flags;        // bit 0 = doubleSided, bit 1 = transparent
    u32 _pad0;
    u32 _pad1;
    float boundingSphere[4]; // xyz = local center, w = local radius
};

// VkDrawIndexedIndirectCommand compatible struct
struct SceneDrawIndexedCommand {
    u32 indexCount;
    u32 instanceCount;
    u32 firstIndex;
    i32 vertexOffset;
    u32 firstInstance;
};

#define DRAW_FLAG_DOUBLE_SIDED 1u
#define DRAW_FLAG_TRANSPARENT  2u
#define DRAW_FLAG_SKINNED      4u

struct VulkanScene {
    // Geometry buffers
    VulkanBuffer vertexBuffer;   // SceneVertex[]
    VulkanBuffer indexBuffer;    // u32[]

    // GPU-side draw instance buffer
    VulkanBuffer drawInstanceBuffer; // GpuDrawInstance[]

    // Transform buffers (per-frame, CPU-mapped)
    VulkanBuffer transformBuffer[FRAMES_IN_FLIGHT];
    VulkanBuffer prevTransformBuffer[FRAMES_IN_FLIGHT];
    Array(TransformUpload) transformUploads[FRAMES_IN_FLIGHT];

    // GPU culling output: opaque single-sided
    VulkanBuffer indirectBuffer[FRAMES_IN_FLIGHT];
    VulkanBuffer drawCountBuffer[FRAMES_IN_FLIGHT];
    VulkanBuffer culledBuffer[FRAMES_IN_FLIGHT];

    // GPU culling output: opaque double-sided
    VulkanBuffer dsIndirectBuffer[FRAMES_IN_FLIGHT];
    VulkanBuffer dsDrawCountBuffer[FRAMES_IN_FLIGHT];
    VulkanBuffer dsCulledBuffer[FRAMES_IN_FLIGHT];

    // GPU culling output: transparent
    VulkanBuffer transIndirectBuffer[FRAMES_IN_FLIGHT];
    VulkanBuffer transDrawCountBuffer[FRAMES_IN_FLIGHT];
    VulkanBuffer transCulledBuffer[FRAMES_IN_FLIGHT];

    // Per-draw visibility flags (0=frustum-culled, 1=visible, 2=occlusion-rejected)
    VulkanBuffer visibilityBuffer[FRAMES_IN_FLIGHT];

    // Phase 2 occlusion culling output: opaque single-sided
    VulkanBuffer phase2IndirectBuffer[FRAMES_IN_FLIGHT];
    VulkanBuffer phase2DrawCountBuffer[FRAMES_IN_FLIGHT];
    VulkanBuffer phase2CulledBuffer[FRAMES_IN_FLIGHT];

    // Phase 2 occlusion culling output: opaque double-sided
    VulkanBuffer phase2DsIndirectBuffer[FRAMES_IN_FLIGHT];
    VulkanBuffer phase2DsDrawCountBuffer[FRAMES_IN_FLIGHT];
    VulkanBuffer phase2DsCulledBuffer[FRAMES_IN_FLIGHT];

    // Stats readback buffer (host-visible, 8 bytes: drawCalls + triangleCount)
    VulkanBuffer statsReadbackBuffer[FRAMES_IN_FLIGHT];

    // Skinning buffers
    VulkanBuffer jointMatrixBuffer[FRAMES_IN_FLIGHT];
    VulkanBuffer prevJointMatrixBuffer[FRAMES_IN_FLIGHT];
    VulkanBuffer entitySkinMapBuffer[FRAMES_IN_FLIGHT];

    u32 totalSkinJointCount;
    u32 totalSkinnedEntityCount;
    u32 totalVertices;
    u32 totalIndices;
    u32 totalDraws;
};

void vulkanSceneCreate(struct Scene* scene);
void vulkanSceneDestroy(struct Scene* scene);
void vulkanSceneUploadTransform(struct Scene* scene,
                                u32 entity,
                                struct Transform* transform);
void vulkanSceneFlushTransforms(struct VulkanScene* vs);
void vulkanSceneInitSkinning(struct Scene* scene);
void vulkanSceneFlushJoints(struct Scene* scene);
