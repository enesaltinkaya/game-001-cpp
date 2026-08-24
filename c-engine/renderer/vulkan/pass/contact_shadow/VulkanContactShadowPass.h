#pragma once

#include "ecs/system/System.h"

namespace engine {
class VulkanContactShadowPass : public System {
public:
    VulkanContactShadowPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
};

extern VulkanContactShadowPass vulkanContactShadowPass;

void  vulkanContactShadowPassSetDisabled(char disabled);
char  vulkanContactShadowPassIsDisabled(void);
void  vulkanContactShadowPassSetLength(float length);
float vulkanContactShadowPassGetLength(void);
void  vulkanContactShadowPassSetThickness(float thickness);
float vulkanContactShadowPassGetThickness(void);
}  // namespace engine
