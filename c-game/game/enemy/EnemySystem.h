#pragma once

#include "ecs/system/System.h"
#include "enemy/Enemy.h"
#include "ecs/system/scene/Scene.h"  // IWYU pragma: keep

namespace game {
class EnemySystem : public engine::System {
public:
    EnemySystem();
    void added() override;
    void removed() override;
    void update() override;
};

extern EnemySystem enemySystem;

// Create an Enemy component on the given entity with default stats
Enemy* enemyCreate(engine::Entity* entity,
                    float aggroRange,
                    float attackRange,
                    float loseTargetRange,
                    float attackDamage,
                    u32   attackDamageType,
                    float attackCooldown,
                    float moveSpeed,
                    float retreatThreshold);

// Add a patrol point (world-space position)
void enemyAddPatrolPoint(Enemy* enemy, vec3 position);

// Set the enemy's base rotation from glTF loader
void enemySetBaseRot(Enemy* enemy, versor baseRot);

//
}  // namespace game
