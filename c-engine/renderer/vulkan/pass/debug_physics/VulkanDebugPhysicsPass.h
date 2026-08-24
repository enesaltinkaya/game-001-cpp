#pragma once

#include "ecs/system/System.h"

typedef struct JoltCharacter JoltCharacter;

namespace engine {
class VulkanDebugPhysicsPass : public System {
public:
    VulkanDebugPhysicsPass();
    void added() override;
    void removed() override;
    void update() override;
};

extern VulkanDebugPhysicsPass vulkanDebugPhysicsPass;

/// Toggle physics debug visualization on/off.
void vulkanDebugPhysicsSetEnabled(char enabled);
char vulkanDebugPhysicsIsEnabled(void);

/// Register a JoltCharacter for debug visualization.
/// Up to 64 characters can be registered. Pass NULL to unregister.
void vulkanDebugPhysicsRegisterCharacter(JoltCharacter* character);
void vulkanDebugPhysicsUnregisterCharacter(JoltCharacter* character);
}  // namespace engine
