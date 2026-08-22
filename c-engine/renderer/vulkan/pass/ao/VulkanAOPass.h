#pragma once

#include "ecs/system/System.h"

namespace engine {
struct VulkanImage;

/* Ambient occlusion via AMD FidelityFX CACAO (depth-based; normals are
 * reconstructed from depth — the engine's normal buffer is oct-encoded and
 * incompatible with CACAO's affine unpack). */
class VulkanAOPass : public System {
public:
    VulkanAOPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern VulkanAOPass vulkanAOPass;

void  vulkanAOPassSetDisabled(char disabled);
char  vulkanAOPassIsDisabled(void);
/// Current-frame CACAO output (R16G16B16A16_SFLOAT, .r = occlusion,
/// 1 = unoccluded).  Sampled by the composite pass; NULL before the
/// first swapchain exists or while disabled.
struct VulkanImage* vulkanAOPassGetOutput(void);
}  // namespace engine