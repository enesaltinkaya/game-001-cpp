#pragma once

#include "ecs/system/System.h"

namespace engine {
class VulkanBloomPass : public System {
public:
    VulkanBloomPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern VulkanBloomPass vulkanBloomPass;

/// Returns the sampled pool index of bloom mip 0 (half-res bloom result).
/// Returns 0 if bloom is not available (first frame, invalid size, etc.).
int vulkanBloomPassGetBloomSampledIndex(void);

/// Returns the bloom mip-0 image (half-res) for debugging; NULL when bloom
/// is disabled or not created yet.
struct VulkanImage* vulkanBloomPassGetBloomImage(void);

/// Returns the bloom strength for the final composite.
float vulkanBloomPassGetStrength(void);

void  vulkanBloomPassSetDisabled(char disabled);
char  vulkanBloomPassIsDisabled(void);
}  // namespace engine
