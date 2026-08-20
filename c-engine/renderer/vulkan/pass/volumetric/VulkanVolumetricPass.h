#pragma once

#include "ecs/system/System.h"
#include "renderer/vulkan/resources/VulkanImage.h"

struct VulkanCommand;

extern System vulkanVolumetricPass;

void  vulkanVolumetricPassSetDisabled(char disabled);
char  vulkanVolumetricPassIsDisabled(void);
VulkanImage* vulkanVolumetricPassGetOutput(void);
