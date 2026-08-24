#pragma once

#include "ecs/system/System.h"

namespace engine {
class VulkanOitCompositePass : public System {
public:
    VulkanOitCompositePass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern VulkanOitCompositePass vulkanOitCompositePass;
}  // namespace engine
