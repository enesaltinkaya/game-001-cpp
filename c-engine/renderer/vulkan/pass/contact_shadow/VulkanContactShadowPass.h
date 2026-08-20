#pragma once

#include "ecs/system/System.h"

extern System vulkanContactShadowPass;

void  vulkanContactShadowPassSetDisabled(char disabled);
char  vulkanContactShadowPassIsDisabled(void);
void  vulkanContactShadowPassSetLength(float length);
float vulkanContactShadowPassGetLength(void);
void  vulkanContactShadowPassSetThickness(float thickness);
float vulkanContactShadowPassGetThickness(void);
