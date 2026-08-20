#pragma once

#include "ecs/system/System.h"

struct VulkanImage;

extern System vulkanFsrPass;

struct VulkanImage* vulkanFsrPassGetOutput(void);
char vulkanFsrPassIsEnabled(void);
void vulkanFsrPassSetReactiveMask(char enabled);
char vulkanFsrPassGetReactiveMask(void);
void vulkanFsrPassSetSharpness(float sharpness);
float vulkanFsrPassGetSharpness(void);
