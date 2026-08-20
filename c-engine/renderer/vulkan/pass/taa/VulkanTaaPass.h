#pragma once

#include "ecs/system/System.h"

struct VulkanImage;

extern System vulkanTaaPass;

struct VulkanImage* vulkanTaaPassGetOutput(void);
char vulkanTaaPassIsEnabled(void);
