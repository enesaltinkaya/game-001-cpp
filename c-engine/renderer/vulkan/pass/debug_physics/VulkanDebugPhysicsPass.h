#pragma once

#include "ecs/system/System.h"

typedef struct JoltCharacter JoltCharacter;

extern System vulkanDebugPhysicsPass;

/// Toggle physics debug visualization on/off.
void vulkanDebugPhysicsSetEnabled(char enabled);
char vulkanDebugPhysicsIsEnabled(void);

/// Register a JoltCharacter for debug visualization.
/// Up to 64 characters can be registered. Pass NULL to unregister.
void vulkanDebugPhysicsRegisterCharacter(JoltCharacter* character);
void vulkanDebugPhysicsUnregisterCharacter(JoltCharacter* character);
