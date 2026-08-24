#pragma once

#include "ecs/system/System.h"
#include "ecs/system/scene/Scene.h"  // IWYU pragma: keep

namespace game {
class CombatSystem : public engine::System {
public:
    CombatSystem();
    void added() override;
    void removed() override;
    void update() override;
};

extern CombatSystem combatSystem;

engine::Entity* combatCreateHitbox(engine::Entity* parent, float radius, float damage, u32 damageType, u32 ownerEntityId, u8 once, float hitCooldown);

void combatSetPlayerEntity(engine::Scene* scene, u32 entityId);

engine::Entity* combatGetPlayerEntity(void);
}  // namespace game
