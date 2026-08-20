#pragma once

#include "ecs/system/System.h"

namespace engine {
class VulkanOitAccumulatePass : public System {
public:
    VulkanOitAccumulatePass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern VulkanOitAccumulatePass vulkanOitAccumulatePass;
}  // namespace engine
