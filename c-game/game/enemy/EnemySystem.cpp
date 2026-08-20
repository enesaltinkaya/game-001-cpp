#include "enemy/EnemySystem.h"
#include "enemy/Enemy.h"
#include "combat/Combat.h"
#include "character/CharacterSystem.h"
#include "character/CharacterStats.h"
#include "player/Player.h"
#include "renderer/vulkan/pass/debug_physics/VulkanDebugPhysicsPass.h"
#include "navmesh/NavMeshSystem.h"

#include "ecs/Ecs.h"
#include "ecs/system/scene/SceneSystem.h"
#include "ecs/system/animation/AnimatorComponent.h"
#include "ecs/system/mesh/MeshComponent.h"
#include "ecs/system/transform/TransformComponent.h"
#include "ecs/system/transform/TransformSystem.h"
#include "renderer/Renderer.h"
#include "timer/Timer.h"
#include <string.h>

#define ENEMY_INSTANCES_PER_MODEL 0

// Animation names (will match actual clips when available)
#define ENEMY_ANIM_IDLE "idle"
#define ENEMY_ANIM_ALERT "alert"
#define ENEMY_ANIM_CHASE "run"
#define ENEMY_ANIM_ATTACK "attack"
#define ENEMY_ANIM_RETREAT "run"
#define ENEMY_ANIM_DEAD "death"

// Timing constants
#define ENEMY_ALERT_DURATION 0.5f
#define ENEMY_TARGET_POS_TTL 3.0f
#define ENEMY_PATROL_WAIT_MIN 2.0f
#define ENEMY_PATROL_WAIT_MAX 5.0f
#define ENEMY_ATTACK_ANIM_DUR 0.8f
#define ENEMY_RETREAT_HP_MARGIN 0.15f  // extra buffer on retreat threshold
#define ENEMY_TURN_SPEED 8.0f
#define ENEMY_DEATH_ROT_DURATION 0.4f  // time to rotate onto ground

static void added(void);
static void removed(void);
static void update(void);

struct System enemySystem = {
    .name     = "enemy",
    .added    = added,
    .removed  = removed,
    .update   = update,
    .priority = 1200,
};

// ── State transition helpers ────────────────────────────────────────────────

static void enemySetState(Enemy* enemy, Entity* entity, EnemyState newState) {
    (void)entity;
    if (enemy->state == newState) return;

    enemy->state           = newState;
    enemy->stateTimer      = 0.0f;
    enemy->alertAnimPlayed = 0;

    // Play animation for new state
    switch (newState) {
        case ENEMY_STATE_IDLE:
            // animationPlayBlended(entity, ENEMY_ANIM_IDLE, 1.0f, true, 0.15f);
            break;
        case ENEMY_STATE_ALERT:
            // animationPlayBlended(entity, ENEMY_ANIM_ALERT, 1.0f, false, 0.1f);
            enemy->alertAnimPlayed = 1;
            break;
        case ENEMY_STATE_CHASE:
            // animationPlayBlended(entity, ENEMY_ANIM_CHASE, 1.2f, true, 0.1f);
            break;
        case ENEMY_STATE_ATTACK:
            // animationPlayBlended(entity, ENEMY_ANIM_ATTACK, 1.0f, false, 0.05f);
            break;
        case ENEMY_STATE_RETREAT:
            // animationPlayBlended(entity, ENEMY_ANIM_RETREAT, 1.5f, true, 0.1f);
            break;
        case ENEMY_STATE_DEAD:
            // animationPlayBlended(entity, ENEMY_ANIM_DEAD, 1.0f, false, 0.1f);
            enemy->deathAnimPlayed = 1;
            break;
    }
}

// ── Target detection ────────────────────────────────────────────────────────

static Entity* enemyGetTarget(Enemy* enemy) {
    if (!enemy->targetScene || !enemy->targetEntityId) return NULL;
    return getEntity(enemy->targetScene, enemy->targetEntityId);
}

static u8 enemyIsPlayerInRange(Enemy* enemy, vec3 enemyPos) {
    Entity* player = combatGetPlayerEntity();
    if (!player) return 0;

    Transform* pT = getComponent(player->scene, Transform, player->id);
    if (!pT) return 0;

    vec3 dir;
    glm_vec3_sub(pT->pos, enemyPos, dir);
    dir[1]       = 0.0f;  // ignore height difference
    float distSq = glm_vec3_norm2(dir);

    return distSq < enemy->aggroRange * enemy->aggroRange;
}

static u8 enemyIsPlayerInAttackRange(Enemy* enemy, vec3 enemyPos) {
    Entity* player = combatGetPlayerEntity();
    if (!player) return 0;

    Transform* pT = getComponent(player->scene, Transform, player->id);
    if (!pT) return 0;

    vec3 dir;
    glm_vec3_sub(pT->pos, enemyPos, dir);
    dir[1]       = 0.0f;
    float distSq = glm_vec3_norm2(dir);

    return distSq < enemy->attackRange * enemy->attackRange;
}

static u8 enemyIsFarEnoughToRegain(Enemy* enemy, vec3 enemyPos) {
    Entity* player = combatGetPlayerEntity();
    if (!player) return 1;  // no player = safe

    Transform* pT = getComponent(player->scene, Transform, player->id);
    if (!pT) return 1;

    vec3 dir;
    glm_vec3_sub(pT->pos, enemyPos, dir);
    dir[1]       = 0.0f;
    float distSq = glm_vec3_norm2(dir);

    float regainDist = enemy->aggroRange + enemy->regainRange;
    return distSq > regainDist * regainDist;
}

// ── Rotation toward target ──────────────────────────────────────────────────

static void enemyRotateToward(vec3 enemyPos, vec3 targetPos, Enemy* enemy, Transform* transform) {
    vec3 dir;
    glm_vec3_sub(targetPos, enemyPos, dir);
    dir[1] = 0.0f;
    if (glm_vec3_norm2(dir) < 0.001f) return;
    glm_vec3_normalize(dir);

    float targetYaw = atan2f(dir[0], dir[2]);
    versor yawQuat;
    glm_quatv(yawQuat, targetYaw, (vec3){0.0f, 1.0f, 0.0f});

    versor targetRot;
    glm_quat_mul(yawQuat, enemy->baseRot, targetRot);

    float t = glm_clamp(enemy->turnSpeed * timer.dt, 0.0f, 1.0f);
    quatSlerpShortest(transform->rot, targetRot, t, transform->rot);
    glm_quat_normalize(transform->rot);
}

// ── Attack execution ────────────────────────────────────────────────────────

static void enemyAttack(Enemy* enemy, Entity* entity) {
    if (enemy->attackCooldown > 0.0f) return;

    enemy->attackCooldown = enemy->attackCooldownMax;

    // Spawn a one-shot hitbox at the enemy's position
    combatCreateHitbox(entity,
                       enemy->attackRange,
                       enemy->attackDamage,
                       enemy->attackDamageType,
                       entity->id,
                       1,
                       0.0f);
}

// ── Character-vs-character separation ────────────────────────────────────────
// Pushes desiredVel away from nearby enemies to prevent stacking.
// Called from each state handler before joltCharacterUpdate().

#define ENEMY_SEPARATION_RADIUS 1.2f
#define ENEMY_SEPARATION_STRENGTH 3.0f

static Scene* enemySepScene;
static u32 enemySepCount;
static vec3 enemySepPositions[256];  // scene-local cache

static void enemyApplySeparation(vec3 pos, vec3 desiredVel) {
    float radiusSq = ENEMY_SEPARATION_RADIUS * ENEMY_SEPARATION_RADIUS;
    for (u32 i = 0; i < enemySepCount && i < 256; i++) {
        vec3 dir;
        glm_vec3_sub(pos, enemySepPositions[i], dir);
        dir[1]       = 0.0f;
        float distSq = glm_vec3_norm2(dir);
        if (distSq < radiusSq && distSq > 0.0001f) {
            float dist = sqrtf(distSq);
            glm_vec3_normalize(dir);
            // Stronger push when closer; falls off to zero at the radius edge
            float weight = 1.0f - (dist / ENEMY_SEPARATION_RADIUS);
            weight *= weight;  // quadratic falloff
            vec3 sep;
            glm_vec3_scale(dir, weight * ENEMY_SEPARATION_STRENGTH, sep);
            glm_vec3_add(desiredVel, sep, desiredVel);
        }
    }
}

// ── State update functions ──────────────────────────────────────────────────

static void enemyStateIdle(Enemy* enemy, Entity* entity, Transform* transform, vec3 pos) {
    // Check for player aggro
    if (enemyIsPlayerInRange(enemy, pos)) {
        enemy->targetScene = NULL;  // will be set by combatGetPlayerEntity path
        Entity* player     = combatGetPlayerEntity();
        if (player) {
            enemy->targetScene           = player->scene;
            enemy->targetEntityId        = player->id;
            enemy->lastKnownTargetPos[0] = player->scene ? 0 : 0;
            Transform* pT                = getComponent(player->scene, Transform, player->id);
            if (pT) {
                enemy->lastKnownTargetPos[0] = pT->pos[0];
                enemy->lastKnownTargetPos[1] = pT->pos[1];
                enemy->lastKnownTargetPos[2] = pT->pos[2];
            }
            enemy->targetPosValidTimer = ENEMY_TARGET_POS_TTL;
        }
        enemySetState(enemy, entity, ENEMY_STATE_ALERT);
        return;
    }

    // Patrol behavior
    if (enemy->patrolPointCount == 0) return;

    vec3 targetPatrol;
    glm_vec3_copy(enemy->patrolPoints[enemy->currentPatrolIndex], targetPatrol);

    // Move toward patrol point using Jolt character
    if (enemy->character) {
        vec3 desiredVel = {0.0f, 0.0f, 0.0f};
        vec3 dir;
        glm_vec3_sub(targetPatrol, pos, dir);
        dir[1]       = 0.0f;
        float distSq = glm_vec3_norm2(dir);

        if (distSq > 1.0f) {
            glm_vec3_normalize(dir);
            glm_vec3_scale(dir, enemy->moveSpeed * 0.5f, desiredVel);  // half speed for patrol

            // Face patrol direction
            enemyRotateToward(pos, targetPatrol, enemy, transform);

            // Push away from nearby enemies
            enemyApplySeparation(pos, desiredVel);
        } else {
            // Reached patrol point, wait then advance
            enemy->patrolWaitTimer += timer.dt;
            if (enemy->patrolWaitTimer >= enemy->patrolWaitDuration) {
                enemy->patrolWaitTimer = 0.0f;
                enemy->currentPatrolIndex =
                    (enemy->currentPatrolIndex + 1) % enemy->patrolPointCount;
            }
            return;
        }

        joltCharacterUpdate(enemy->character, desiredVel, timer.dt);
        joltCharacterGetPosition(enemy->character, pos);
        transform->pos[0] = pos[0];
        transform->pos[1] = pos[1];
        transform->pos[2] = pos[2];
        if (enemy->sensorBody) joltBodySetPosition(enemy->sensorBody, pos);
    }
}

static void enemyStateAlert(Enemy* enemy, Entity* entity, Transform* transform, vec3 pos) {
    enemy->stateTimer += timer.dt;

    // Face player
    Entity* player = enemyGetTarget(enemy);
    if (player) {
        Transform* pT = getComponent(player->scene, Transform, player->id);
        if (pT) {
            enemyRotateToward(pos, pT->pos, enemy, transform);
            enemy->lastKnownTargetPos[0] = pT->pos[0];
            enemy->lastKnownTargetPos[1] = pT->pos[1];
            enemy->lastKnownTargetPos[2] = pT->pos[2];
            enemy->targetPosValidTimer   = ENEMY_TARGET_POS_TTL;
        }
    }

    // After alert duration, move to chase
    if (enemy->stateTimer >= ENEMY_ALERT_DURATION) {
        enemySetState(enemy, entity, ENEMY_STATE_CHASE);
    }
}

static void enemyStateChase(Enemy* enemy, Entity* entity, Transform* transform, vec3 pos) {
    enemy->stateTimer += timer.dt;

    // Check HP for retreat
    CharacterStats* stats = getComponent(entity->scene, CharacterStats, entity->id);
    if (stats && stats->hp / stats->maxHp < enemy->retreatThreshold) {
        enemySetState(enemy, entity, ENEMY_STATE_RETREAT);
        return;
    }

    // Only give up if player is very far away
    {
        Entity* player = combatGetPlayerEntity();
        if (player) {
            Transform* pT = getComponent(player->scene, Transform, player->id);
            if (pT) {
                vec3 dir;
                glm_vec3_sub(pT->pos, pos, dir);
                dir[1]       = 0.0f;
                float distSq = glm_vec3_norm2(dir);
                if (distSq > enemy->loseTargetRange * enemy->loseTargetRange) {
                    enemy->targetEntityId    = 0;
                    enemy->targetScene       = NULL;
                    enemy->pathWaypointCount = 0;
                    enemySetState(enemy, entity, ENEMY_STATE_IDLE);
                    return;
                }
            }
        }
    }

    // Check if in attack range
    if (enemyIsPlayerInAttackRange(enemy, pos)) {
        enemy->pathWaypointCount = 0;
        enemySetState(enemy, entity, ENEMY_STATE_ATTACK);
        return;
    }

    // Determine target position
    vec3 targetPos;
    Entity* player = enemyGetTarget(enemy);
    if (player) {
        Transform* pT = getComponent(player->scene, Transform, player->id);
        if (pT) {
            targetPos[0]               = pT->pos[0];
            targetPos[1]               = pT->pos[1];
            targetPos[2]               = pT->pos[2];
            enemy->targetPosValidTimer = ENEMY_TARGET_POS_TTL;
        } else {
            glm_vec3_copy(enemy->lastKnownTargetPos, targetPos);
        }
    } else {
        glm_vec3_copy(enemy->lastKnownTargetPos, targetPos);
    }

    // ── NavMesh pathfinding ────────────────────────────────────────────
    enemy->pathRecalcTimer -= timer.dt;
    if (enemy->pathRecalcTimer <= 0.0f || enemy->pathCurrentWaypoint >= enemy->pathWaypointCount) {
        enemy->pathRecalcTimer = 0.5f;
        enemy->pathWaypointCount =
            navMeshFindPath(entity->scene, pos, targetPos, (float*)enemy->pathWaypoints, 64);
        enemy->pathCurrentWaypoint = 1;
    }

    // Pick the move target: follow waypoints, or fall back to straight line
    vec3 moveTarget;
    if (enemy->pathWaypointCount > 0 && enemy->pathCurrentWaypoint < enemy->pathWaypointCount) {
        glm_vec3_copy(enemy->pathWaypoints[enemy->pathCurrentWaypoint], moveTarget);

        // Check if we reached the current waypoint
        vec3 toWaypoint;
        glm_vec3_sub(moveTarget, pos, toWaypoint);
        toWaypoint[1] = 0.0f;
        float distSq  = glm_vec3_norm2(toWaypoint);

        if (distSq < 1.0f * 1.0f) {
            enemy->pathCurrentWaypoint++;
            if (enemy->pathCurrentWaypoint < enemy->pathWaypointCount) {
                glm_vec3_copy(enemy->pathWaypoints[enemy->pathCurrentWaypoint], moveTarget);
            } else {
                glm_vec3_copy(targetPos, moveTarget);
            }
        }
    } else {
        // No navmesh path — fall back to straight line
        glm_vec3_copy(targetPos, moveTarget);
    }

    // ── Movement ───────────────────────────────────────────────────────
    if (enemy->character) {
        vec3 desiredVel = {0.0f, 0.0f, 0.0f};
        vec3 dir;
        glm_vec3_sub(moveTarget, pos, dir);
        dir[1] = 0.0f;
        glm_vec3_normalize(dir);
        glm_vec3_scale(dir, enemy->moveSpeed, desiredVel);

        // Push away from nearby enemies
        enemyApplySeparation(pos, desiredVel);

        joltCharacterUpdate(enemy->character, desiredVel, timer.dt);
        joltCharacterGetPosition(enemy->character, pos);
        transform->pos[0] = pos[0];
        transform->pos[1] = pos[1];
        transform->pos[2] = pos[2];
        if (enemy->sensorBody) joltBodySetPosition(enemy->sensorBody, pos);
    }

    enemyRotateToward(pos, moveTarget, enemy, transform);
}

static void enemyStateAttack(Enemy* enemy, Entity* entity, Transform* transform, vec3 pos) {
    enemy->stateTimer += timer.dt;
    enemy->attackCooldown -= timer.dt;
    if (enemy->attackCooldown < 0.0f) enemy->attackCooldown = 0.0f;

    // Check HP for retreat
    CharacterStats* stats = getComponent(entity->scene, CharacterStats, entity->id);
    if (stats && stats->hp / stats->maxHp < enemy->retreatThreshold) {
        enemySetState(enemy, entity, ENEMY_STATE_RETREAT);
        return;
    }

    // Check if still in attack range
    if (!enemyIsPlayerInAttackRange(enemy, pos)) {
        // Check broader aggro range
        if (!enemyIsPlayerInRange(enemy, pos)) {
            // Too far, go back to chase
            enemySetState(enemy, entity, ENEMY_STATE_CHASE);
            return;
        }
        // In aggro range but not attack range, chase
        enemySetState(enemy, entity, ENEMY_STATE_CHASE);
        return;
    }

    // Face player
    Entity* player = enemyGetTarget(enemy);
    if (player) {
        Transform* pT = getComponent(player->scene, Transform, player->id);
        if (pT) {
            enemyRotateToward(pos, pT->pos, enemy, transform);
            enemy->lastKnownTargetPos[0] = pT->pos[0];
            enemy->lastKnownTargetPos[1] = pT->pos[1];
            enemy->lastKnownTargetPos[2] = pT->pos[2];
        }
    }

    // Execute attack when cooldown allows
    if (enemy->attackCooldown <= 0.0f) {
        enemyAttack(enemy, entity);
    }
}

static void enemyStateRetreat(Enemy* enemy, Entity* entity, Transform* transform, vec3 pos) {
    enemy->stateTimer += timer.dt;

    // Check if far enough from player
    if (enemyIsFarEnoughToRegain(enemy, pos)) {
        enemy->targetEntityId = 0;
        enemy->targetScene    = NULL;
        enemySetState(enemy, entity, ENEMY_STATE_IDLE);
        return;
    }

    // Check if player is too close (still in attack range)
    if (enemyIsPlayerInAttackRange(enemy, pos)) {
        // Can't retreat fast enough, fight
        enemySetState(enemy, entity, ENEMY_STATE_ATTACK);
        return;
    }

    // Compute a flee point behind the enemy (away from player)
    vec3 fleeDir   = {0.0f, 0.0f, 0.0f};
    Entity* player = enemyGetTarget(enemy);
    vec3 playerPos;
    if (player) {
        Transform* pT = getComponent(player->scene, Transform, player->id);
        if (pT) {
            glm_vec3_sub(pos, pT->pos, fleeDir);
            enemy->lastKnownTargetPos[0] = pT->pos[0];
            enemy->lastKnownTargetPos[1] = pT->pos[1];
            enemy->lastKnownTargetPos[2] = pT->pos[2];
            glm_vec3_copy(pT->pos, playerPos);
        } else {
            glm_vec3_sub(pos, enemy->lastKnownTargetPos, fleeDir);
            glm_vec3_copy(enemy->lastKnownTargetPos, playerPos);
        }
    } else {
        glm_vec3_sub(pos, enemy->lastKnownTargetPos, fleeDir);
        glm_vec3_copy(enemy->lastKnownTargetPos, playerPos);
    }
    fleeDir[1] = 0.0f;
    {
        float norm2 = glm_vec3_norm2(fleeDir);
        if (norm2 < 0.001f) return;
        glm_vec3_normalize(fleeDir);
    }

    // Project a flee target ~20m away from player
    vec3 fleeTarget;
    glm_vec3_scale(fleeDir, 20.0f, fleeTarget);
    glm_vec3_add(pos, fleeTarget, fleeTarget);

    // Use navmesh to find a path to the flee target
    enemy->pathRecalcTimer -= timer.dt;
    if (enemy->pathRecalcTimer <= 0.0f || enemy->pathCurrentWaypoint >= enemy->pathWaypointCount) {
        enemy->pathRecalcTimer = 0.5f;
        enemy->pathWaypointCount =
            navMeshFindPath(entity->scene, pos, fleeTarget, (float*)enemy->pathWaypoints, 64);
        enemy->pathCurrentWaypoint = 1;
    }

    vec3 moveTarget;
    if (enemy->pathWaypointCount > 0 && enemy->pathCurrentWaypoint < enemy->pathWaypointCount) {
        glm_vec3_copy(enemy->pathWaypoints[enemy->pathCurrentWaypoint], moveTarget);

        vec3 toWaypoint;
        glm_vec3_sub(moveTarget, pos, toWaypoint);
        toWaypoint[1] = 0.0f;
        float distSq  = glm_vec3_norm2(toWaypoint);

        if (distSq < 1.0f * 1.0f) {
            enemy->pathCurrentWaypoint++;
            if (enemy->pathCurrentWaypoint < enemy->pathWaypointCount) {
                glm_vec3_copy(enemy->pathWaypoints[enemy->pathCurrentWaypoint], moveTarget);
            } else {
                glm_vec3_scale(fleeDir, 20.0f, moveTarget);
                glm_vec3_add(pos, moveTarget, moveTarget);
            }
        }
    } else {
        glm_vec3_scale(fleeDir, 20.0f, moveTarget);
        glm_vec3_add(pos, moveTarget, moveTarget);
    }

    if (enemy->character) {
        vec3 desiredVel = {0.0f, 0.0f, 0.0f};
        vec3 dir;
        glm_vec3_sub(moveTarget, pos, dir);
        dir[1] = 0.0f;
        glm_vec3_normalize(dir);
        glm_vec3_scale(dir, enemy->moveSpeed * 0.75f, desiredVel);

        // Push away from nearby enemies
        enemyApplySeparation(pos, desiredVel);

        joltCharacterUpdate(enemy->character, desiredVel, timer.dt);
        joltCharacterGetPosition(enemy->character, pos);
        transform->pos[0] = pos[0];
        transform->pos[1] = pos[1];
        transform->pos[2] = pos[2];
        if (enemy->sensorBody) joltBodySetPosition(enemy->sensorBody, pos);
    }

    // Face away from player (rotate toward flee direction)
    enemyRotateToward(pos, moveTarget, enemy, transform);
}

// ── System lifecycle ────────────────────────────────────────────────────────

static void update(void) {
    u32 numScenes = arraySize(ecs.scenes);
    for (u32 si = 0; si < numScenes; si++) {
        Scene* scene = ecs.scenes[si];
        if (!scene || !scene->ready) continue;

        SparseSet* enemies = getComponents(scene, Enemy);
        if (!enemies || enemies->size == 0) continue;

        // Pre-pass: cache all alive enemy positions for separation
        enemySepScene = scene;
        enemySepCount = 0;
        for (u32 i = 0; i < enemies->size; i++) {
            u32 entityId = ssGetValueByIndex(enemies, i);
            Enemy* enemy = (Enemy*)ssGetDataByIndex(enemies, i);
            if (!enemy || enemy->state == ENEMY_STATE_DEAD) continue;
            Transform* transform = getComponent(scene, Transform, entityId);
            if (!transform) continue;
            if (enemySepCount < 256) {
                if (enemy->character) {
                    joltCharacterGetPosition(enemy->character, enemySepPositions[enemySepCount]);
                } else {
                    glm_vec3_copy(transform->pos, enemySepPositions[enemySepCount]);
                }
                enemySepCount++;
            }
        }

        for (u32 i = 0; i < enemies->size; i++) {
            u32 entityId = ssGetValueByIndex(enemies, i);
            Enemy* enemy = (Enemy*)ssGetDataByIndex(enemies, i);
            if (!enemy) continue;

            // Death rotation: tilt enemy onto ground (no death anim yet)
            if (enemy->state == ENEMY_STATE_DEAD) {
                Transform* dT = getComponent(scene, Transform, entityId);
                if (dT) {
                    if (enemy->stateTimer < 0.001f) {
                        glm_vec4_copy(dT->rot, enemy->deathRotStart);
                        versor tiltRot = {};
                        glm_quatv(tiltRot, glm_rad(90.0f), (vec3){1.0f, 0.0f, 0.0f});
                        glm_quat_mul(enemy->deathRotStart, tiltRot, enemy->deathRotTarget);
                    }
                    float t = enemy->stateTimer / ENEMY_DEATH_ROT_DURATION;
                    if (t > 1.0f) t = 1.0f;
                    if (t > 0.0f) {
                        versor current = {};
                        quatSlerpShortest(enemy->deathRotStart, enemy->deathRotTarget, t, current);
                        glm_vec4_copy(current, dT->rot);
                    }
                    enemy->stateTimer += timer.dt;
                    transformSaveLast(scene, entityId);
                }
                continue;
            }

            Entity* entity = getEntity(scene, entityId);
            if (!entity) continue;

            Transform* transform = getComponent(scene, Transform, entityId);
            if (!transform) continue;

            vec3 pos;
            glm_vec3_copy(transform->pos, pos);

            // Read back from Jolt character for accurate physics position
            if (enemy->character) {
                vec3 prevPos = {pos[0], pos[1], pos[2]};
                joltCharacterGetPosition(enemy->character, pos);
                vec3 delta;
                glm_vec3_sub(pos, prevPos, delta);
                float deltaLen = glm_vec3_norm(delta);
                if (deltaLen > 2.0f) {
                    warn(
                        "enemy: large position jump %.2fm (%.1f,%.1f,%.1f)->(%.1f,%.1f,%.1f) "
                        "state=%d",
                        deltaLen,
                        prevPos[0],
                        prevPos[1],
                        prevPos[2],
                        pos[0],
                        pos[1],
                        pos[2],
                        enemy->state);
                }
                transform->pos[0] = pos[0];
                transform->pos[1] = pos[1];
                transform->pos[2] = pos[2];
                if (enemy->sensorBody) joltBodySetPosition(enemy->sensorBody, pos);
            }

            // Dispatch to state handler
            switch (enemy->state) {
                case ENEMY_STATE_IDLE:
                    enemyStateIdle(enemy, entity, transform, pos);
                    break;
                case ENEMY_STATE_ALERT:
                    enemyStateAlert(enemy, entity, transform, pos);
                    break;
                case ENEMY_STATE_CHASE:
                    enemyStateChase(enemy, entity, transform, pos);
                    break;
                case ENEMY_STATE_ATTACK:
                    enemyStateAttack(enemy, entity, transform, pos);
                    break;
                case ENEMY_STATE_RETREAT:
                    enemyStateRetreat(enemy, entity, transform, pos);
                    break;
                case ENEMY_STATE_DEAD:
                    break;
            }

            // Mark transform as active so it keeps getting uploaded to GPU
            transformSaveLast(scene, entityId);
        }
    }
}

Enemy* enemyCreate(Entity* entity,
                   float aggroRange,
                   float attackRange,
                   float loseTargetRange,
                   float attackDamage,
                   u32 attackDamageType,
                   float attackCooldown,
                   float moveSpeed,
                   float retreatThreshold) {
    if (!entity) return NULL;

    Enemy* enemy = createComponent(entity->scene, Enemy, entity->id);
    if (!enemy) return NULL;

    // Default values
    enemy->state      = ENEMY_STATE_IDLE;
    enemy->stateTimer = 0.0f;

    enemy->aggroRange       = aggroRange;
    enemy->attackRange      = attackRange;
    enemy->loseTargetRange  = loseTargetRange;
    enemy->retreatThreshold = retreatThreshold;
    enemy->regainRange      = 10.0f;

    enemy->targetEntityId        = 0;
    enemy->targetScene           = NULL;
    enemy->lastKnownTargetPos[0] = 0.0f;
    enemy->lastKnownTargetPos[1] = 0.0f;
    enemy->lastKnownTargetPos[2] = 0.0f;
    enemy->targetPosValidTimer   = 0.0f;

    enemy->patrolPointCount   = 0;
    enemy->currentPatrolIndex = 0;
    enemy->patrolWaitTimer    = 0.0f;
    enemy->patrolWaitDuration = 3.0f;

    enemy->attackCooldown    = 0.0f;
    enemy->attackCooldownMax = attackCooldown;
    enemy->attackDamage      = attackDamage;
    enemy->attackDamageType  = attackDamageType;

    enemy->moveSpeed = moveSpeed;

    enemy->character         = NULL;
    enemy->capsuleHalfHeight = 0.0f;
    enemy->capsuleRadius     = 0.0f;

    enemy->turnSpeed = ENEMY_TURN_SPEED;
    glm_quat_identity(enemy->baseRot);

    enemy->alertAnimPlayed = 0;
    enemy->deathAnimPlayed = 0;

    enemy->pathWaypointCount   = 0;
    enemy->pathCurrentWaypoint = 0;
    enemy->pathRecalcTimer     = 0.0f;

    // Initialize patrol wait with random duration
    enemy->patrolWaitDuration =
        ENEMY_PATROL_WAIT_MIN +
        (float)rand() / (float)RAND_MAX * (ENEMY_PATROL_WAIT_MAX - ENEMY_PATROL_WAIT_MIN);

    // Start with idle animation
    animationPlayBlended(entity, ENEMY_ANIM_IDLE, 1.0f, true, 0.15f);

    return enemy;
}

void enemyAddPatrolPoint(Enemy* enemy, vec3 position) {
    if (!enemy) return;
    if (enemy->patrolPointCount >= ENEMY_MAX_PATROL_POINTS) return;

    glm_vec3_copy(position, enemy->patrolPoints[enemy->patrolPointCount++]);
}

void enemySetBaseRot(Enemy* enemy, versor baseRot) {
    if (!enemy) return;
    glm_quat_copy(baseRot, enemy->baseRot);
}

static Mesh* enemyFindMesh(Entity* entity) {
    Mesh* mesh = getComponent(entity->scene, Mesh, entity->id);
    if (mesh) return mesh;

    u32 childCount = arraySize(entity->children);
    for (u32 i = 0; i < childCount; i++) {
        Entity* child = entity->children[i];
        mesh          = getComponent(child->scene, Mesh, child->id);
        if (mesh) return mesh;
    }
    return NULL;
}

static void enemySetupComponents(Scene* scene,
                                 Entity* entity,
                                 vec3 spawnPos,
                                 float dist,
                                 Mesh* templateMesh) {
    Transform* t = getComponent(scene, Transform, entity->id);
    if (t) {
        t->pos[0] = spawnPos[0];
        t->pos[1] = spawnPos[1];
        t->pos[2] = spawnPos[2];
        transformActivateAndSaveLastSubtree(scene, entity->id);
    }

    CharacterStats* stats = createComponent(scene, CharacterStats, entity->id);
    if (stats) {
        stats->hp                 = 100.0f;
        stats->maxHp              = 100.0f;
        stats->mana               = 0.0f;
        stats->maxMana            = 0.0f;
        stats->moveSpeed          = 4.0f;
        stats->attackSpeed        = 1.0f;
        stats->damage             = 15.0f;
        stats->armor              = 5.0f;
        stats->elementalResist[0] = 0.1f;
        stats->elementalResist[1] = 0.1f;
        stats->elementalResist[2] = 0.1f;
        stats->xp                 = 0.0f;
        stats->xpToNext           = 50.0f;
        stats->level              = 1;
        stats->isDead             = 0;
    }

    Enemy* enemy = enemyCreate(entity,
                               10.0f,  // aggroRange
                               1.f,    // attackRange
                               80.0f,  // loseTargetRange
                               15.0f,  // attackDamage
                               DAMAGE_TYPE_PHYSICAL,
                               2.0f,
                               3.0f,
                               0.3f);

    if (enemy) {
        // Derive capsule dimensions from model AABB
        float aabbMinY          = templateMesh->aabbLocal[0][1];
        float aabbMaxY          = templateMesh->aabbLocal[1][1];
        float aabbMinX          = templateMesh->aabbLocal[0][0];
        float aabbMaxX          = templateMesh->aabbLocal[1][0];
        float aabbMinZ          = templateMesh->aabbLocal[0][2];
        float aabbMaxZ          = templateMesh->aabbLocal[1][2];
        float modelHeight       = aabbMaxY - aabbMinY;
        float modelWidth        = fmaxf(aabbMaxX - aabbMinX, aabbMaxZ - aabbMinZ);
        float capsuleRadius     = modelWidth * 0.35f;
        float capsuleHalfHeight = (modelHeight * 0.5f - capsuleRadius);
        if (capsuleHalfHeight < 0.1f) capsuleHalfHeight = 0.1f;
        enemy->capsuleHalfHeight = capsuleHalfHeight;
        enemy->capsuleRadius     = capsuleRadius;

        enemyAddPatrolPoint(enemy, spawnPos);

        float startPos[3] = {spawnPos[0], spawnPos[1], spawnPos[2]};
        enemy->character  = joltCharacterCreate(enemy->capsuleHalfHeight,
                                                enemy->capsuleRadius,
                                                startPos,
                                                glm_rad(45.0f));
        enemy->sensorBody = joltCreateSensorCapsule(enemy->capsuleHalfHeight,
                                                    enemy->capsuleRadius,
                                                    startPos,
                                                    (uint64_t)entity->id);
        info("enemy: created '%s' at (%.1f, %.1f, %.1f) [%.0fm from player]",
             entity->name,
             spawnPos[0],
             spawnPos[1],
             spawnPos[2],
             dist);

        // Immediately aggro the player on spawn
        Entity* player = combatGetPlayerEntity();
        if (player) {
            enemy->targetScene    = player->scene;
            enemy->targetEntityId = player->id;
            Transform* pT         = getComponent(player->scene, Transform, player->id);
            if (pT) {
                enemy->lastKnownTargetPos[0] = pT->pos[0];
                enemy->lastKnownTargetPos[1] = pT->pos[1];
                enemy->lastKnownTargetPos[2] = pT->pos[2];
                enemy->targetPosValidTimer   = ENEMY_TARGET_POS_TTL;
            }
            enemy->state = ENEMY_STATE_CHASE;
        }

        vulkanDebugPhysicsRegisterCharacter(enemy->character);
    }
}

static Scene* runtimeScene;

static Mesh* enemyCloneMesh(Scene* dstScene, u32 dstEntityId, Mesh* srcMesh) {
    Mesh* dstMesh = createComponent(dstScene, Mesh, dstEntityId);

    // Copy local AABB
    glm_vec3_copy(srcMesh->aabbLocal[0], dstMesh->aabbLocal[0]);
    glm_vec3_copy(srcMesh->aabbLocal[1], dstMesh->aabbLocal[1]);

    // Deep-copy each primitive (vertex/index data)
    for (u32 p = 0; p < arraySize(srcMesh->primitives); p++) {
        Primitive* srcPrim = &srcMesh->primitives[p];
        Primitive dstPrim  = {};

        dstPrim.materialId    = srcPrim->materialId;
        dstPrim.attributeMask = srcPrim->attributeMask;
        dstPrim.indexCount    = srcPrim->indexCount;
        dstPrim.vertexCount   = srcPrim->vertexCount;

        // Copy indices
        if (srcPrim->indexCount > 0 && srcPrim->indices) {
            arraySetSize(dstPrim.indices, srcPrim->indexCount);
            memcpy(dstPrim.indices, srcPrim->indices, srcPrim->indexCount * sizeof(u32));
        }

        // Copy positions
        if (srcPrim->vertexCount > 0 && srcPrim->positions) {
            arraySetSize(dstPrim.positions, srcPrim->vertexCount * 3);
            memcpy(dstPrim.positions, srcPrim->positions, srcPrim->vertexCount * 3 * sizeof(float));
        }

        // Copy all attribute channels
        for (u32 a = 0; a < cgltf_attribute_type_max_enum; a++) {
            if (!srcPrim->attributes[a]) continue;
            // attribute size was stored as byte count (attrSize = vertexCount * accessor->stride)
            u32 attrSize = arraySize(srcPrim->attributes[a]);
            arraySetSize(dstPrim.attributes[a], attrSize);
            memcpy(dstPrim.attributes[a], srcPrim->attributes[a], attrSize);
        }

        arrayPut(dstMesh->primitives, dstPrim);
    }

    return dstMesh;
}

static void added(void) {
    Entity* playerEntity = combatGetPlayerEntity();
    vec3 playerPos       = {0.0f, 0.0f, 0.0f};
    if (playerEntity) {
        Transform* playerTransform = getComponent(playerEntity->scene, Transform, playerEntity->id);
        if (playerTransform) {
            glm_vec3_copy(playerTransform->pos, playerPos);
        }
    }
    info("enemy: spawning around player at (%.1f, %.1f, %.1f)",
         playerPos[0],
         playerPos[1],
         playerPos[2]);

    // Create a dedicated runtime scene for dynamically spawned entities.
    // alwaysVisible = true ensures it is never CPU-frustum-culled regardless
    // of where enemies move.  hasBounds = false means AABB tests are skipped.
    runtimeScene                 = static_cast<Scene*>(memoryAlloc(sizeof(Scene)));
    *runtimeScene               = (Scene){0};
    runtimeScene->alwaysVisible = true;
    arrayPut(ecs.scenes, runtimeScene);
    info("enemy: created runtime scene for dynamic entities");

    // Scan all loaded scenes for enemy templates
    u32 numScenes = arraySize(ecs.scenes);
    for (u32 si = 0; si < numScenes; si++) {
        Scene* scene = ecs.scenes[si];
        if (!scene || !scene->ready) continue;
        if (scene == runtimeScene) continue;

        u32 enemyTemplateCount = 0;
        u32 numEntities        = arraySize(scene->entities);
        for (u32 ei = 0; ei < numEntities; ei++) {
            Entity* entity = scene->entities[ei];
            if (!entity || !entity->name) continue;

            if (strncmp(entity->name, "enemy_", 6) != 0) continue;

            Mesh* templateMesh = enemyFindMesh(entity);
            if (!templateMesh) {
                warn("enemy: '%s' has no Mesh, skipping", entity->name);
                continue;
            }

            enemyTemplateCount++;

            info("enemy: template '%s' (id %u), mesh on entity %u, creating %d instances",
                 entity->name,
                 entity->id,
                 templateMesh == getComponent(scene, Mesh, entity->id) ? entity->id : 0,
                 ENEMY_INSTANCES_PER_MODEL);

            Transform* templateTransform = getComponent(scene, Transform, entity->id);
            versor templateRot           = {0.0f, 0.0f, 0.0f, 1.0f};
            if (templateTransform) {
                glm_quat_copy(templateTransform->rot, templateRot);
            }

            for (u32 inst = 0; inst < ENEMY_INSTANCES_PER_MODEL; inst++) {
                char name[64];
                snprintf(name, sizeof(name), "%s_inst%u", entity->name, inst);
                Entity* newEntity = createEntity(runtimeScene, name);

                Transform* t = createComponent(runtimeScene, Transform, newEntity->id);
                glm_quat_copy(templateRot, t->rot);
                t->pos[3] = 1.0f;

                // Spread instances in a ring around the player
                float spawnDistMin = 20.0f;
                float spawnDistMax = 30.0f;
                float anglePerInst = (2.0f * GLM_PI) / (float)ENEMY_INSTANCES_PER_MODEL;
                float angleOffset =
                    (float)enemyTemplateCount * anglePerInst * 0.37f;  // stagger per template
                float angle = anglePerInst * (float)inst + angleOffset;
                float spawnDist =
                    spawnDistMin + (spawnDistMax - spawnDistMin) *
                                       ((float)inst / (float)(ENEMY_INSTANCES_PER_MODEL - 1));
                float offX = sinf(angle) * spawnDist;
                float offZ = cosf(angle) * spawnDist;

                // Cast ray down from high above to find terrain height
                float modelHalfH = -templateMesh->aabbLocal[0][1];

                vec3 rayOrigin = {playerPos[0] + offX, playerPos[1] + 100.0f, playerPos[2] + offZ};
                vec3 rayDir    = {0.0f, -1.0f, 0.0f};
                vec3 spawnPos  = {rayOrigin[0], playerPos[1], rayOrigin[2]};
                vec3 hitPos;
                if (!joltCastRay(rayOrigin, rayDir, 200.0f, hitPos)) {
                    warn("enemy: raycast missed terrain for '%s' instance %u, using fallback Y",
                         entity->name,
                         inst);
                } else {
                    spawnPos[0] = hitPos[0];
                    spawnPos[1] = hitPos[1] + modelHalfH;
                    spawnPos[2] = hitPos[2];
                }

                t->pos[0] = spawnPos[0];
                t->pos[1] = spawnPos[1];
                t->pos[2] = spawnPos[2];

                // Clone mesh data into the runtime scene for this instance
                Mesh* instMesh = enemyCloneMesh(runtimeScene, newEntity->id, templateMesh);
                InstanceData instanceData = {.entity = newEntity->id};
                arrayPut(instMesh->instances, instanceData);

                transformActivateAndSaveLastSubtree(runtimeScene, newEntity->id);

                enemySetupComponents(runtimeScene, newEntity, spawnPos, spawnDist, instMesh);
            }

            // Hide the template entity in the source scene (scale to 0)
            Transform* origT = getComponent(scene, Transform, entity->id);
            if (origT) {
                origT->pos[3] = 0.0f;
                transformActivateAndSaveLastSubtree(scene, entity->id);
            }
        }

        // Rebuild the source scene GPU data (template entities now hidden)
        if (enemyTemplateCount > 0) {
            info("enemy: rebuilding Vulkan scene for '%s'", scene->name.data);
            rendererSceneDestroy(scene);
            rendererSceneCreate(scene);
        }
    }

    // Create GPU representation for the runtime scene
    rendererSceneCreate(runtimeScene);
    runtimeScene->ready = true;
}

void removed(void) {
    // Clean up Jolt characters in all scenes
    u32 numScenes = arraySize(ecs.scenes);
    for (u32 si = 0; si < numScenes; si++) {
        Scene* scene = ecs.scenes[si];
        if (!scene || !scene->ready) continue;

        SparseSet* enemies = getComponents(scene, Enemy);
        if (!enemies || enemies->size == 0) continue;

        for (u32 i = 0; i < enemies->size; i++) {
            Enemy* enemy = (Enemy*)ssGetDataByIndex(enemies, i);
            if (enemy && enemy->character) {
                vulkanDebugPhysicsUnregisterCharacter(enemy->character);
                joltCharacterDestroy(enemy->character);
                enemy->character = NULL;
            }
            if (enemy && enemy->sensorBody) {
                joltBodyDestroy(enemy->sensorBody);
                enemy->sensorBody = NULL;
            }
        }
    }

    // Destroy the runtime scene
    if (runtimeScene) {
        rendererSceneDestroy(runtimeScene);
        sceneDestroy(runtimeScene);
        runtimeScene = NULL;
    }
}
