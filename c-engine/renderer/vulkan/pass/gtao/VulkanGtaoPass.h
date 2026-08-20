#pragma once

#include "ecs/system/System.h"

namespace engine {
class VulkanGtaoPass : public System {
public:
    VulkanGtaoPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
};

extern VulkanGtaoPass vulkanGtaoPass;

void  vulkanGtaoPassSetDisabled(char disabled);
char  vulkanGtaoPassIsDisabled(void);
void  vulkanGtaoPassSetStrength(float strength);
float vulkanGtaoPassGetStrength(void);
}  // namespace engine
