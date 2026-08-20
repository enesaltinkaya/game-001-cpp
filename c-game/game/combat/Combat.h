#pragma once

#include "ecs/system/System.h"

struct Entity;
struct Scene;

extern struct System combatSystem;

Entity* combatCreateHitbox(Entity* parent, float radius, float damage, u32 damageType, u32 ownerEntityId, u8 once, float hitCooldown);

void combatSetPlayerEntity(Scene* scene, u32 entityId);

Entity* combatGetPlayerEntity(void);
