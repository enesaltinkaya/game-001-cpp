#pragma once

#include "ecs/system/scene/SceneSystem.h"
#include "ecs/system/System.h"
#include "ecs/system/physics/PhysicsComponent.h"  // IWYU pragma: keep
#include "ecs/system/transform/TransformComponent.h"  // IWYU pragma: keep
struct JoltCharacter;
namespace game {


// ── Player component ────────────────────────────────────────────────────────
struct Player {
    float moveSpeed;
    float turnSpeed;
    float cameraYaw;        // horizontal orbit angle (radians)
    float cameraPitch;      // vertical orbit angle (radians)
    float moveYaw;          // movement basis yaw (decoupled from camera during left-click drag)
    float facingYaw;        // actual player facing yaw
    bool  isMoving;
    bool  isJumping;
    bool  isCasting;        // currently playing an ability animation
    bool  autoRun;
    versor baseRot;         // initial upright rotation (from glTF armature)
    JoltCharacter* character; // Jolt CharacterVirtual
    JoltBody*       sensorBody; // kinematic sensor capsule for overlap queries
    // True until the streaming heightfield body under the spawn position
    // exists. While set, the character is held at its spawn position (no
    // physics update) so it cannot fall through the terrain before the
    // collision data is ready (see heightmapTerrainHasBodyAt).
    bool  waitingForGround;
};
REGISTER_COMPONENT(Player);

// ── PlayerSystem ────────────────────────────────────────────────────────────
class PlayerSystem : public engine::System {
public:
    PlayerSystem();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern PlayerSystem playerSystem;
engine::Scene* getPlayerScene(void);
bool playerGetFacingYaw(float* yaw);

// Debug helpers for lightweight GUI/state inspection.
bool playerGetRotation(versor outRot);
bool playerGetDebugState(bool* isMoving, bool* isJumping, bool* autoRun);
bool playerGetStats(float* hp, float* maxHp, float* mana, float* maxMana, u32* level);

// Current player world position (X, Y, Z in meters). Returns false until the
// player scene has finished loading. Used by terrain streaming to decide
// which tile grid to keep resident.
bool playerGetPosition(vec3 outPos);

// Teleports the loaded player to a world position and synchronizes the physics
// character/sensor bodies. Returns false until the player scene is ready.
bool playerTeleportTo(vec3 pos);

// Default spawn position (matches the hardcoded spawn in playerSceneLoaded).
// Used as a fallback for load-time terrain centering when no DB transform is
// saved yet.
void playerGetSpawn(vec3 outPos);

// ── Shared character movement utility ───────────────────────────────────────
// Takes a world position and speed, sets Jolt velocity. Used by both player
// controller and enemy AI.
void characterMoveToward(JoltCharacter* character, versor baseRot,
                         float turnSpeed, float moveSpeed,
                         vec3 targetPos, vec3 currentPos,
                         engine::Transform* transform, bool* isMoving,
                         engine::Entity* playerEntity,
                         const char* animIdle, const char* animRun, const char* animWalk,
                         bool shiftHold);
}  // namespace game
