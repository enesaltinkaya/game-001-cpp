#pragma once

#include "ecs/system/System.h"

namespace engine {
class VulkanScenePass : public System {
public:
    VulkanScenePass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern VulkanScenePass vulkanScenePass;
}  // namespace engine
