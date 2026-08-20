#pragma once

#include "ecs/system/System.h"

namespace engine {
class VulkanOcclusionPass : public System {
public:
    VulkanOcclusionPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern VulkanOcclusionPass vulkanOcclusionPass;
}  // namespace engine
