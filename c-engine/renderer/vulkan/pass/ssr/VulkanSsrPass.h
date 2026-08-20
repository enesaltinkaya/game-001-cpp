#pragma once

#include "ecs/system/System.h"

extern System vulkanSsrPass;

void  vulkanSsrPassSetDisabled(char disabled);
char  vulkanSsrPassIsDisabled(void);
