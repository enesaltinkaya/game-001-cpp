#include "Combat.h"
#include "combat/Combat.h"
#include "combat/AttackHitbox.h"
#include "character/CharacterSystem.h"
#include "character/CharacterStats.h"
#include "enemy/Enemy.h"

#include "ecs/Ecs.h"
#include "ecs/system/scene/SceneSystem.h"
#include "ecs/system/transform/TransformComponent.h"
#include "ecs/system/physics/PhysicsSystem.h"
#include <vector>
#include "timer/Timer.h"

namespace game {

CombatSystem combatSystem;

CombatSystem::CombatSystem() : engine::System("combat") {}

static engine::Scene* gPlayerScene;
static u32    gPlayerEntityId;

static engine::Entity* findEntityById(u32 entityId) {
    u32 numScenes = static_cast<i32>(engine::ecs.scenes.size());
    for (u32 si = 0; si < numScenes; si++) {
        engine::Scene* scene = engine::ecs.scenes[si];
        engine::Entity* e = engine::getEntity(scene, entityId);
        if (e) return e;
    }
    return nullptr;
}

static u8 canHit(AttackHitbox* hitbox, u32 entityId);
static void markHit(AttackHitbox* hitbox, u32 entityId);

/// Try to apply damage for one overlap hit across all scenes.
/// Entity IDs are globally unique, so ID collisions across scenes are
/// not possible.  We skip the owner (self) and damage the first entity
/// with CharacterStats.
static void processHit(AttackHitbox* hb, const JoltOverlapHit* hit, vec3 center) {
    u32 hitEntityId = static_cast<u32>(hit->userData);
    if (hitEntityId == 0) return;

    u32 numScenes = static_cast<i32>(engine::ecs.scenes.size());
    for (u32 si = 0; si < numScenes; si++) {
        engine::Scene* hitScene = engine::ecs.scenes[si];
        engine::Entity* target = engine::getEntity(hitScene, hitEntityId);
        if (!target) continue;
        if (target->id == hb->ownerEntityId && target->scene == hb->ownerScene) continue;
        if (!getComponent(hitScene, CharacterStats, hitEntityId)) continue;
        // No friendly fire: if owner is an enemy, skip other enemies
        if (getComponent(hb->ownerScene, Enemy, hb->ownerEntityId) &&
            getComponent(hitScene, Enemy, hitEntityId)) continue;
        if (!canHit(hb, hitEntityId)) continue;

        utils::debug("combat: hit entity '%s' (id %u) with %.1f damage at (%.1f,%.1f,%.1f)",
              target->name, target->id, hb->damage,
              center[0], center[1], center[2]);
        applyDamage(target, hb->damage, hb->damageType);
        markHit(hb, hitEntityId);
        return;  // one damage per overlap
    }
}

static u8 canHit(AttackHitbox* hitbox, u32 entityId) {
    for (u32 i = 0; i < hitbox->recentHitCount; i++) {
        if (hitbox->recentHits[i].entityId == entityId) {
            // cooldown == 0 means "never again" (projectiles like fireball)
            if (hitbox->hitCooldown == 0.0f) return 0;
            if (utils::timer.timeSinceStartSeconds - hitbox->recentHits[i].hitTime < hitbox->hitCooldown) {
                return 0;
            }
            // cooldown expired — allow hit (time updated in markHit)
            return 1;
        }
    }
    return 1;
}

static void markHit(AttackHitbox* hitbox, u32 entityId) {
    // Update existing entry first
    for (u32 i = 0; i < hitbox->recentHitCount; i++) {
        if (hitbox->recentHits[i].entityId == entityId) {
            hitbox->recentHits[i].hitTime = static_cast<float>(utils::timer.timeSinceStartSeconds);
            return;
        }
    }
    // Add new entry
    if (hitbox->recentHitCount < ATTACK_HITBOX_MAX_RECENT) {
        hitbox->recentHits[hitbox->recentHitCount].entityId = entityId;
        hitbox->recentHits[hitbox->recentHitCount].hitTime = static_cast<float>(utils::timer.timeSinceStartSeconds);
        hitbox->recentHitCount++;
    }
}

void CombatSystem::update() {
    u32 numScenes = static_cast<i32>(engine::ecs.scenes.size());
    for (u32 si = 0; si < numScenes; si++) {
        engine::Scene* scene = engine::ecs.scenes[si];
        if (!scene || !scene->ready) continue;

        utils::SparseSet* hitboxes = getComponents(scene, AttackHitbox);
        if (!hitboxes || hitboxes->size == 0) continue;

        for (u32 i = 0; i < hitboxes->size; i++) {
            u32 entityId = utils::ssGetValueByIndex(hitboxes, i);
            AttackHitbox* hb = (AttackHitbox*)utils::ssGetDataByIndex(hitboxes, i);
            if (!hb) continue;

            engine::Transform* t = getComponent(scene, engine::Transform, entityId);
            if (!t) continue;

            vec3 center;
            glm_vec3_copy(t->pos, center);

            JoltOverlapHit hits[16];
            u32 hitCount = joltSphereOverlap(center, hb->radius, hits, 16);
            if (hitCount > 0) {
                utils::debug("combat: hitbox at (%.1f,%.1f,%.1f) radius %.2f found %u overlaps", center[0], center[1], center[2], hb->radius, hitCount);
            }

            for (u32 h = 0; h < hitCount; h++) {
                utils::debug("combat: overlap[%u] userData=%lu bodyId=%u", h, (unsigned long)hits[h].userData, hits[h].bodyId);
                processHit(hb, &hits[h], center);
            }

            if (hb->once) {
                engine::F_sceneRemoveComponent(scene, entityId, &AttackHitbox_id);
            }

            // Clean up entries that no longer exist (entity destroyed)
            {
                u32 valid = 0;
                for (u32 r = 0; r < hb->recentHitCount; r++) {
                    if (findEntityById(hb->recentHits[r].entityId)) {
                        hb->recentHits[valid++] = hb->recentHits[r];
                    }
                }
                hb->recentHitCount = valid;
            }
        }
    }
}

engine::Entity* combatCreateHitbox(engine::Entity* parent, float radius, float damage, u32 damageType, u32 ownerEntityId, u8 once, float hitCooldown) {
    if (!parent) return nullptr;

    engine::Scene* scene = parent->scene;
    engine::Entity* entity = engine::createEntity(scene, "hitbox");

    engine::Transform* parentT = getComponent(scene, engine::Transform, parent->id);
    engine::Transform* t = createComponent(scene, engine::Transform, entity->id);
    if (parentT) {
        glm_vec3_copy(parentT->pos, t->pos);
        glm_quat_copy(parentT->rot, t->rot);
    }
    t->pos[3] = 1.0f;

    AttackHitbox* hb = createComponent(scene, AttackHitbox, entity->id);
    hb->radius = radius;
    hb->damage = damage;
    hb->damageType = damageType;
    hb->ownerEntityId = ownerEntityId;
    hb->ownerScene     = parent->scene;
    hb->once          = once;
    hb->hitCooldown   = hitCooldown;
    hb->recentHitCount = 0;

    entity->parent = parent;

    return entity;
}

void combatSetPlayerEntity(engine::Scene* scene, u32 entityId) {
    gPlayerScene = scene;
    gPlayerEntityId = entityId;
}

engine::Entity* combatGetPlayerEntity(void) {
    if (!gPlayerScene) return nullptr;
    return engine::getEntity(gPlayerScene, gPlayerEntityId);
}

void CombatSystem::added() {
}

void CombatSystem::removed() {
    gPlayerScene = nullptr;
    gPlayerEntityId = 0;
}
}  // namespace game
