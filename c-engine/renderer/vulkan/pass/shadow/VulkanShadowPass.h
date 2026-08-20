#pragma once

#include "ecs/system/System.h"

extern System vulkanShadowPass;

void vulkanShadowPassSetDisabled(char disabled);
char vulkanShadowPassIsDisabled(void);

void vulkanShadowPassSetPCSS(char enabled);
char vulkanShadowPassIsPCSS(void);

void vulkanShadowPassSetFocusDistance(float distance);
