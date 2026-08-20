#pragma once

#include "ecs/system/System.h"

extern System vulkanGtaoPass;

void  vulkanGtaoPassSetDisabled(char disabled);
char  vulkanGtaoPassIsDisabled(void);
void  vulkanGtaoPassSetStrength(float strength);
float vulkanGtaoPassGetStrength(void);
