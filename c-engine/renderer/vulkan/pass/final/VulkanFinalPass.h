#pragma once

#include "ecs/system/System.h"

namespace engine {
class VulkanFinalPass : public System {
public:
    VulkanFinalPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern VulkanFinalPass vulkanFinalPass;
}  // namespace engine
