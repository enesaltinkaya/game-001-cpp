#pragma once

#include "ecs/system/System.h"

namespace engine {
class VulkanCullingPass : public System {
public:
    VulkanCullingPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern VulkanCullingPass vulkanCullingPass;
}  // namespace engine
