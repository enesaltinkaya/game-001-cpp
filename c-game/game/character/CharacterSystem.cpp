#include "character/CharacterSystem.h"
#include "character/CharacterStats.h"
#include "enemy/Enemy.h"
#include "hud/Hud.h"

#include "ecs/Ecs.h"
#include "ecs/system/scene/SceneSystem.h"
#include "ecs/system/animation/AnimatorComponent.h"
#include "ecs/system/transform/TransformComponent.h"
#include "ecs/system/sound/SoundSystem.h"
#include "Utils.h"
#include "settings/Settings.h"
#include "timer/Timer.h"

static void added(void);
static void removed(void);
static void update(void);

System characterSystem = {
    .name                = "character",
    .added               = added,
    .removed             = removed,
    .preUpdate           = nullptr,
    .update              = update,
    .postUpdate          = nullptr,
    .cpuElapsedLastFrame = 0.0,
    .cpuElapsed          = 0.0,
    .gpuElapsed          = 0.0,
    .priority            = 1001,
};

static Sound* combatHitSound;
static Sound* combatDeathSound1;
static Sound* combatDeathSound2;

static void characterNumberSpawn(Entity* target, float amount, u32 damageType, vec3 position);
static void combatAudioPlayHit(u32 damageType);
static void combatAudioPlayDeath(void);

// Forward reference to Enemy_id (defined in enemy/Enemy.h when compiled).
// Weak link: resolves to 0 if Enemy component is not yet in the build.
void applyDamage(Entity* target, float amount, u32 damageType) {
    if (!target) return;

    CharacterStats* stats = getComponent(target->scene, CharacterStats, target->id);
    if (!stats || stats->isDead) return;

    float resistance = 0.0f;
    if (damageType >= DAMAGE_TYPE_FIRE && damageType <= DAMAGE_TYPE_LIGHTNING) {
        u32 idx = static_cast<u32>(damageType - DAMAGE_TYPE_FIRE);
        resistance = stats->elementalResist[idx];
    }

    float actualDamage = amount * (1.0f - resistance);
    if (actualDamage < 0.0f) actualDamage = 0.0f;

    stats->hp -= actualDamage;
    if (stats->hp < 0.0f) stats->hp = 0.0f;

    Transform* transform = getComponent(target->scene, Transform, target->id);
    vec3 hitPosition = {0.0f, 0.0f, 0.0f};
    if (transform) {
        hitPosition[0] = transform->pos[0];
        hitPosition[1] = transform->pos[1] + 1.0f;
        hitPosition[2] = transform->pos[2];
    }

    characterNumberSpawn(target, actualDamage, damageType, hitPosition);
    combatAudioPlayHit(damageType);

    if (stats->hp <= 0.0f) {
        triggerDeath(target);
    }
}

void applyHeal(Entity* target, float amount) {
    if (!target) return;

    CharacterStats* stats = getComponent(target->scene, CharacterStats, target->id);
    if (!stats || stats->isDead) return;

    stats->hp += amount;
    if (stats->hp > stats->maxHp) stats->hp = stats->maxHp;
}

void gainXP(Entity* target, float amount) {
    if (!target) return;

    CharacterStats* stats = getComponent(target->scene, CharacterStats, target->id);
    if (!stats || stats->isDead) return;

    stats->xp += amount;

    while (stats->xp >= stats->xpToNext) {
        stats->xp -= stats->xpToNext;
        stats->level++;
        stats->xpToNext *= 1.5f;

        stats->hp = stats->maxHp;
        stats->mana = stats->maxMana;
    }
}

void triggerDeath(Entity* target) {
    if (!target) return;

    CharacterStats* stats = getComponent(target->scene, CharacterStats, target->id);
    if (!stats || stats->isDead) return;

    stats->isDead = 1;

    animationPlayBlended(target, "death", 1.0f, false, 0.1f);

    {
        Enemy* enemy = getComponent(target->scene, Enemy, target->id);
        if (enemy) {
            enemy->state = ENEMY_STATE_DEAD;
            enemy->stateTimer = 0.0f;
            enemy->deathAnimPlayed = 1;
        }
    }

    combatAudioPlayDeath();
}

static void characterNumberSpawn(Entity* target, float amount, u32 damageType, vec3 position) {
    static_cast<void>(damageType);
    // Positive = damage dealt to enemy (yellow), negative = damage taken by player (red)
    Enemy* enemy = getComponent(target->scene, Enemy, target->id);
    float signedAmount = enemy ? amount : -amount;
    hudDamageNumber(position[0], position[1], position[2], signedAmount);
}

static void combatAudioPlayHit(u32 damageType) {
    static_cast<void>(damageType);
    if (!combatHitSound) return;
    soundPlay(combatHitSound, settingsGetDouble("effects") / 100.0f, 0);
}

static void combatAudioPlayDeath(void) {
    Sound* sound = randomU32() % 2 == 0 ? combatDeathSound1 : combatDeathSound2;
    if (!sound) return;
    soundPlay(sound, settingsGetDouble("effects") / 100.0f, 0);
}

void added(void) {
    combatHitSound      = soundLoad("sound/player/hit.ogg");
    combatDeathSound1   = soundLoad("sound/effects/death1.ogg");
    combatDeathSound2   = soundLoad("sound/effects/death2.ogg");
}

void removed(void) {
    soundDestroy(combatHitSound);
    soundDestroy(combatDeathSound1);
    soundDestroy(combatDeathSound2);
    combatHitSound      = nullptr;
    combatDeathSound1   = nullptr;
    combatDeathSound2   = nullptr;
}

void update(void) {
}
