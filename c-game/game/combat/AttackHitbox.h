#pragma once

#include "ecs/system/scene/SceneSystem.h"

#define ATTACK_HITBOX_MAX_RECENT 16

struct RecentHit {
    u32  entityId;
    float hitTime;  // absolute time of last hit (used for cooldown check)
};

struct AttackHitbox {
    float  radius;
    float  damage;
    u32    damageType;
    u32    ownerEntityId;  // skip damaging the owner (self)
    Scene* ownerScene;     // scene of the owner entity
    u8     once;
    float  hitCooldown;    // seconds before same entity can be hit again (0 = never again)
    u32    recentHitCount;
    RecentHit recentHits[ATTACK_HITBOX_MAX_RECENT];
};

REGISTER_COMPONENT(AttackHitbox);
