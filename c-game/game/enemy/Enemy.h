#pragma once

#include "ecs/system/scene/SceneSystem.h"

struct JoltCharacter;
struct JoltBody;

enum EnemyState {
    ENEMY_STATE_IDLE,
    ENEMY_STATE_ALERT,
    ENEMY_STATE_CHASE,
    ENEMY_STATE_ATTACK,
    ENEMY_STATE_RETREAT,
    ENEMY_STATE_DEAD,
};

#define ENEMY_MAX_PATROL_POINTS 8

struct Enemy {
    // State machine
    EnemyState state;
    float stateTimer;

    // Detection
    float aggroRange;
    float attackRange;
    float loseTargetRange;   // distance at which enemy gives up chasing
    float retreatThreshold;  // HP ratio (0.0-1.0) to trigger retreat
    float regainRange;       // distance beyond aggroRange to return from retreat

    // Target tracking
    u32    targetEntityId;
    Scene* targetScene;
    float lastKnownTargetPos[3];
    float targetPosValidTimer;

    // Patrol (idle state)
    vec3  patrolPoints[ENEMY_MAX_PATROL_POINTS];
    u32   patrolPointCount;
    u32   currentPatrolIndex;
    float patrolWaitTimer;
    float patrolWaitDuration;

    // Attack
    float attackCooldown;
    float attackCooldownMax;
    float attackDamage;
    u32   attackDamageType;

    // Movement
    float moveSpeed;

    // Jolt character controller
    JoltCharacter* character;
    JoltBody*      sensorBody;   // kinematic sensor capsule for overlap queries
    float capsuleHalfHeight;
    float capsuleRadius;

    // Rotation
    float turnSpeed;
    versor baseRot;

    // Animation trigger guard
    u8 alertAnimPlayed;
    u8 deathAnimPlayed;

    // NavMesh pathfinding (chase state)
    vec3  pathWaypoints[64];
    u32   pathWaypointCount;
    u32   pathCurrentWaypoint;
    float pathRecalcTimer;
    float pathWaypointStuckTimer;
    u32   pathPrevWaypointIdx;

    // Death rotation (no death anim yet, so we rotate to lie on ground)
    versor deathRotStart;
    versor deathRotTarget;
};

REGISTER_COMPONENT(Enemy);
