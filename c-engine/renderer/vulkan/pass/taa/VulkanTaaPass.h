#pragma once

#include "ecs/system/System.h"

namespace engine {
struct VulkanImage;

class VulkanTaaPass : public System {
public:
    VulkanTaaPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern VulkanTaaPass vulkanTaaPass;

struct VulkanImage* vulkanTaaPassGetOutput(void);
char vulkanTaaPassIsEnabled(void);
}  // namespace engine
