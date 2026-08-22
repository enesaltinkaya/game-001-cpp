#pragma once

#include "ecs/system/System.h"

namespace engine {
struct VulkanImage;

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
/// Current-frame AO temporal accumulator (R16G16_SFLOAT: .r = occlusion,
/// .g = S-space inverse view depth, 0 = no data).  Sampled by the composite
/// pass; NULL before the first swapchain exists.
struct VulkanImage* vulkanAOPassGetOutput(void);
}  // namespace engine