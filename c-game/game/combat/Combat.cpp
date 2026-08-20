#include "combat/Combat.h"
#include "combat/AttackHitbox.h"
#include "character/CharacterSystem.h"
#include "character/CharacterStats.h"
#include "enemy/Enemy.h"

#include "ecs/Ecs.h"
#include "ecs/system/scene/SceneSystem.h"
#include "ecs/system/transform/TransformComponent.h"
#include "ecs/system/physics/PhysicsSystem.h"
#include "container/Array.h"
#include "timer/Timer.h"

static void added(void);
static void removed(void);
static void update(void);

struct System combatSystem = {
    .name     = "combat",
    .added    = added,
    .removed  = removed,
    .update   = update,
    .priority = 1500,
};

static Scene* gPlayerScene;
static u32    gPlayerEntityId;

static Entity* findEntityById(u32 entityId) {
    u32 numScenes = arraySize(ecs.scenes);
    for (u32 si = 0; si < numScenes; si++) {
        Scene* scene = ecs.scenes[si];
        Entity* e = getEntity(scene, entityId);
        if (e) return e;
    }
    return NULL;
}

static u8 canHit(AttackHitbox* hitbox, u32 entityId);
static void markHit(AttackHitbox* hitbox, u32 entityId);

/// Try to apply damage for one overlap hit across all scenes.
/// Entity IDs are globally unique, so ID collisions across scenes are
/// not possible.  We skip the owner (self) and damage the first entity
/// with CharacterStats.
static void processHit(AttackHitbox* hb, const JoltOverlapHit* hit, vec3 center) {
    u32 hitEntityId = (u32)hit->userData;
    if (hitEntityId == 0) return;

    u32 numScenes = arraySize(ecs.scenes);
    for (u32 si = 0; si < numScenes; si++) {
        Scene* hitScene = ecs.scenes[si];
        Entity* target = getEntity(hitScene, hitEntityId);
        if (!target) continue;
        if (target->id == hb->ownerEntityId && target->scene == hb->ownerScene) continue;
        if (!getComponent(hitScene, CharacterStats, hitEntityId)) continue;
        // No friendly fire: if owner is an enemy, skip other enemies
        if (getComponent(hb->ownerScene, Enemy, hb->ownerEntityId) &&
            getComponent(hitScene, Enemy, hitEntityId)) continue;
        if (!canHit(hb, hitEntityId)) continue;

        debug("combat: hit entity '%s' (id %u) with %.1f damage at (%.1f,%.1f,%.1f)",
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
            if (timer.timeSinceStartSeconds - hitbox->recentHits[i].hitTime < hitbox->hitCooldown) {
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
            hitbox->recentHits[i].hitTime = (float)timer.timeSinceStartSeconds;
            return;
        }
    }
    // Add new entry
    if (hitbox->recentHitCount < ATTACK_HITBOX_MAX_RECENT) {
        hitbox->recentHits[hitbox->recentHitCount].entityId = entityId;
        hitbox->recentHits[hitbox->recentHitCount].hitTime = (float)timer.timeSinceStartSeconds;
        hitbox->recentHitCount++;
    }
}

void update(void) {
    u32 numScenes = arraySize(ecs.scenes);
    for (u32 si = 0; si < numScenes; si++) {
        Scene* scene = ecs.scenes[si];
        if (!scene || !scene->ready) continue;

        SparseSet* hitboxes = getComponents(scene, AttackHitbox);
        if (!hitboxes || hitboxes->size == 0) continue;

        for (u32 i = 0; i < hitboxes->size; i++) {
            u32 entityId = ssGetValueByIndex(hitboxes, i);
            AttackHitbox* hb = (AttackHitbox*)ssGetDataByIndex(hitboxes, i);
            if (!hb) continue;

            Transform* t = getComponent(scene, Transform, entityId);
            if (!t) continue;

            vec3 center;
            glm_vec3_copy(t->pos, center);

            JoltOverlapHit hits[16];
            u32 hitCount = joltSphereOverlap(center, hb->radius, hits, 16);
            if (hitCount > 0) {
                debug("combat: hitbox at (%.1f,%.1f,%.1f) radius %.2f found %u overlaps", center[0], center[1], center[2], hb->radius, hitCount);
            }

            for (u32 h = 0; h < hitCount; h++) {
                debug("combat: overlap[%u] userData=%lu bodyId=%u", h, (unsigned long)hits[h].userData, hits[h].bodyId);
                processHit(hb, &hits[h], center);
            }

            if (hb->once) {
                F_sceneRemoveComponent(scene, entityId, &AttackHitbox_id);
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

Entity* combatCreateHitbox(Entity* parent, float radius, float damage, u32 damageType, u32 ownerEntityId, u8 once, float hitCooldown) {
    if (!parent) return NULL;

    Scene* scene = parent->scene;
    Entity* entity = createEntity(scene, "hitbox");

    Transform* parentT = getComponent(scene, Transform, parent->id);
    Transform* t = createComponent(scene, Transform, entity->id);
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

void combatSetPlayerEntity(Scene* scene, u32 entityId) {
    gPlayerScene = scene;
    gPlayerEntityId = entityId;
}

Entity* combatGetPlayerEntity(void) {
    if (!gPlayerScene) return NULL;
    return getEntity(gPlayerScene, gPlayerEntityId);
}

void added(void) {
}

void removed(void) {
    gPlayerScene = NULL;
    gPlayerEntityId = 0;
}
