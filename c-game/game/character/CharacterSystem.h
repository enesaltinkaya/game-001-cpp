#pragma once

#include "ecs/system/System.h"
#include "ecs/system/scene/Scene.h"  // IWYU pragma: keep

namespace game {
class CharacterSystem : public engine::System {
public:
    CharacterSystem();
    void added() override;
    void removed() override;
    void update() override;
};

extern CharacterSystem characterSystem;

void applyDamage(engine::Entity* target, float amount, u32 damageType);
void applyHeal(engine::Entity* target, float amount);
void gainXP(engine::Entity* target, float amount);
void triggerDeath(engine::Entity* target);
}  // namespace game
