#pragma once

#include "ecs/system/scene/SceneSystem.h"

namespace game {
enum DamageType {
    DAMAGE_TYPE_PHYSICAL = 0,
    DAMAGE_TYPE_FIRE,
    DAMAGE_TYPE_COLD,
    DAMAGE_TYPE_LIGHTNING,
};

struct CharacterStats {
    float  hp;
    float  maxHp;
    float  mana;
    float  maxMana;
    float  moveSpeed;
    float  attackSpeed;
    float  damage;
    float  armor;
    float  elementalResist[3];
    float  xp;
    float  xpToNext;
    u32    level;
    u8     isDead;
};

REGISTER_COMPONENT(CharacterStats);
}  // namespace game
