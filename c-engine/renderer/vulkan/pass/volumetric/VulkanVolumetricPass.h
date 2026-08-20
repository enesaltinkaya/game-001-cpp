#pragma once

#include "ecs/system/System.h"
#include "renderer/vulkan/resources/VulkanImage.h"

namespace engine {
struct VulkanCommand;

class VulkanVolumetricPass : public System {
public:
    VulkanVolumetricPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern VulkanVolumetricPass vulkanVolumetricPass;

void  vulkanVolumetricPassSetDisabled(char disabled);
char  vulkanVolumetricPassIsDisabled(void);
VulkanImage* vulkanVolumetricPassGetOutput(void);
}  // namespace engine
