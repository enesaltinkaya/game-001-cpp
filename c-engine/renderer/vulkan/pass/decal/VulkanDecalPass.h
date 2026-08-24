#pragma once
#include "ecs/system/System.h"

namespace engine {
class VulkanDecalPass : public System {
public:
    VulkanDecalPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern VulkanDecalPass vulkanDecalPass;
}  // namespace engine
