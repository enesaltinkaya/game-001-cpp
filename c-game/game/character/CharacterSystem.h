#pragma once

#include "ecs/system/System.h"

struct Entity;

extern struct System characterSystem;

void applyDamage(Entity* target, float amount, u32 damageType);
void applyHeal(Entity* target, float amount);
void gainXP(Entity* target, float amount);
void triggerDeath(Entity* target);
