#include "Player.h"
#include "Player.h"
#include <algorithm>
#include "events/Events.h"
#include "azgaar/AzgaarWater.h"
#include "azgaar/AzgaarWeather.h"
#include "ecs/system/heightmap/HeightmapTerrain.h"
#include "renderer/vulkan/pass/debug_physics/VulkanDebugPhysicsPass.h"

// ── Animation names ──────────────────────────────────────────────────────────
#define ANIM_IDLE "eve_idle1"
#define ANIM_RUN "eve_run1"
#define ANIM_RUN_BACK \
    "female_run_backward"  // legacy fallback; backward locomotion uses ANIM_RUN reversed
#define ANIM_STRAFE_LEFT "strafe_left"
#define ANIM_STRAFE_RIGHT "strafe_right"
#define ANIM_WALK "female_walk"
#define ANIM_JUMP "eve_jump"
#define ANIM_TPOSE "eve_t"
#define ANIM_HURRICANE_KICK "flying_hurricane_kick"
#define ANIM_BICYCLE_KICK "flying_bicycle_kick"

// ── Character movement speeds ───────────────────────────────────────────────
#define MOVE_SPEED_RUN 4.0f           // default forward run (m/s)
#define MOVE_SPEED_WALK 2.0f          // shift-walk / backward (m/s)
#define MOVE_SPEED_TURN 20.0f         // rotation slerp factor
#define MOVE_SPEED_JUMP 4.0f          // vertical jump impulse (m/s)
#define MOVE_SPEED_SPRINT_MULT 40.0f  // alt-sprint multiplier
#define MOVE_SPEED_BICYCLE_MULT 3.0f  // bicycle-kick speed multiplier

// ── Animation speeds ────────────────────────────────────────────────────────
#define ANIM_SPEED_IDLE 1.0f
#define ANIM_SPEED_RUN 1.750f
#define ANIM_SPEED_WALK 1.50f
#define ANIM_SPEED_JUMP 1.0f
#define ANIM_SPEED_TPOSE 1.0f
#define ANIM_SPEED_HURRICANE_KICK 1.5f
#define ANIM_SPEED_BICYCLE_KICK 1.25f
#define ANIM_SPEED_CAST_REPLAY 1.25f

// ── Fireball constants ──────────────────────────────────────────────────────
#define FIREBALL_MAX 8
#define FIREBALL_SPEED 25.0f      // meters per second
#define FIREBALL_LIFETIME 2.5f    // seconds
#define FIREBALL_RANGE 6.0f       // light range
#define FIREBALL_INTENSITY 30.0f  // light intensity

#include "ecs/Ecs.h"
#include "ecs/system/scene/SceneSystem.h"
#include "ecs/system/scene/SceneParser.h"
#include "ecs/system/System.h"
#include "ecs/system/animation/AnimatorComponent.h"
#include "character/CharacterStats.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/camera/flyingCamera/FlyingCamera.h"
#include "camera/TopDownCamera.h"
#include "camera/ThirdPersonCamera.h"
#include "ecs/system/window/WindowSystem.h"
#include "ecs/system/light/LightComponent.h"
#include "ecs/system/light/LightSystem.h"
#include "ecs/system/transform/TransformComponent.h"
#include "ecs/system/transform/TransformDb.h"
#include "ecs/system/transform/TransformSystem.h"
#include "renderer/Renderer.h"
#include "ecs/system/sound/SoundSystem.h"
#include "ecs/system/window/WindowSystem.h"
#include "combat/Combat.h"
#include "combat/AttackHitbox.h"
#include "character/CharacterStats.h"
#include "json/Json.h"
#include <unordered_map>
#include "timer/Timer.h"

namespace game {
static void playerMovementTopDownOnly(void);
static void playerLightCreate(engine::Entity* playerEntity);
static void playerLightUpdate(void);

PlayerSystem playerSystem;

PlayerSystem::PlayerSystem() : engine::System("player") {}

static engine::Scene* playerScene;

static vec3 Y_UP = {0.0f, 1.0f, 0.0f};
static u32 playerEntityId;
static bool playerReady;

engine::Scene* getPlayerScene(void) {
    return playerScene;
}

bool playerGetFacingYaw(float* yaw) {
    if (!yaw || !playerReady || !playerScene) return false;

    Player* player = getComponent(playerScene, Player, playerEntityId);
    if (!player) return false;

    *yaw = player->facingYaw;
    return true;
}

bool playerGetRotation(versor outRot) {
    if (!outRot || !playerReady || !playerScene) return false;

    engine::Transform* transform = getComponent(playerScene, engine::Transform, playerEntityId);
    if (!transform) return false;

    glm_quat_copy(transform->rot, outRot);
    return true;
}

bool playerGetDebugState(bool* isMoving, bool* isJumping, bool* autoRun) {
    if (!playerReady || !playerScene) return false;

    Player* player = getComponent(playerScene, Player, playerEntityId);
    if (!player) return false;

    if (isMoving) *isMoving = player->isMoving;
    if (isJumping) *isJumping = player->isJumping;
    if (autoRun) *autoRun = player->autoRun;
    return true;
}

bool playerGetStats(float* hp, float* maxHp, float* mana, float* maxMana, u32* level) {
    if (!playerReady || !playerScene) return false;

    CharacterStats* stats = getComponent(playerScene, CharacterStats, playerEntityId);
    if (!stats) return false;

    if (hp) *hp = stats->hp;
    if (maxHp) *maxHp = stats->maxHp;
    if (mana) *mana = stats->mana;
    if (maxMana) *maxMana = stats->maxMana;
    if (level) *level = stats->level;
    return true;
}

bool playerGetPosition(vec3 outPos) {
    if (!outPos || !playerReady || !playerScene) return false;

    engine::Transform* transform = getComponent(playerScene, engine::Transform, playerEntityId);
    if (!transform) return false;

    outPos[0] = transform->pos[0];
    outPos[1] = transform->pos[1];
    outPos[2] = transform->pos[2];
    return true;
}

bool playerTeleportTo(vec3 pos) {
    if (!pos || !playerReady || !playerScene) return false;

    engine::Transform* transform = getComponent(playerScene, engine::Transform, playerEntityId);
    Player* player       = getComponent(playerScene, Player, playerEntityId);
    if (!transform || !player) return false;

    engine::transformActivateAndSaveLastSubtree(playerScene, playerEntityId);
    glm_vec3_copy(pos, transform->pos);

    if (player->character) joltCharacterSetPosition(player->character, transform->pos);
    if (player->sensorBody) joltBodySetPosition(player->sensorBody, transform->pos);

    player->autoRun = false;
    return true;
}

void playerGetSpawn(vec3 outPos) {
    if (!outPos) return;
    outPos[0] = -881.88f;
    outPos[1] = 511.55f;
    outPos[2] = 1691.46f;
}
static engine::Sound* kickSound;
static engine::Sound* bicycleKickSound;
static engine::Sound* jumpSound;
static engine::Sound* stepSound;
static engine::Sound* hitSound;
static const char* castingAnim;                   // animation name of the active ability
static int castingRepeatsLeft;                    // how many more times to replay the animation
static engine::Entity* kickHitboxEntity;                  // hitbox entity for kick damage
static engine::Entity* kickLightEntity;                   // bone entity carrying the kick point light
static float kickLightFade;                       // >0 while fading out (counts down to 0)
static const float kickLightFadeDuration = 2.0f;  // seconds
static const float kickLightIntensity    = 15.0f;
static engine::Entity* playerLightEntity;                // soft point light following the player
static const float playerLightHeight    = 3.0f;  // meters above the player origin
static const float playerLightIntensity = 50.0f;
static bool isTposing;  // playing eve_t emote

// ── Camera mode (ISO default, C to toggle) ─────────────────────────────────
enum CameraMode {
    CAM_MODE_ISO,
    CAM_MODE_ORBIT,
};

static CameraMode gCameraMode = CAM_MODE_ISO;

// ── Fireball state ──────────────────────────────────────────────────────────
struct Fireball {
    engine::Entity* entity;
    engine::Entity* hitboxEntity;  // combat hitbox that follows the fireball
    vec3 velocity;
    float lifetime;  // remaining seconds
};

static Fireball fireballs[FIREBALL_MAX];
static int fireballCount = 0;
static engine::Entity* fireballDestroyList[FIREBALL_MAX];  // entities to destroy in postUpdate
static int fireballDestroyCount = 0;

// ── Player DB (persist camera angles + distance across runs) ────────────────

struct PlayerDb {
    float cameraYaw;
    float cameraPitch;
    float moveYaw;
    float cameraDistance;
    int cameraMode;
};

static CameraMode playerDbSanitizeCameraMode(int cameraMode) {
    if (cameraMode == CAM_MODE_ORBIT) return CAM_MODE_ORBIT;
    return CAM_MODE_ISO;
}

static void playerDbInit(void) {
    if (!utils::sqliteTableExists("player")) {
        utils::sqliteExecute(
            "CREATE TABLE IF NOT EXISTS player ("
            "name TEXT PRIMARY KEY, "
            "data BLOB);");
    }
}

static void playerDbSave(const char* name, PlayerDb* data) {
    void* stmt = utils::sqliteStatement("REPLACE INTO player (name, data) VALUES (?, ?);");
    utils::sqliteBindText(stmt, 1, name);
    utils::sqliteBindBlob(stmt, 2, data, sizeof(PlayerDb));
    utils::sqliteStep(stmt);
    utils::sqliteFinalize(stmt);
}

static bool playerDbLoad(const char* name, PlayerDb* data) {
    void* stmt  = utils::sqliteStatement("SELECT data, length(data) FROM player WHERE name = ?;");
    bool result = false;
    utils::sqliteBindText(stmt, 1, name);
    if (utils::sqliteStep(stmt)) {
        void* blob   = utils::sqliteGetBlob(stmt, 0);
        int blobSize = utils::sqliteGetInt(stmt, 1);
        memcpy(data, blob, std::min(static_cast<size_t>(blobSize), sizeof(PlayerDb)));
        result = true;
    }
    utils::sqliteFinalize(stmt);
    return result;
}

static void playerDbSaveCameraState(float cameraDistance) {
    if (!playerReady || !playerScene) return;

    Player* player = getComponent(playerScene, Player, playerEntityId);
    if (!player) return;

    PlayerDb data = {
        .cameraYaw      = player->cameraYaw,
        .cameraPitch    = player->cameraPitch,
        .moveYaw        = player->moveYaw,
        .cameraDistance = cameraDistance,
        .cameraMode     = gCameraMode,
    };
    playerDbSave("player", &data);
}

static float cameraSensitivity = 0.15f;
static float pitchMin          = -20.0f * GLM_PIf / 180.0f;
static float pitchMax          = 60.0f * GLM_PIf / 180.0f;

// TEMP DEBUG: ENGINE_CAM_ORBIT="yawDeg,pitchDeg" forces the orbit camera with
// the given angles after the player spawns (used together with
// ENGINE_CAM_TELEPORT for targeted landmark screenshots; view direction is
// (sin yaw, ., cos yaw), so yaw 180 looks toward -Z).
static void tempCameraOrbitOverride(void) {
    static char* env = nullptr;
    static bool done = false;
    if (done) return;
    if (!env) env = getenv("ENGINE_CAM_ORBIT");
    if (!env || !*env) {
        done = true;
        return;
    }
    if (!playerReady) return; // wait for the player entity + cameras to spawn
    done = true;

    float yawDeg, pitchDeg;
    if (sscanf(env, "%f,%f", &yawDeg, &pitchDeg) != 2) return;
    float yaw   = yawDeg * GLM_PIf / 180.0f;
    float pitch = pitchDeg * GLM_PIf / 180.0f;
    pitch       = glm_clamp(pitch, pitchMin, pitchMax);

    gCameraMode = CAM_MODE_ORBIT;
    Player* p   = getComponent(playerScene, Player, playerEntityId);
    if (p) {
        p->cameraYaw   = yaw;
        p->cameraPitch = pitch;
        p->moveYaw     = yaw;
        p->facingYaw   = yaw;
    }
    engine::Entity* camEntity = engine::cameraGetEntity();
    engine::Camera* camera    = camEntity ? getComponent(engine::ecs.defaultScene, engine::Camera, camEntity->id) : nullptr;
    if (camera) {
        camera->yaw   = yaw;
        camera->pitch = pitch;
    }
    thirdPersonCameraSetTarget(playerScene, playerEntityId);
    thirdPersonCameraSetAngles(yaw, pitch);
    utils::info("TEMP camera forced to orbit (yaw %.0f deg, pitch %.0f deg)", yawDeg, pitchDeg);
}

// ── Player capsule dimensions (meters) ──────────────────────────────────────
static const float playerCapsuleHalfHeight = 0.45f;  // half of cylindrical part
static const float playerCapsuleRadius     = 0.25f;  // capsule radius
static const float jumpSpeed               = MOVE_SPEED_JUMP;

// ── Accumulated input (preUpdate → update) ──────────────────────────────────
// One-shot events are OR-accumulated across frames so they are never missed
// when update() does not run every frame (UPS < FPS).  Accumulated values
// are summed.  Continuous held-key state is the latest snapshot.
static struct {
    bool ability1;           // KEY_1 pressed
    bool ability2;           // KEY_2 pressed
    bool ability5;           // KEY_5 pressed (fireball)
    bool autoRunToggle;      // middle-mouse clicked
    float scrollY;           // scroll-wheel delta (summed)
    float mouseDx, mouseDy;  // relative mouse motion while dragging (summed)
    bool moveW, moveS, moveA, moveD;
    bool jump;  // SPACE held
    bool shift, alt, ctrl;
    bool rightMouse, leftMouse;
    bool tKey;  // T key pressed (eve_t emote)
} playerInput;

// Cursor-management state driven by preUpdate every frame.
static bool piPrevRightMouse;
static bool piPrevLeftMouse;
static bool piPrevMiddleMouse;

// ── scene-load callback ─────────────────────────────────────────────────────

static void playerSceneLoaded(engine::Scene* scene, void*) {
    scene->alwaysVisible = true;
    playerScene          = scene;
    for (const auto& entry : scene->extras) {
        u32 entityId = entry.first;
        Json* extras = entry.second;

        json_t* p = json_object_get(extras, "player");
        if (p) {
            Player* player      = createComponent(scene, Player, entityId);
            player->moveSpeed   = MOVE_SPEED_RUN;
            player->turnSpeed   = MOVE_SPEED_TURN;
            player->cameraYaw   = glm_rad(180.0f);
            player->cameraPitch = glm_rad(8.0f);
            player->moveYaw     = glm_rad(180.0f);
            player->facingYaw   = player->moveYaw;
            player->isMoving    = false;
            player->isJumping   = false;
            // player->autoRun     = true;

            engine::Entity* playerEntity = engine::getEntity(scene, entityId);
            utils::info("player: tagged entity '%s' (id %u) as player",
                 playerEntity ? playerEntity->name : "(null)",
                 entityId);

            engine::animationPlay(playerEntity, ANIM_IDLE, ANIM_SPEED_IDLE, true);

            engine::Transform* transform = getComponent(scene, engine::Transform, playerEntity->id);
            glm_quat_copy(transform->rot, player->baseRot);

            // Default spawn position
            playerGetSpawn(transform->pos);

            // Load saved player state (position + camera angles)
            playerDbInit();
            engine::Transform savedTransform;
            if (engine::transformDbLoad("player", &savedTransform)) {
                glm_vec3_copy(savedTransform.pos, transform->pos);
                glm_quat_copy(savedTransform.rot, transform->rot);
                // transform->pos[1] += 10;
                utils::info("player: loaded saved transform");
            }
            bool loadedPlayerDb       = false;
            float savedCameraDistance = 10.0f;  // default, matches ThirdPersonCamera.c
            PlayerDb savedPlayer      = {
                .cameraYaw      = player->cameraYaw,
                .cameraPitch    = player->cameraPitch,
                .moveYaw        = player->moveYaw,
                .cameraDistance = savedCameraDistance,
                .cameraMode     = CAM_MODE_ISO,
            };
            if (playerDbLoad("player", &savedPlayer)) {
                player->cameraYaw   = savedPlayer.cameraYaw;
                player->cameraPitch = savedPlayer.cameraPitch;
                player->moveYaw     = savedPlayer.moveYaw;
                player->facingYaw   = player->moveYaw;
                savedCameraDistance = savedPlayer.cameraDistance;
                gCameraMode         = playerDbSanitizeCameraMode(savedPlayer.cameraMode);
                loadedPlayerDb      = true;
                utils::info("player: loaded saved camera state (%s)",
                     (gCameraMode == CAM_MODE_ISO) ? "isometric" : "orbit");
            }

            // Create Jolt CharacterVirtual
            float startPos[3]  = {transform->pos[0], transform->pos[1], transform->pos[2]};
            player->character  = joltCharacterCreate(playerCapsuleHalfHeight,
                                                     playerCapsuleRadius,
                                                     startPos,
                                                     glm_rad(45.0f));
            player->sensorBody = joltCreateSensorCapsule(playerCapsuleHalfHeight,
                                                         playerCapsuleRadius,
                                                         startPos,
                                                         (uint64_t)entityId);
            // Hold the character at its spawn position until the streaming
            // heightfield body under it exists (Azgaar world). In the regular
            // (mesh) world the active heightmap is nullptr, so this clears on the
            // first update and the character moves normally.
            player->waitingForGround = true;
            utils::info("player: created character controller");

            engine::vulkanDebugPhysicsRegisterCharacter(player->character);

            kickSound        = engine::soundLoad("sound/player/depdep.ogg");
            bicycleKickSound = engine::soundLoad("sound/player/bicycle_kick.ogg");
            jumpSound        = engine::soundLoad("sound/player/jump-mono.ogg");
            stepSound        = engine::soundLoad("sound/player/step-mono.ogg");
            hitSound         = engine::soundLoad("sound/player/hit.ogg");

            playerEntityId = entityId;
            playerReady    = true;

            combatSetPlayerEntity(scene, entityId);

            // Create CharacterStats for the player (used by HUD + combat)
            CharacterStats* stats = createComponent(scene, CharacterStats, entityId);
            stats->hp             = 500.0f;
            stats->maxHp          = 500.0f;
            stats->mana           = 50.0f;
            stats->maxMana        = 50.0f;
            stats->moveSpeed      = MOVE_SPEED_RUN;
            stats->attackSpeed    = 1.0f;
            stats->damage         = 10.0f;
            stats->armor          = 5.0f;
            stats->xp             = 0.0f;
            stats->xpToNext       = 100.0f;
            stats->level          = 1;
            stats->isDead         = 0;
            for (int i = 0; i < 3; i++) stats->elementalResist[i] = 0.0f;

            // Initialize camera behind player
            engine::Entity* camEntity = engine::cameraGetEntity();
            engine::Camera* camera    = getComponent(engine::ecs.defaultScene, engine::Camera, camEntity->id);
            if (!engine::flyingCameraIsActive()) {
                camera->yaw   = player->cameraYaw;
                camera->pitch = player->cameraPitch;
            }

            // Initialize third-person orbit camera
            thirdPersonCameraInit();
            thirdPersonCameraSetTarget(scene, entityId);
            thirdPersonCameraSetAngles(player->cameraYaw, player->cameraPitch);
            thirdPersonCameraSetDistance(savedCameraDistance);

            // Initialize top-down camera (default mode)
            topDownCameraInit();
            topDownCameraSetTarget(scene, entityId);
            topDownCameraSetDistance(savedCameraDistance);

            // Sync moveYaw to the top-down camera so WASD directions match
            // the camera orientation at first launch (no DB save exists).
            if (!loadedPlayerDb) {
                player->moveYaw   = topDownCameraGetYaw();
                player->facingYaw = player->moveYaw;
            }

            // playerLightCreate(playerEntity);
        }
    }
}

static void waitAsecondTemp(void*) {
    engine::sceneLoadCb("models/eve.dat", playerSceneLoaded, nullptr);
}

static void animationsLoaded(void*) {
    utils::futureTaskAdd(1000, waitAsecondTemp, nullptr);
}

void PlayerSystem::added() {
    utils::signalSubscribe("animationsLoaded", animationsLoaded);
}

void PlayerSystem::removed() {
    utils::signalRemoveSubscription("animationsLoaded", animationsLoaded);
    if (playerReady && playerScene) {
        // The HeightmapTerrain component lives in the player scene: clear the
        // active pointer and free its tile data while the component is still
        // valid. Pending future tasks (grid verification, screenshots) or the
        // heightmap streaming system must never touch a dangling instance
        // after the scene is destroyed below.
        engine::HeightmapTerrain* hm = engine::heightmapTerrainGetActive();
        if (hm) {
            engine::heightmapTerrainSetActive(nullptr);
            engine::heightmapTerrainDestroyData(hm);
        }

        Player* player = getComponent(playerScene, Player, playerEntityId);
        if (player && player->character) {
            engine::vulkanDebugPhysicsUnregisterCharacter(player->character);
            joltCharacterDestroy(player->character);
            player->character = nullptr;
        }
        if (player && player->sensorBody) {
            joltBodyDestroy(player->sensorBody);
            player->sensorBody = nullptr;
        }
        engine::rendererSceneDestroy(playerScene);
        engine::sceneDestroy(playerScene);
    }
    playerLightEntity = nullptr;
    playerScene       = nullptr;
    playerReady       = false;
}

void PlayerSystem::preUpdate() {
    if (!playerReady || engine::flyingCameraIsActive()) {
        // Don't drain SDL relative-mouse state here — the flying camera's
        // preUpdate needs to read it when it is active.
        return;
    }
    tempCameraOrbitOverride();

    // ── One-shot key events (OR-accumulate) — both camera modes ──────
    if (engine::input.pressed == KEY_1) playerInput.ability1 = true;
    if (engine::input.pressed == KEY_2) playerInput.ability2 = true;
    if (engine::input.pressed == KEY_5) playerInput.ability5 = true;
    if (engine::input.pressed == KEY_T) playerInput.tKey = true;

    // ── Camera mode toggle (C key) ────────────────────────────────────
    if (engine::input.pressed == KEY_C) {
        float currentCameraDistance = (gCameraMode == CAM_MODE_ISO)
                                          ? topDownCameraGetDistance()
                                          : thirdPersonCameraGetDistance();
        gCameraMode                 = (gCameraMode == CAM_MODE_ISO) ? CAM_MODE_ORBIT : CAM_MODE_ISO;
        playerDbSaveCameraState(currentCameraDistance);
        if (gCameraMode == CAM_MODE_ISO) {
            Player* p = getComponent(playerScene, Player, playerEntityId);
            if (p) {
                p->moveYaw   = topDownCameraGetYaw();
                p->facingYaw = p->moveYaw;
            }
            utils::info("camera: switched to isometric");
        } else {
            utils::info("camera: switched to orbit");
        }
    }

    // ── Middle-mouse auto-run toggle — both camera modes ─────────────
    bool middleDown = engine::windowSystemIsMiddleMouseDown();
    if (middleDown && !piPrevMiddleMouse) playerInput.autoRunToggle = true;
    piPrevMiddleMouse = middleDown;

    // Hoisted above the ISO-mode `goto read_held_keys;` (C++ forbids jumping
    // past an initialization into a variable's scope).
    float rawDx         = 0.0f;
    float rawDy         = 0.0f;
    bool anyDragNow     = false;
    bool anyDragBefore  = false;

    // In top-down mode, delegate camera to TopDownCamera system
    if (gCameraMode == CAM_MODE_ISO) {
        topDownCameraPreUpdate();
        playerInput.scrollY += engine::input.scrollY;

        playerInput.leftMouse  = engine::windowSystemIsLeftMouseDown();
        playerInput.rightMouse = engine::windowSystemIsRightMouseDown();

        piPrevLeftMouse  = playerInput.leftMouse;
        piPrevRightMouse = playerInput.rightMouse;

        goto read_held_keys;
    }

    if (engine::input.pressed == KEY_5) {
        // windowSystemWarpCenter();
        // float x, y;
        // SDL_GetMouseState(&x, &y);
        // info("%f %f", x, y);
        SDL_SetWindowMouseGrab(engine::window.sdlWindowHandle,
                               !SDL_GetWindowMouseGrab(engine::window.sdlWindowHandle));
    }

    // Drain relative-mouse state so deltas never go stale.
    engine::windowSystemGetRelativeMouseDelta(&rawDx, &rawDy);

    // ── Scroll wheel (sum) ───────────────────────────────────────────
    playerInput.scrollY += engine::input.scrollY;

    // ── Mouse buttons ────────────────────────────────────────────────
    playerInput.rightMouse = engine::windowSystemIsRightMouseDown();
    playerInput.leftMouse  = engine::windowSystemIsLeftMouseDown();
    anyDragNow  = playerInput.rightMouse || playerInput.leftMouse;
    anyDragBefore = piPrevRightMouse || piPrevLeftMouse;

    // Cursor show/hide on mouse-button transitions.
    // Use SDL relative mouse mode — it works on Wayland (where warping
    // is not supported) and automatically restores the cursor position
    // when the mode is exited.
    if (anyDragNow && !anyDragBefore) {
        engine::windowSystemHideCursor();
        // Drain any stale delta from the mode switch.
        engine::windowSystemGetRelativeMouseDelta(&rawDx, &rawDy);
    } else if (!anyDragNow && anyDragBefore) {
        engine::windowSystemShowCursor();
    }

    // Accumulate relative mouse only during an ongoing drag.
    if (anyDragNow && anyDragBefore) {
        playerInput.mouseDx += rawDx;
        playerInput.mouseDy += rawDy;
    }

    piPrevRightMouse = playerInput.rightMouse;
    piPrevLeftMouse  = playerInput.leftMouse;

    // ── Held keys (latest state — these persist across frames) ───────
read_held_keys:
    playerInput.moveW = engine::input.repeating[KEY_W];
    playerInput.moveS = engine::input.repeating[KEY_S];
    playerInput.moveA = engine::input.repeating[KEY_A];
    playerInput.moveD = engine::input.repeating[KEY_D];
    playerInput.jump  = engine::input.repeating[KEY_SPACE];
    playerInput.shift = engine::input.shift;
    playerInput.alt   = engine::input.alt;
    playerInput.ctrl  = engine::input.ctrl;
}

// ── Abilities ───────────────────────────────────────────────────────────────

// ── Footstep detection ──────────────────────────────────────────────────────
static const float footRayLength = 0.15f;  // short downward ray from foot
static const float stepVolume    = 0.05f;

static bool footOnGround(engine::Entity* footBone) {
    if (!footBone) return false;
    engine::WorldTransform* w = getComponent(footBone->scene, engine::WorldTransform, footBone->id);
    if (!w) return false;

    vec3 origin = {w->pos[0], w->pos[1], w->pos[2]};
    vec3 down   = {0.0f, -1.0f, 0.0f};
    vec3 hit;
    return joltCastRay(origin, down, footRayLength, hit);
}

static void playerFootsteps(engine::Entity* playerEntity, bool isMoving) {
    if (!stepSound || !isMoving) return;

    static bool leftWasUp  = true;
    static bool rightWasUp = true;

    engine::Entity* leftFoot  = engine::animationGetBoneEntity(playerEntity, "mixamorig:LeftFoot");
    engine::Entity* rightFoot = engine::animationGetBoneEntity(playerEntity, "mixamorig:RightFoot");

    bool leftDown  = footOnGround(leftFoot);
    bool rightDown = footOnGround(rightFoot);

    if (leftWasUp && leftDown) engine::soundPlay(stepSound, stepVolume, 0);
    if (rightWasUp && rightDown) engine::soundPlay(stepSound, stepVolume, 0);

    leftWasUp  = !leftDown;
    rightWasUp = !rightDown;
}

// ── Kick hit detection ──────────────────────────────────────────────────────
static const float kickHitRadius = 0.3f;   // sphere around the foot
static const float kickImpulse   = 20.0f;  // impulse strength (kg·m/s)

static void kickHitDetection(engine::Entity* playerEntity) {
    engine::Entity* footBone = engine::animationGetBoneEntity(playerEntity, "mixamorig:LeftFoot");
    if (!footBone) return;

    // Get foot world position from the bone's world transform
    engine::WorldTransform* footWorld = getComponent(playerEntity->scene, engine::WorldTransform, footBone->id);
    if (!footWorld) return;

    vec3 footPos;
    glm_vec3_copy(footWorld->pos, footPos);

    // Sphere overlap at foot position
    JoltOverlapHit hits[8];
    u32 hitCount = joltSphereOverlap(footPos, kickHitRadius, hits, 8);

    for (u32 i = 0; i < hitCount; i++) {
        // Build impulse direction: from foot toward hit contact point (outward)
        vec3 dir;
        glm_vec3_sub(hits[i].contactPoint, footPos, dir);
        if (glm_vec3_norm2(dir) < 0.0001f) {
            // Fallback: push outward from player center
            engine::Transform* playerTransform =
                getComponent(playerEntity->scene, engine::Transform, playerEntity->id);
            glm_vec3_sub(hits[i].contactPoint, playerTransform->pos, dir);
        }
        if (glm_vec3_norm2(dir) < 0.0001f) continue;
        glm_vec3_normalize(dir);

        // Add upward bias so objects fly up a bit
        dir[1] += 0.5f;
        glm_vec3_normalize(dir);

        vec3 impulse;
        glm_vec3_scale(dir, kickImpulse, impulse);

        if (joltBodyAddImpulseAt(hits[i].bodyId, impulse, hits[i].contactPoint)) {
            if (hitSound) engine::soundPlay(hitSound, 1.0f, 0);
        }
    }
}

static void kickLightAttach(engine::Entity* playerEntity) {
    engine::Entity* foot = engine::animationGetBoneEntity(playerEntity, "mixamorig:LeftFoot");
    if (!foot) return;

    // If already fading from a previous kick, reuse the existing component
    if (kickLightEntity && kickLightEntity->id == foot->id) {
        engine::Light* light = getComponent(foot->scene, engine::Light, foot->id);
        if (light) {
            light->intensity = kickLightIntensity;
            kickLightFade    = 0.0f;  // no longer fading
            engine::lightMarkDirty(foot->scene, foot->id);
            return;
        }
    }

    // Remove any stale light from a different entity
    if (kickLightEntity) {
        engine::F_sceneRemoveComponent(kickLightEntity->scene, kickLightEntity->id, &engine::Light_id);
        engine::lightMarkDirty(kickLightEntity->scene, kickLightEntity->id);
        kickLightEntity = nullptr;
    }

    engine::Light* light     = createComponent(foot->scene, engine::Light, foot->id);
    light->lightType = engine::LIGHT_POINT;
    vec3 kickLightColor = {1.0f, 0.6f, 0.2f};
    glm_vec3_copy(kickLightColor, light->color);  // warm orange
    light->intensity = kickLightIntensity;
    light->range     = 8.0f;
    kickLightFade    = 0.0f;
    engine::lightMarkDirty(foot->scene, foot->id);
    kickLightEntity = foot;
}

static void kickLightStartFade(void) {
    if (!kickLightEntity) return;
    kickLightFade = kickLightFadeDuration;
}

// Call every frame — dims the light while fading, removes when done.
static void kickLightUpdate(void) {
    if (!kickLightEntity || kickLightFade <= 0.0f) return;

    kickLightFade -= utils::timer.dt;
    if (kickLightFade <= 0.0f) {
        // Fully faded — remove the light
        kickLightFade = 0.0f;
        engine::Scene* scene  = kickLightEntity->scene;
        u32 id        = kickLightEntity->id;
        engine::F_sceneRemoveComponent(scene, id, &engine::Light_id);
        engine::lightMarkDirty(scene, id);
        kickLightEntity = nullptr;
        return;
    }

    // Dim intensity proportionally
    engine::Light* light = getComponent(kickLightEntity->scene, engine::Light, kickLightEntity->id);
    if (light) {
        float t          = kickLightFade / kickLightFadeDuration;  // 1→0
        light->intensity = kickLightIntensity * t * t;             // quadratic ease-out
        engine::lightMarkDirty(kickLightEntity->scene, kickLightEntity->id);
    }
}

// ── Player follow light ───────────────────────────────────────────────────
[[maybe_unused]]
static void playerLightCreate(engine::Entity* playerEntity) {
    if (!playerEntity || playerLightEntity) return;

    engine::Transform* playerTransform = getComponent(playerEntity->scene, engine::Transform, playerEntity->id);
    if (!playerTransform) return;

    playerLightEntity = engine::createEntity(playerEntity->scene, "player_light");

    engine::Transform* transform = createComponent(playerEntity->scene, engine::Transform, playerLightEntity->id);
    glm_quat_identity(transform->rot);
    glm_vec3_copy(playerTransform->pos, transform->pos);
    transform->pos[0] += 1.0f;
    transform->pos[1] += playerLightHeight;
    transform->pos[3] = 1.0f;

    engine::Light* light        = createComponent(playerEntity->scene, engine::Light, playerLightEntity->id);
    light->lightType    = engine::LIGHT_POINT;
    light->color[0]     = 1.0f;
    light->color[1]     = 0.86f;
    light->color[2]     = 0.65f;
    light->intensity    = playerLightIntensity;
    light->range        = 6.0f;
    light->castsShadows = false;
    engine::lightMarkDirty(playerEntity->scene, playerLightEntity->id);
}

static void playerLightUpdate(void) {
    if (!playerLightEntity || !playerReady || !playerScene) return;

    engine::Transform* playerTransform = getComponent(playerScene, engine::Transform, playerEntityId);
    engine::Transform* lightTransform =
        getComponent(playerLightEntity->scene, engine::Transform, playerLightEntity->id);
    if (!playerTransform || !lightTransform) return;

    glm_vec3_copy(playerTransform->pos, lightTransform->pos);
    // lightTransform->pos[0] -= 1.0f;
    lightTransform->pos[1] += playerLightHeight;
    // lightTransform->pos[3] = 1.0f;
}

// ── Fireball (KEY_5: throw a point-light projectile) ─────────────────────
static void fireballCreate(void) {
    if (fireballCount >= FIREBALL_MAX) return;

    Player* player       = getComponent(playerScene, Player, playerEntityId);
    engine::Transform* transform = getComponent(playerScene, engine::Transform, playerEntityId);
    if (!player || !transform) return;

    // Spawn position: slightly above and in front of the player
    vec3 spawnPos;
    glm_vec3_copy(transform->pos, spawnPos);
    spawnPos[1] += 1.5f;  // chest height

    // Direction: player's facing direction (from moveYaw)
    vec3 forward = {sinf(player->moveYaw), 0.0f, cosf(player->moveYaw)};

    // Velocity = direction * speed
    vec3 velocity;
    glm_vec3_scale(forward, FIREBALL_SPEED, velocity);

    // Create entity in the player's scene
    engine::Entity* entity = engine::createEntity(playerScene, "fireball");

    // Add Transform
    engine::Transform* t = createComponent(playerScene, engine::Transform, entity->id);
    glm_quat_identity(t->rot);
    glm_vec3_copy(spawnPos, t->pos);
    t->pos[3] = 1.0f;

    // Add a point light (orange-yellow fireball glow)
    engine::Light* light        = createComponent(playerScene, engine::Light, entity->id);
    light->lightType    = engine::LIGHT_POINT;
    light->color[0]     = 1.0f;
    light->color[1]     = 0.5f;
    light->color[2]     = 0.1f;
    light->intensity    = FIREBALL_INTENSITY;
    light->range        = FIREBALL_RANGE;
    light->castsShadows = false;
    engine::lightMarkDirty(playerScene, entity->id);

    // Store in fireball array
    Fireball* fb = &fireballs[fireballCount++];
    fb->entity   = entity;
    fb->hitboxEntity =
        combatCreateHitbox(entity, 0.8f, 20.0f, DAMAGE_TYPE_FIRE, playerEntityId, 0, 0.0f);
    glm_vec3_copy(velocity, fb->velocity);
    fb->lifetime = FIREBALL_LIFETIME;
}

static void fireballUpdate(void) {
    int write = 0;
    for (int read = 0; read < fireballCount; read++) {
        Fireball* fb = &fireballs[read];
        if (!fb->entity) continue;

        fb->lifetime -= utils::timer.dt;
        if (fb->lifetime <= 0.0f) {
            // Destroy hitbox
            if (fb->hitboxEntity) {
                engine::Scene* hbScene = fb->hitboxEntity->scene;
                u32 hbId       = fb->hitboxEntity->id;
                engine::F_sceneRemoveComponent(hbScene, hbId, &AttackHitbox_id);
                engine::F_sceneRemoveComponent(hbScene, hbId, &engine::Transform_id);
                engine::destroyEntity(fb->hitboxEntity);
                fb->hitboxEntity = nullptr;
            }
            // Queue entity for destruction in postUpdate (safe to call destroyEntity there)
            fireballDestroyList[fireballDestroyCount++] = fb->entity;
            fb->entity                                  = nullptr;
            continue;
        }

        // Move the fireball
        engine::Transform* t = getComponent(fb->entity->scene, engine::Transform, fb->entity->id);
        if (t) {
            t->pos[0] += fb->velocity[0] * utils::timer.dt;
            t->pos[1] += fb->velocity[1] * utils::timer.dt;
            t->pos[2] += fb->velocity[2] * utils::timer.dt;
        }

        // Update hitbox position to follow the fireball
        if (fb->hitboxEntity) {
            engine::Transform* hbTransform =
                getComponent(fb->hitboxEntity->scene, engine::Transform, fb->hitboxEntity->id);
            if (hbTransform && t) {
                glm_vec3_copy(t->pos, hbTransform->pos);
            }
        }

        // Pulse and fade the light
        engine::Light* light = getComponent(fb->entity->scene, engine::Light, fb->entity->id);
        if (light) {
            float lifeRatio = fb->lifetime / FIREBALL_LIFETIME;  // 1→0
            // Pulse: sine wave modulated by lifetime
            float pulse      = 0.7f + 0.3f * sinf(fb->lifetime * 20.0f);
            light->intensity = FIREBALL_INTENSITY * pulse * lifeRatio;
            // Shift color from yellow-white (hot) to red (cooling)
            light->color[0] = 1.0f;
            light->color[1] = 0.5f + 0.3f * lifeRatio;
            light->color[2] = 0.1f * lifeRatio;
            engine::lightMarkDirty(fb->entity->scene, fb->entity->id);
        }

        // Keep compact array (pack alive fireballs to front)
        if (write != read) fireballs[write] = *fb;
        write++;
    }
    fireballCount = write;
}

static void playerAbilities(void) {
    Player* player        = getComponent(playerScene, Player, playerEntityId);
    CharacterStats* stats = getComponent(playerScene, CharacterStats, playerEntityId);
    engine::Entity* playerEntity  = engine::getEntity(playerScene, playerEntityId);
    if (!player || !playerEntity) return;

    // If a cast is in progress, check whether the animation finished
    if (player->isCasting) {
        // Run hit detection every frame while kicking
        kickHitDetection(playerEntity);

        // Restart slightly before the animation finishes so the reset
        // happens while it is still in motion (avoids a one-frame pause
        // since playerSystem runs before animationSystem each frame).
        bool nearEnd = engine::animationIsNearEnd(playerEntity, castingAnim, 0.05f);

        if (nearEnd && castingRepeatsLeft > 0) {
            // Replay early to avoid the one-frame pause at the end pose
            castingRepeatsLeft--;
            engine::animationRestart(playerEntity, castingAnim, ANIM_SPEED_CAST_REPLAY);
        } else if (engine::animationIsFinished(playerEntity, castingAnim)) {
            if (castingRepeatsLeft > 0) {
                // Safety fallback: animation finished but repeats remain
                castingRepeatsLeft--;
                engine::animationRestart(playerEntity, castingAnim, ANIM_SPEED_CAST_REPLAY);
            } else {
                // Destroy kick damage hitbox
                if (kickHitboxEntity) {
                    engine::Scene* hbScene = kickHitboxEntity->scene;
                    u32 hbId       = kickHitboxEntity->id;
                    engine::F_sceneRemoveComponent(hbScene, hbId, &AttackHitbox_id);
                    engine::F_sceneRemoveComponent(hbScene, hbId, &engine::Transform_id);
                    engine::destroyEntity(kickHitboxEntity);
                    kickHitboxEntity = nullptr;
                }
                player->isCasting = false;
                castingAnim       = nullptr;
                kickLightStartFade();
                // Transition back to idle/move
                if (player->isMoving)
                    engine::animationPlayBlended(playerEntity, ANIM_RUN, ANIM_SPEED_RUN, true, 0.2f);
                else
                    engine::animationPlayBlended(playerEntity, ANIM_IDLE, ANIM_SPEED_IDLE, true, 0.2f);
            }
        }
        // Update kick hitbox position to follow the foot bone
        if (kickHitboxEntity) {
            engine::Entity* footBone = engine::animationGetBoneEntity(playerEntity, "mixamorig:LeftFoot");
            if (footBone) {
                engine::WorldTransform* footWorld =
                    getComponent(playerEntity->scene, engine::WorldTransform, footBone->id);
                engine::Transform* hbTransform =
                    getComponent(kickHitboxEntity->scene, engine::Transform, kickHitboxEntity->id);
                if (footWorld && hbTransform) {
                    glm_vec3_copy(footWorld->pos, hbTransform->pos);
                }
            }
        }
        return;  // block new casts while one is playing
    }

    // Key 1: Flying Hurricane Kick
    if (playerInput.ability1) {
        isTposing          = false;
        player->isCasting  = true;
        castingAnim        = ANIM_HURRICANE_KICK;
        castingRepeatsLeft = 1;
        engine::animationPlayBlended(playerEntity, castingAnim, ANIM_SPEED_HURRICANE_KICK, false, 0.15f);
        if (kickSound) engine::soundPlay(kickSound, 1.0f, 0);
        kickLightAttach(playerEntity);
        kickHitboxEntity = combatCreateHitbox(playerEntity,
                                              kickHitRadius,
                                              stats->damage,
                                              DAMAGE_TYPE_PHYSICAL,
                                              playerEntityId,
                                              0,
                                              0.3f);
    }

    // Key 2: Flying Bicycle Kick (repeats 3 times)
    if (playerInput.ability2) {
        isTposing          = false;
        player->isCasting  = true;
        castingAnim        = ANIM_BICYCLE_KICK;
        castingRepeatsLeft = 2;  // plays once + 2 repeats = 3 total
        engine::animationPlayBlended(playerEntity, castingAnim, ANIM_SPEED_BICYCLE_KICK, false, 0.15f);
        if (bicycleKickSound) engine::soundPlay(bicycleKickSound, 1.0f, 0);
        kickHitboxEntity = combatCreateHitbox(playerEntity,
                                              kickHitRadius,
                                              stats->damage,
                                              DAMAGE_TYPE_PHYSICAL,
                                              playerEntityId,
                                              0,
                                              0.3f);
    }

    // Key 5: Fireball (point-light projectile)
    if (playerInput.ability5) {
        fireballCreate();
    }
}

// ── Update: movement + camera orbit ─────────────────────────────────────────

static const char* playerLocomotionClips[] = {
    ANIM_RUN,
    ANIM_RUN_BACK,
    ANIM_STRAFE_LEFT,
    ANIM_STRAFE_RIGHT,
};

static bool playerIsLocomotionClip(const char* clipName) {
    for (size_t i = 0; i < sizeof(playerLocomotionClips) / sizeof(playerLocomotionClips[0]); i++) {
        if (utils::strequals(clipName, playerLocomotionClips[i])) return true;
    }
    return false;
}

static engine::AnimationInstance* playerFindAnimationInstance(engine::Animator* animator, const char* clipName) {
    if (!animator) return nullptr;
    for (i32 i = static_cast<i32>(static_cast<i32>(animator->activeInstances.size())) - 1; i >= 0; i--) {
        engine::AnimationInstance* instance = animator->activeInstances[i];
        if (instance && !instance->markedForRemoval &&
            utils::strequals(instance->clip->name.data, clipName)) {
            return instance;
        }
    }
    return nullptr;
}

static float playerLocomotionPhase(engine::Animator* animator) {
    if (!animator) return 0.0f;
    for (size_t i = 0; i < sizeof(playerLocomotionClips) / sizeof(playerLocomotionClips[0]); i++) {
        engine::AnimationInstance* instance =
            playerFindAnimationInstance(animator, playerLocomotionClips[i]);
        if (instance && instance->clip->duration > 0.0f) {
            return fmodf(instance->currentTime / instance->clip->duration, 1.0f);
        }
    }
    return 0.0f;
}

static void playerSetAnimationWeight(engine::Animator* animator,
                                     const char* clipName,
                                     float speed,
                                     float targetWeight,
                                     float blendDuration,
                                     float phase) {
    engine::AnimationInstance* instance = playerFindAnimationInstance(animator, clipName);
    if (!instance && targetWeight <= 0.001f) return;

    if (!instance) {
        engine::AnimationClip* clip = engine::animationGet(clipName);
        if (!clip) {
            utils::warn("player: locomotion clip '%s' not found", clipName);
            return;
        }

        instance  = new engine::AnimationInstance{};
        instance->clip             = clip;
        instance->currentTime      = phase * clip->duration;
        instance->speed            = speed;
        instance->loop             = true;
        instance->weight           = 0.0f;
        instance->markedForRemoval = false;
        animator->activeInstances.push_back(instance);
    }

    instance->speed             = speed;
    instance->loop              = true;
    instance->blendWeightStart  = instance->weight;
    instance->blendWeightTarget = targetWeight;
    instance->blendDuration     = blendDuration;
    instance->blendElapsed      = 0.0f;
}

static bool playerCursorYawAtTransform(engine::Transform* transform, float* outYaw) {
    if (!transform || !outYaw || gCameraMode != CAM_MODE_ISO) return false;

    vec3 rayOrigin, rayDir;
    topDownCameraUnproject(engine::input.xpos, engine::input.ypos, rayOrigin, rayDir);

    // Intersect with horizontal ground plane at player's Y level.
    if (fabsf(rayDir[1]) <= 0.001f) return false;

    float groundY = transform->pos[1];
    float t       = (groundY - rayOrigin[1]) / rayDir[1];
    if (t <= 0.0f) return false;

    vec3 cursorTarget;
    cursorTarget[0] = rayOrigin[0] + rayDir[0] * t;
    cursorTarget[1] = groundY;
    cursorTarget[2] = rayOrigin[2] + rayDir[2] * t;

    vec3 toCursor;
    glm_vec3_sub(cursorTarget, transform->pos, toCursor);
    toCursor[1] = 0.0f;
    if (glm_vec3_norm2(toCursor) <= 0.001f) return false;

    *outYaw = atan2f(toCursor[0], toCursor[2]);
    return true;
}

static void playerPlayLocomotionBlend(engine::Entity* entity,
                                      float facingYaw,
                                      vec3 velocity,
                                      float speed,
                                      float blendDuration) {
    engine::Animator* animator = getComponent(entity->scene, engine::Animator, entity->id);
    if (!animator) {
        animator          = createComponent(entity->scene, engine::Animator, entity->id);
        animator->entity  = entity;
        animator->mapping = nullptr;
    }

    // Fade out idle/emotes/etc.  Ability and jump animations do not call this path.
    for (size_t i = 0; i < animator->activeInstances.size(); i++) {
        engine::AnimationInstance* instance = animator->activeInstances[i];
        if (!instance || instance->markedForRemoval) continue;
        if (playerIsLocomotionClip(instance->clip->name.data)) continue;

        instance->blendWeightStart  = instance->weight;
        instance->blendWeightTarget = 0.0f;
        instance->blendDuration     = blendDuration;
        instance->blendElapsed      = 0.0f;
    }

    vec3 moveDir = {velocity[0], 0.0f, velocity[2]};
    if (glm_vec3_norm2(moveDir) <= 0.0001f) return;
    glm_vec3_normalize(moveDir);

    vec3 forward  = {sinf(facingYaw), 0.0f, cosf(facingYaw)};
    vec3 leftAxis = {cosf(facingYaw), 0.0f, -sinf(facingYaw)};

    float forwardAmount = glm_vec3_dot(forward, moveDir);
    float sideAmount    = glm_vec3_dot(leftAxis, moveDir);

    float forwardWeight = fmaxf(forwardAmount, 0.0f);
    float backWeight    = fmaxf(-forwardAmount, 0.0f);
    float leftWeight    = fmaxf(sideAmount, 0.0f);
    float rightWeight   = fmaxf(-sideAmount, 0.0f);
    float totalWeight   = forwardWeight + backWeight + leftWeight + rightWeight;
    if (totalWeight > 0.0001f) {
        forwardWeight /= totalWeight;
        backWeight /= totalWeight;
        leftWeight /= totalWeight;
        rightWeight /= totalWeight;
    }

    float phase = playerLocomotionPhase(animator);

    // PoE-style backward movement: use the forward run clip in reverse instead
    // of a separate backward-run animation.  Forward/back weights are mutually
    // exclusive for a normalized movement direction, so one ANIM_RUN instance
    // can represent either direction by changing playback sign.
    float runWeight = forwardWeight;
    float runSpeed  = speed;
    if (backWeight > forwardWeight) {
        runWeight = backWeight;
        runSpeed  = -speed;
    }

    playerSetAnimationWeight(animator, ANIM_RUN, runSpeed, runWeight, blendDuration, phase);
    playerSetAnimationWeight(animator, ANIM_RUN_BACK, speed, 0.0f, blendDuration, phase);
    playerSetAnimationWeight(animator, ANIM_STRAFE_LEFT, speed, leftWeight, blendDuration, phase);
    playerSetAnimationWeight(animator, ANIM_STRAFE_RIGHT, speed, rightWeight, blendDuration, phase);
}

// Movement-only variant for ISO camera mode (no camera positioning)
static void playerMovementTopDownOnly(void) {
    Player* player             = getComponent(playerScene, Player, playerEntityId);
    engine::Transform* transform       = getComponent(playerScene, engine::Transform, playerEntityId);
    engine::Entity* cameraEntity       = engine::cameraGetEntity();
    engine::Camera* camera             = getComponent(engine::ecs.defaultScene, engine::Camera, cameraEntity->id);
    engine::Transform* cameraTransform = getComponent(engine::ecs.defaultScene, engine::Transform, cameraEntity->id);

    if (!player || !transform || !camera || !cameraTransform || !player->character) return;

    // Save the previous pose BEFORE modifying the player/root transform.
    // Saving after the movement update makes the root snap to the new pose
    // immediately, which breaks render-frame interpolation and reads as
    // shaky character motion.
    engine::transformActivateAndSaveLastSubtree(playerScene, playerEntityId);

    // ── Mouse look (deltas accumulated in preUpdate) ──────────────────
    {
        float dx = playerInput.mouseDx;
        float dy = playerInput.mouseDy;
        if (dx != 0.0f || dy != 0.0f) {
            player->cameraYaw -= dx * cameraSensitivity * utils::timer.dt;
            player->cameraPitch += dy * cameraSensitivity * utils::timer.dt;
            player->cameraPitch = glm_clamp(player->cameraPitch, pitchMin, pitchMax);
        }
    }

    // Scroll wheel to zoom (third-person orbit camera)
    if (gCameraMode != CAM_MODE_ISO) {
        float zoomDistance = thirdPersonCameraGetDistance();
        if (playerInput.scrollY != 0) {
            zoomDistance -= playerInput.scrollY * 2.0f;
            zoomDistance = glm_clamp(zoomDistance, 1.5f, 20.0f);
            thirdPersonCameraSetDistance(zoomDistance);
        }
    }

    // ── Auto-run toggle (middle mouse button) ─────────────────────────
    if (playerInput.autoRunToggle) {
        player->autoRun = !player->autoRun;
    }

    // ── WASD input ──────────────────────────────────────────────────────
    float moveX = 0.0f;
    float moveZ = 0.0f;

    if (playerInput.moveW) moveZ += 1.0f;
    if (playerInput.moveS) moveZ -= 1.0f;
    if (playerInput.moveA) moveX += 1.0f;
    if (playerInput.moveD) moveX -= 1.0f;

    // W/S cancels auto-run (A/D strafe does not)
    if (moveZ != 0.0f && player->autoRun) {
        player->autoRun = false;
    }

    // Auto-run moves toward the cursor in ISO mode, allowing mouse steering.
    if (player->autoRun) {
        if (gCameraMode == CAM_MODE_ISO) {
            float cursorYaw;
            if (playerCursorYawAtTransform(transform, &cursorYaw)) {
                player->moveYaw = cursorYaw;
            }
        }
        moveZ = 1.0f;
    }

    bool moving = (moveX != 0.0f || moveZ != 0.0f);
    thirdPersonCameraSetMoving(moving);

    // Movement forward/right on XZ plane (derived from moveYaw)
    float moveYaw   = player->moveYaw;
    vec3 camForward = {sinf(moveYaw), 0.0f, cosf(moveYaw)};
    vec3 camRight   = {cosf(moveYaw), 0.0f, -sinf(moveYaw)};

    // Build desired horizontal velocity
    vec3 desiredVel = {0, 0, 0};
    float speed     = player->moveSpeed;
    if (moving) {
        glm_vec3_muladds(camForward, moveZ, desiredVel);
        glm_vec3_muladds(camRight, moveX, desiredVel);
        glm_vec3_normalize(desiredVel);

        if (playerInput.shift) speed = MOVE_SPEED_WALK;
        if (playerInput.alt) speed *= MOVE_SPEED_SPRINT_MULT;

        if (player->isCasting && castingAnim && strcmp(castingAnim, ANIM_BICYCLE_KICK) == 0)
            speed *= MOVE_SPEED_BICYCLE_MULT;

        glm_vec3_scale(desiredVel, speed, desiredVel);
    }

    // Jump
    int groundState = joltCharacterGetGroundState(player->character);
    bool onGround   = (groundState == JOLT_GROUND_STATE_ON_GROUND);
    if (onGround && playerInput.jump) {
        desiredVel[1] = jumpSpeed;
        if (jumpSound) engine::soundPlay(jumpSound, 1.0f, 0);
    }

    // Update Jolt character
    // Hold the character at its spawn position until the streaming heightfield
    // body under it exists (Azgaar world). While waiting, skip the physics
    // update so the character cannot fall through the terrain before the
    // collision data is ready. In the regular (mesh) world the active
    // heightmap is nullptr, so this clears on the first update.
    if (player->waitingForGround) {
        engine::HeightmapTerrain* hm = engine::heightmapTerrainGetActive();
        if (!hm) {
            player->waitingForGround = false;  // no heightmap world: ground is a mesh
        } else if (engine::heightmapTerrainHasBodyAt(hm, transform->pos[0], transform->pos[2])) {
            player->waitingForGround = false;
            utils::info("player: ground body ready, releasing character");
        } else {
            // No ground yet: keep the character pinned at its spawn position.
            if (player->sensorBody) {
                joltBodySetPosition(player->sensorBody, transform->pos);
            }
            return;
        }
    }
    joltCharacterUpdate(player->character, desiredVel, utils::timer.dt);

    // Read back position from Jolt
    float charPos[3];
    joltCharacterGetPosition(player->character, charPos);
    transform->pos[0] = charPos[0];
    transform->pos[1] = charPos[1];
    transform->pos[2] = charPos[2];

    // Sync sensor body so overlap queries can find the player
    if (player->sensorBody) {
        joltBodySetPosition(player->sensorBody, charPos);
    }

    // While moving, rotate the whole character toward movement direction so
    // regular forward-run locomotion looks natural.  When idle in ISO mode,
    // face the cursor so attacks/aiming still line up with the mouse.
    // Compose yaw rotation on top of the base upright rotation so the
    // glTF coordinate-system correction (e.g. Blender Z-up → Y-up) is preserved.
    // Skip rotation while casting so ability animations are not disrupted.
    float animFacingYaw = player->moveYaw;
    if (!player->isCasting) {
        float targetYaw = 0.0f;
        bool hasTarget  = false;

        if (moving) {
            vec3 moveDir;
            glm_vec3_copy(desiredVel, moveDir);
            moveDir[1] = 0.0f;
            if (glm_vec3_norm2(moveDir) > 0.001f) {
                glm_vec3_normalize(moveDir);
                targetYaw = atan2f(moveDir[0], moveDir[2]);
                hasTarget = true;
            }
        } else if (gCameraMode == CAM_MODE_ISO) {
            hasTarget = playerCursorYawAtTransform(transform, &targetYaw);
        }

        if (hasTarget) {
            animFacingYaw     = targetYaw;
            player->facingYaw = targetYaw;

            versor yawQuat;
            glm_quatv(yawQuat, targetYaw, Y_UP);
            versor targetRot;
            glm_quat_mul(yawQuat, player->baseRot, targetRot);
            engine::quatSlerpShortest(transform->rot,
                              targetRot,
                              glm_clamp(player->turnSpeed * utils::timer.dt, 0.0f, 1.0f),
                              transform->rot);
            glm_quat_normalize(transform->rot);
        }
    }

    // ── Animation state (skip while casting an ability) ────────────────
    engine::Entity* playerEntity = engine::getEntity(playerScene, playerEntityId);

    if (!player->isCasting) {
        // Jump takes priority over ground animations
        if (!onGround && !player->isJumping) {
            engine::animationPlayBlended(playerEntity, ANIM_JUMP, ANIM_SPEED_JUMP, false, 0.25f);
            player->isJumping = true;
        } else if (onGround && player->isJumping) {
            // Landed — transition back to the appropriate ground animation
            player->isJumping = false;
            if (moving) {
                if (!playerInput.shift)
                    playerPlayLocomotionBlend(playerEntity,
                                              animFacingYaw,
                                              desiredVel,
                                              ANIM_SPEED_RUN,
                                              0.2f);
                else
                    engine::animationPlayBlended(playerEntity, ANIM_WALK, ANIM_SPEED_WALK, true, 0.2f);
            } else {
                engine::animationPlayBlended(playerEntity, ANIM_IDLE, ANIM_SPEED_IDLE, true, 0.2f);
            }
        }

        // Ground movement animations (only when not jumping)
        if (!player->isJumping) {
            // T-key emote: play eve_t until the character moves
            if (playerInput.tKey && !moving && !isTposing) {
                engine::animationPlayBlended(playerEntity, ANIM_TPOSE, ANIM_SPEED_TPOSE, true, 0.3f);
                isTposing = true;
            }
            if (moving && isTposing) {
                isTposing = false;
            }

            if (moving && !player->isMoving) {
                if (!playerInput.shift) {
                    playerPlayLocomotionBlend(playerEntity,
                                              animFacingYaw,
                                              desiredVel,
                                              ANIM_SPEED_RUN,
                                              0.2f);
                } else {
                    engine::animationPlayBlended(playerEntity, ANIM_WALK, ANIM_SPEED_WALK, true, 0.2f);
                }
                player->isMoving = true;
            } else if (!moving && player->isMoving) {
                engine::animationPlayBlended(playerEntity, ANIM_IDLE, ANIM_SPEED_IDLE, true, 0.2f);
                player->isMoving = false;
            } else if (moving) {
                if (!playerInput.shift) {
                    playerPlayLocomotionBlend(playerEntity,
                                              animFacingYaw,
                                              desiredVel,
                                              ANIM_SPEED_RUN,
                                              0.2f);
                } else {
                    if (!engine::animationIsPlaying(playerEntity, ANIM_WALK)) {
                        engine::animationPlayBlended(playerEntity, ANIM_WALK, ANIM_SPEED_WALK, true, 0.2f);
                    }
                }
            }
        }
    }

    // ── Footstep sounds ───────────────────────────────────────────────
    playerFootsteps(playerEntity, moving && !player->isCasting && !player->isJumping);
}

// Full movement + orbit camera (used when CAM_MODE_ORBIT is active)
static void playerMovement(void) {
    // Run movement (handles WASD, physics, animation, etc.)
    playerMovementTopDownOnly();

    Player* player = getComponent(playerScene, Player, playerEntityId);
    if (!player) return;

    // Update moveYaw: sync to cameraYaw only when right-click is held.
    if (playerInput.rightMouse) {
        player->moveYaw   = player->cameraYaw;
        player->facingYaw = player->moveYaw;
    }

    // Sync player angles to third-person camera
    thirdPersonCameraSetAngles(player->cameraYaw, player->cameraPitch);
    thirdPersonCameraSetAnyDrag(playerInput.rightMouse || playerInput.leftMouse);
    thirdPersonCameraSetMouseDy(playerInput.mouseDy);
    thirdPersonCameraUpdate();

    // Read back camera angles for next frame
    player->cameraYaw   = thirdPersonCameraGetYaw();
    player->cameraPitch = thirdPersonCameraGetPitch();
}

static void playerFollowFlyingCamera(void) {
    Player* player       = getComponent(playerScene, Player, playerEntityId);
    engine::Transform* transform = getComponent(playerScene, engine::Transform, playerEntityId);
    engine::Entity* camEntity    = engine::cameraGetEntity();
    engine::Camera* camera       = getComponent(engine::ecs.defaultScene, engine::Camera, camEntity->id);
    engine::Transform* camT      = getComponent(engine::ecs.defaultScene, engine::Transform, camEntity->id);
    if (!player || !transform || !camT) return;

    vec3 direction = {};
    glm_vec3_copy(camera->cameraUbo.renderDirection, direction);
    glm_vec3_scale(direction, 2, direction);
    glm_vec3_add(direction, camT->pos, direction);

    // Place the player at the camera position.
    transform->pos[0] = direction[0];
    transform->pos[1] = camT->pos[1] - 3;
    transform->pos[2] = direction[2];
    // glm_vec3_copy(camT->pos, transform->pos);
    // transform->pos[3] = 1.0f;

    // Sync the Jolt character controller so it doesn't rubber-band back
    // when the player regains control.
    if (player->character) {
        joltCharacterSetPosition(player->character, transform->pos);
    }
    if (player->sensorBody) {
        joltBodySetPosition(player->sensorBody, transform->pos);
    }

    // Keep orbit camera angles in sync with the engine camera so the
    // transition back to player control is seamless.
    engine::Camera* cam = getComponent(engine::ecs.defaultScene, engine::Camera, camEntity->id);
    if (cam) {
        player->cameraYaw   = cam->yaw;
        player->cameraPitch = glm_clamp(cam->pitch, pitchMin, pitchMax);
        player->moveYaw     = cam->yaw;
        player->facingYaw   = player->moveYaw;
        thirdPersonCameraSetAngles(player->cameraYaw, player->cameraPitch);
    }

    // playerLightUpdate();
}

void PlayerSystem::update() {
    if (!playerReady) return;

    // Track the active camera for the water grid recentering and the GPU
    // weather state machine (the weather pass reads cameras[0] directly on
    // the GPU; this drives the climate→condition cross-fade + shared gust).
    // Weather runs first so the water update below sees this frame's gust.
    engine::Entity* camEntity = engine::cameraGetEntity();
    if (camEntity) {
        engine::Camera* cam = getComponent(engine::ecs.defaultScene, engine::Camera, camEntity->id);
        if (cam) {
            azgaarWeatherUpdate(cam->cameraUbo.renderLocation[0],
                                cam->cameraUbo.renderLocation[1],
                                cam->cameraUbo.renderLocation[2]);
            azgaarWaterUpdate(cam->cameraUbo.renderLocation[0],
                            cam->cameraUbo.renderLocation[2]);
        }
    }

    if (engine::flyingCameraIsActive()) {
        return;
    }

    // Top-down camera mode: skip orbit camera code in playerMovement(),
    // use TopDownCamera system for camera positioning
    if (gCameraMode == CAM_MODE_ISO) {
        playerAbilities();
        kickLightUpdate();
        fireballUpdate();

        // In ISO mode, movement direction always follows the top-down camera yaw
        // so W moves "up" on screen regardless of camera rotation.
        Player* _p = getComponent(playerScene, Player, playerEntityId);
        if (_p) _p->moveYaw = topDownCameraGetYaw();

        playerMovementTopDownOnly();
        // playerLightUpdate();

        topDownCameraScrollZoom(playerInput.scrollY);

        topDownCameraUpdate();
    } else {
        playerAbilities();
        kickLightUpdate();
        fireballUpdate();
        playerMovement();
        playerLightUpdate();
    }

    // Clear accumulated one-shot / summed inputs after consumption.
    playerInput.ability1      = false;
    playerInput.ability2      = false;
    playerInput.ability5      = false;
    playerInput.tKey          = false;
    playerInput.autoRunToggle = false;
    playerInput.scrollY       = 0.0f;
    playerInput.mouseDx       = 0.0f;
    playerInput.mouseDy       = 0.0f;
}

void PlayerSystem::postUpdate() {
    // Destroy expired fireball entities (safe in postUpdate)
    for (int i = 0; i < fireballDestroyCount; i++) {
        engine::Entity* entity = fireballDestroyList[i];
        if (entity) {
            engine::Scene* scene = entity->scene;
            u32 id       = entity->id;
            engine::F_sceneRemoveComponent(scene, id, &engine::Light_id);
            engine::F_sceneRemoveComponent(scene, id, &engine::Transform_id);
            engine::lightMarkDirty(scene, id);
            engine::destroyEntity(entity);
        }
    }
    fireballDestroyCount = 0;

    if (!playerReady) return;

    static double lastSave;
    double now = utils::millies();

    if (engine::flyingCameraIsActive()) {
        playerFollowFlyingCamera();
        if (now > lastSave + 1000) {
            lastSave             = now;
            engine::Transform* transform = getComponent(playerScene, engine::Transform, playerEntityId);
            if (transform) engine::transformDbSave("player", transform);
        }
        return;
    }

    if (now > lastSave + 1000) {
        lastSave = now;

        engine::Transform* transform = getComponent(playerScene, engine::Transform, playerEntityId);
        engine::transformDbSave("player", transform);

        float cameraDistance = (gCameraMode == CAM_MODE_ISO) ? topDownCameraGetDistance()
                                                             : thirdPersonCameraGetDistance();
        playerDbSaveCameraState(cameraDistance);
    }
}
}  // namespace game
