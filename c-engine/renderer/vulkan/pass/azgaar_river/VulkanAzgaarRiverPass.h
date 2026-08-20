#pragma once

#include "ecs/system/System.h"
#include "renderer/vulkan/resources/VulkanBuffer.h"

namespace engine {
class VulkanAzgaarRiverPass : public System {
public:
    VulkanAzgaarRiverPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
};

extern VulkanAzgaarRiverPass vulkanAzgaarRiverPass;

// Upload (or clear) the river ribbon mesh.  Called from the CPU river side
// (AzgaarRivers.c) on the game thread; uploaded to the GPU on the render
// thread via a transient command buffer.
//
//   vertices: SceneVertex[]  (position, normal, tangent, uv)
//   indices:  u32[]
//   vertexCount / indexCount: sizes
void vulkanAzgaarRiverSetMesh(const void* vertices, u32 vertexCount,
                               const void* indices, u32 indexCount);
void vulkanAzgaarRiverClear(void);

// Expose the uploaded GPU mesh (same contract as vulkanAzgaarWaterGetGpuMesh).
bool vulkanAzgaarRiverGetGpuMesh(VulkanBuffer** outVertexBuffer,
                                  VulkanBuffer** outIndexBuffer,
                                  u32* outVertexCount,
                                  u32* outIndexCount);
}  // namespace engine
