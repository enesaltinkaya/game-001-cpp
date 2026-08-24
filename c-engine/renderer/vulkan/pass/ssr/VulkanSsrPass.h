#pragma once

#include "ecs/system/System.h"

namespace engine {
class VulkanSsrPass : public System {
public:
    VulkanSsrPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern VulkanSsrPass vulkanSsrPass;

void  vulkanSsrPassSetDisabled(char disabled);
char  vulkanSsrPassIsDisabled(void);
}  // namespace engine
