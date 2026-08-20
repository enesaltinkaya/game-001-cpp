#pragma once

#include "ecs/system/System.h"

struct VulkanImage;

extern System vulkanHiZPass;

/// Returns current frame's Hi-Z image (for SSR, culling, etc.)
struct VulkanImage* vulkanHiZGetCurrentImage(void);

/// Returns previous frame's Hi-Z image (for occlusion culling)
struct VulkanImage* vulkanHiZGetPreviousImage(void);

/// Returns the number of mip levels in the Hi-Z chain
int vulkanHiZGetMipCount(void);

/// Returns the sampled pool index for a specific mip level of the current frame's Hi-Z
u32 vulkanHiZGetMipSampledIndex(int mip);

/// Returns the storage pool index for a specific mip level of the current frame's Hi-Z
u32 vulkanHiZGetMipStorageIndex(int mip);
