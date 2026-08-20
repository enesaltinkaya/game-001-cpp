#pragma once

#include "ecs/system/System.h"
#include "renderer/vulkan/resources/VulkanBuffer.h"

extern struct System vulkanAzgaarWaterPass;

// Upload (or clear) the water grid mesh.  Called from the CPU water side
// (AzgaarWater.c) on the game thread; uploaded to the GPU on the render
// thread via a transient command buffer.
//
//   vertices: SceneVertex[]  (position, normal, tangent, uv)
//   indices:  uint32_t[]
//   vertexCount / indexCount: sizes
void vulkanAzgaarWaterSetMesh(const void* vertices, u32 vertexCount,
                               const void* indices, u32 indexCount);
void vulkanAzgaarWaterClear(void);

// Expose the uploaded GPU mesh so the depth/velocity pre-pass can render
// azgaar water into the velocity buffer. Without this, FSR has no motion
// vectors for water pixels and the upscaled image ghosts badly.
// Returns false if no mesh is currently uploaded; output pointers are valid
// only until the next set/clear (i.e. within the same frame).
bool vulkanAzgaarWaterGetGpuMesh(VulkanBuffer** outVertexBuffer,
                                  VulkanBuffer** outIndexBuffer,
                                  u32* outVertexCount,
                                  u32* outIndexCount);

// Per-frame visibility test for the (effectively infinite) sea plane.
// Returns false when water params are disabled, the camera is underwater,
// or when the entire view frustum lies above the water surface (e.g.
// flying over inland terrain) — in those cases no water can be on screen,
// so both the water color pass and the depth/velocity pre-pass draw can
// be skipped entirely.
bool vulkanAzgaarWaterIsVisible(void);
