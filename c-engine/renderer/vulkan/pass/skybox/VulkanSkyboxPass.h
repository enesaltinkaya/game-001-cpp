#pragma once

#include "ecs/system/System.h"

namespace engine {
class VulkanSkyboxPass : public System {
public:
    VulkanSkyboxPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern VulkanSkyboxPass vulkanSkyboxPass;
}  // namespace engine
