#include <stdlib.h>
#include "gameState/GameState.h"
#include "Game.h"
#include "ecs/Ecs.h"
#include "ecs/system/System.h"
#include "ecs/system/scene/SceneSystem.h"
#include "ecs/system/animation/AnimatorComponent.h"
#include "ecs/system/window/WindowSystem.h"
#include "ecs/system/camera/flyingCamera/FlyingCamera.h"
#include "renderer/Renderer.h"
#include "player/Player.h"
#include "character/CharacterSystem.h"
#include "combat/Combat.h"
#include "enemy/EnemySystem.h"
#include "navmesh/NavMeshSystem.h"
#include "hud/Hud.h"
#include "compassGui/CompassGui.h"
#include "zoneGui/ZoneGui.h"
#include "azgaar/AzgaarCellTracker.h"
#include "mainMenu/MainMenu.h"
#include "loadingAzgaar/LoadingAzgaar.h"
#include "azgaar/AzgaarStreaming.h"
#include "pauseMenu/PauseMenuGui.h"
#include "settingsGui/SettingsGui.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "events/Events.h"
#include "timer/Timer.h"

namespace game {
static GameState currentState;
static GameState prevState;
static float transitionProgress;
// TEMP DEBUG: ENGINE_TEST_REENTRY drives an unattended
// main menu -> loading -> gameplay -> main menu -> loading ... cycle.
static bool testReentryActive;
static double testStateEnterTime;

// timer.timeSinceStart is in nanoseconds.
#define TEST_REENTRY_MENU_WAIT_NS     (2.0 * BILLION)  // wait in the main menu (first start and after re-entry)
#define TEST_REENTRY_GAMEPLAY_WAIT_NS (2.0 * BILLION)  // wait after entering the world completes

#define STATE_TABLE_SIZE 8
static StateCallbacks stateTable[STATE_TABLE_SIZE];

static GameplayLoadState gameplayLoadState;

// Assets transferred from Loading state
static engine::Scene* gameplayScene;
static engine::Scene* gameplayAnimationsScene;
static engine::Scene* gameplayPlayerScene;

static void gameStateRendererInitialized(void*);

// ── Asset transfer setters (called by the loading state) ───────────────────

void gameStateSetLoadedScene(engine::Scene* scene) {
    gameplayScene = scene;
}

void gameStateSetLoadedAnimationsScene(engine::Scene* scene) {
    gameplayAnimationsScene = scene;
}

// ── Registration ─────────────────────────────────────────────────────────────

void gameStateRegisterInternal(GameState state, StateCallbacks callbacks) {
    if (static_cast<int>(state) >= STATE_TABLE_SIZE) {
        utils::error("gameStateRegisterInternal: state %d out of bounds (max %d)",
              static_cast<int>(state),
              STATE_TABLE_SIZE - 1);
        return;
    }
    stateTable[state] = callbacks;
}

// ── Init ─────────────────────────────────────────────────────────────────────

void gameStateInit(void) {
    transitionProgress      = 1.0f;
    gameplayLoadState       = GAMEPLAY_LOADED_NONE;
    gameplayScene           = nullptr;
    gameplayAnimationsScene = nullptr;
    gameplayPlayerScene     = nullptr;

    gameStateRegisterInternal(STATE_MAIN_MENU,
                              StateCallbacks{
                                  .enter  = gameStateMainMenuEnter,
                                  .exit   = gameStateMainMenuExit,
                                  .update = gameStateMainMenuUpdate,
                              });

    gameStateRegisterInternal(STATE_LOADING_AZGAAR,
                              StateCallbacks{
                                  .enter  = gameStateLoadingAzgaarEnter,
                                  .exit   = gameStateLoadingAzgaarExit,
                                  .update = gameStateLoadingAzgaarUpdate,
                              });

    gameStateRegisterInternal(STATE_GAMEPLAY,
                              StateCallbacks{
                                  .enter  = gameStateGameplayEnter,
                                  .exit   = gameStateGameplayExit,
                                  .update = gameStateGameplayUpdate,
                              });

    testReentryActive = getenv("ENGINE_TEST_REENTRY") != nullptr;

    utils::signalSubscribe("rendererInitialized", gameStateRendererInitialized);
}

static void gameStateSkipToLoading(void) {
    gameStateTransition(STATE_LOADING_AZGAAR);
}

static void gameStateRendererInitialized(void* _) {
    static_cast<void>(_);
    // Auto-skip main menu for automated testing (screenshots, logs, etc.)
    if (getenv("ENGINE_SKIP_MAIN_MENU")) {
        utils::futureTaskAddNoParam(500, gameStateSkipToLoading);
    }
    gameStateTransition(STATE_MAIN_MENU);
}

// ── Transition ───────────────────────────────────────────────────────────────

void gameStateTransition(GameState target) {
    // Ignore requests to enter the state we are already in — including while
    // a transition into it is still in progress.  A double click on "Enter
    // World" used to re-run the loading state's enter() mid-transition:
    // systems were added to the ECS twice and the in-flight world load was
    // torn down while async workers were still using it static_cast<SIGSEGV>(.)
    if (target == currentState) return;

    prevState          = currentState;
    transitionProgress = 0.0f;

    if (currentState != target && stateTable[currentState].exit) {
        stateTable[currentState].exit();
    }

    currentState = target;
    testStateEnterTime = utils::timer.timeSinceStart;  // TEMP DEBUG: ENGINE_TEST_REENTRY

    if (stateTable[currentState].enter) {
        stateTable[currentState].enter();
    }
}

// ── Update ───────────────────────────────────────────────────────────────────

void gameStateUpdate(void) {
    // TEMP DEBUG: ENGINE_TEST_REENTRY drives the crash repro without input:
    // main menu (3 s) -> loading -> gameplay (3 s after world entry completes)
    // -> main menu (3 s) -> re-enter world.
    if (testReentryActive) {
        double elapsed = utils::timer.timeSinceStart - testStateEnterTime;
        if (currentState == STATE_MAIN_MENU && elapsed >= TEST_REENTRY_MENU_WAIT_NS) {
            utils::info("testReentry: main menu -> loading");
            gameStateTransition(STATE_LOADING_AZGAAR);
        } else if (currentState == STATE_GAMEPLAY && elapsed >= TEST_REENTRY_GAMEPLAY_WAIT_NS) {
            utils::info("testReentry: gameplay -> main menu");
            gameStateTransition(STATE_MAIN_MENU);
        }
    }

    if (transitionProgress < 1.0f) {
        transitionProgress += utils::timer.dt;
        if (transitionProgress > 1.0f) transitionProgress = 1.0f;
    }

    if (stateTable[currentState].update) {
        stateTable[currentState].update();
    }
}

GameState gameStateCurrent(void) {
    return currentState;
}

GameplayLoadState gameStateGameplayLoadState(void) {
    return gameplayLoadState;
}

/* ── Main Menu callbacks ─────────────────────────────────────────────────── */

void gameStateMainMenuEnter(void) {
    engine::systemAdd(gameSystem.priority + 1, &mainMenu);
}

void gameStateMainMenuExit(void) {
    engine::systemRemove(&mainMenu);
}

void gameStateMainMenuUpdate(void) {
    // F8 debug shortcut to skip straight to gameplay
    if (engine::input.pressed == KEY_F8) {
        gameStateTransition(STATE_LOADING_AZGAAR);
    }
}

/* ── Azgaar loading callbacks ─────────────────────────────────────────────── */

void gameStateLoadingAzgaarEnter(void) {
    // Reuse the normal player bootstrap, but keep Azgaar loading isolated from
    // the existing .dat terrain/scene loader.
    engine::systemAddNow(gameSystem.priority + 2, &playerSystem);
    engine::systemAddNow(gameSystem.priority + 1, &loadingAzgaarSystem);
    loadingAzgaarOnEnter();
}

void gameStateLoadingAzgaarExit(void) {
    loadingAzgaarOnExit();
    engine::systemRemove(&loadingAzgaarSystem);
}

void gameStateLoadingAzgaarUpdate(void) {
    // Input is handled inside loadingAzgaarSystem.update()
}

/* ── Gameplay callbacks ──────────────────────────────────────────────────── */

void gameStateGameplayEnter(void) {
    gameplayLoadState = GAMEPLAY_LOADED_READY;
    engine::flyingCameraLoadForGameplay();
    utils::signalEmit("gameLoaded", nullptr);
    // playerSystem was already added during STATE_LOADING
    engine::systemAdd(gameSystem.priority + 1, &characterSystem);
    engine::systemAdd(gameSystem.priority + 1, &combatSystem);
    engine::systemAdd(gameSystem.priority + 1, &enemySystem);
    engine::systemAdd(gameSystem.priority + 1, &navMeshSystem);
    engine::systemAdd(gameSystem.priority + 1, &azgaarStreamingSystem);
    engine::systemAdd(gameSystem.priority + 1, &azgaarCellTrackerSystem);
    engine::guiManagerAddGuiNextFrame(&hud);
    engine::guiManagerAddGuiNextFrame(&compassGui);
    engine::guiManagerAddGuiNextFrame(&zoneGui);
}

void gameStateGameplayExit(void) {
    engine::systemRemove(&characterSystem);
    engine::systemRemove(&combatSystem);
    engine::systemRemove(&enemySystem);
    engine::systemRemove(&navMeshSystem);
    engine::systemRemove(&azgaarStreamingSystem);
    engine::systemRemove(&azgaarCellTrackerSystem);
    engine::systemRemove(&playerSystem);
    engine::guiManagerRemoveGuiNextFrame(&hud);
    engine::guiManagerRemoveGuiNextFrame(&compassGui);
    engine::guiManagerRemoveGuiNextFrame(&zoneGui);

    // Destroy Vulkan backends + free CPU data for each scene.
    // (playerScene is destroyed by playerSystem.removed())
    if (gameplayAnimationsScene) {
        engine::rendererSceneDestroy(gameplayAnimationsScene);
        engine::sceneDestroy(gameplayAnimationsScene);
        gameplayAnimationsScene = nullptr;
    }
    if (gameplayScene) {
        engine::rendererSceneDestroy(gameplayScene);
        engine::sceneDestroy(gameplayScene);
        gameplayScene = nullptr;
    }
    // gameplayPlayerScene is owned by playerSystem; it cleans up itself.
    gameplayPlayerScene = nullptr;

    // Free the world data retained for streaming during gameplay (also
    // detaches the heightmap and destroys grass/water/roads).
    loadingAzgaarReleaseWorld();

    gameplayLoadState = GAMEPLAY_LOADED_NONE;
}

void gameStateGameplayUpdate(void) {
    if (engine::input.pressed == KEY_ESCAPE) {
        // Only add if no menu is showing: while the pause menu or the settings
        // chain is open (settingsGui stays registered — sub-pages only hide its
        // document), ESC is handled by the focused RMLUI document
        // (pauseKeyDown / settingsKeyDown / *SettingsKeyDown -> *Close).
        // Re-adding here would race the removal and pop the game menu on top
        // of settings (addGui does not dedupe).
        if (!pauseMenuGuiIsShowing() && !settingsGuiIsShowing()) {
            engine::guiManagerAddGuiNextFrame(&pauseMenuGui);
        }
        return;
    }

    // Track the player scene once it becomes available (loaded async)
    if (!gameplayPlayerScene) {
        gameplayPlayerScene = getPlayerScene();
    }
}
}  // namespace game
