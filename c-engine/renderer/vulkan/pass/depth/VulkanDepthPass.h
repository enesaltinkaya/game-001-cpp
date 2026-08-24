#pragma once

#include "ecs/system/System.h"

namespace engine {
class VulkanDepthPass : public System {
public:
    VulkanDepthPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern VulkanDepthPass vulkanDepthPass;
}  // namespace engine
