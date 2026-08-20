#pragma once

#include "ecs/system/System.h"

namespace engine {
struct VulkanImage;

class VulkanHiZPass : public System {
public:
    VulkanHiZPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern VulkanHiZPass vulkanHiZPass;

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
}  // namespace engine
