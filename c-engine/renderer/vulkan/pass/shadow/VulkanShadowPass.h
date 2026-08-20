#pragma once

#include "ecs/system/System.h"

namespace engine {
class VulkanShadowPass : public System {
public:
    VulkanShadowPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
};

extern VulkanShadowPass vulkanShadowPass;

void vulkanShadowPassSetDisabled(char disabled);
char vulkanShadowPassIsDisabled(void);

void vulkanShadowPassSetPCSS(char enabled);
char vulkanShadowPassIsPCSS(void);

void vulkanShadowPassSetFocusDistance(float distance);
}  // namespace engine
