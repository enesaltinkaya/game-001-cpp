#pragma once

#include "ecs/system/System.h"

namespace engine {
class VulkanLightCullingPass : public System {
public:
    VulkanLightCullingPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
};

extern VulkanLightCullingPass vulkanLightCullingPass;
}  // namespace engine
