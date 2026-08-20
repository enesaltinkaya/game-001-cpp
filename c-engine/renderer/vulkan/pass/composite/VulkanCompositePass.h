#pragma once

#include "ecs/system/System.h"

namespace engine {
class VulkanCompositePass : public System {
public:
    VulkanCompositePass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern VulkanCompositePass vulkanCompositePass;
}  // namespace engine
