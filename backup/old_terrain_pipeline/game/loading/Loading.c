#include "Loading.h"
#include "gameState/GameState.h"
#include "ecs/system/terrain/TerrainParser.h"
#include "ecs/system/scene/SceneParser.h"
#include "ecs/system/scene/SceneSystem.h"
#include "ecs/system/window/WindowSystem.h"
#include "renderer/Renderer.h"
#include "player/Player.h"
#include "events/Events.h"
#include "timer/Timer.h"
#include "futuretask/FutureTask.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "rmlui/wrapper/src/crmlui.h"

// Forward declare to avoid including AnimatorComponent.h (EventCallback name clash with crmlui.h)
extern void animationPlayBlendedByName(const char* entityName,
                                       const char* clipName,
                                       float speed,
                                       bool loop,
                                       float blendDuration);

static void added(void);
static void removed(void);
static void update(void);

struct System loadingSystem = {
    .name    = "loading",
    .added   = added,
    .removed = removed,
    .update  = update,
};

// ── Loading stages ──────────────────────────────────────────────────────────

typedef enum {
    LOAD_STAGE_TERRAIN,
    LOAD_STAGE_ANIMATIONS,
    LOAD_STAGE_SCENE,
    LOAD_STAGE_READY,
} LoadStage;

static LoadStage loadStage;
static char cancelled;
static double enterTime;  // timer.timeSinceStart when loading entered

// Minimum time (seconds) the loading screen must be visible
static const double MIN_LOADING_DISPLAY_TIME = 1.0;

// Loaded assets — transferred to GameState on ready, cleaned up on cancel/exit
static Terrain* loadedTerrain;
static Scene* loadedScene;
static Scene* loadedAnimationsScene;

static const char* stageTexts[] = {
    [LOAD_STAGE_TERRAIN]    = "Loading terrain...",
    [LOAD_STAGE_ANIMATIONS] = "Loading animations...",
    [LOAD_STAGE_SCENE]      = "Loading scene...",
    [LOAD_STAGE_READY]      = "Ready!",
};

const char* loadingStageText(void) {
    return stageTexts[loadStage];
}

// ── RmlUi state ─────────────────────────────────────────────────────────────

static void* document;
static void* model;
static char stageTextBuf[64] = "test";
static char* stageTextPtr    = stageTextBuf;

// ── Helpers ─────────────────────────────────────────────────────────────────

static void checkReady(void) {
    // Only advance to ready if both the gameplay scene and player scene are loaded
    if (loadStage != LOAD_STAGE_READY && loadedScene && loadedScene->ready &&
        getPlayerScene() != NULL) {
        loadStage = LOAD_STAGE_READY;
    }
}

// ── Async load callbacks ────────────────────────────────────────────────────

static void gameplayAnimationsLoaded(Scene* scene, void* _) {
    (void)_;
    if (cancelled) {
        rendererSceneDestroy(scene);
        sceneDestroy(scene);
        return;
    }
    loadedAnimationsScene = scene;
    scene->alwaysVisible  = true;
    animationPlayBlendedByName("eve_animator", "female_walk", 0.1f, 1, 1);
    signalEmit("animationsLoaded", NULL);
    loadStage = LOAD_STAGE_SCENE;
    checkReady();
}

static void gameplayTerrainLoaded(Terrain* terrain, void* _) {
    (void)_;
    if (cancelled) {
        terrainDestroy(terrain);
        return;
    }
    loadedTerrain = terrain;
    loadStage     = LOAD_STAGE_TERRAIN;
    sceneLoadCb("models/animations.dat", gameplayAnimationsLoaded, NULL);
}

static void gameplaySceneLoaded(Scene* scene, void* _) {
    (void)_;
    if (cancelled) {
        rendererSceneDestroy(scene);
        sceneDestroy(scene);
        return;
    }
    scene->alwaysVisible = false;
    loadedScene          = scene;
    checkReady();
}

// ── Enter / Exit (called by GameState transitions) ──────────────────────────

void loadingOnEnter(void) {
    cancelled             = 0;
    loadStage             = LOAD_STAGE_TERRAIN;
    enterTime             = timer.timeSinceStart;
    loadedTerrain         = NULL;
    loadedScene           = NULL;
    loadedAnimationsScene = NULL;

    // Start the async load chain:
    //  - terrain → animations → signal → player (by Player.c)
    //  - scene (test2.dat) loads in parallel
    terrainLoadCb("models/terrain/oghuzlands.dat", gameplayTerrainLoaded, NULL);
    sceneLoadCb("models/test2.dat", gameplaySceneLoaded, NULL);
    sceneLoad("models/test3.dat");
}

void loadingOnExit(void) {
    cancelled = 1;

    // Clean up assets if we are exiting because of a cancel (back to main menu)
    if (loadStage != LOAD_STAGE_READY) {
        if (loadedTerrain) {
            terrainDestroy(loadedTerrain);
            loadedTerrain = NULL;
        }
        if (loadedScene) {
            rendererSceneDestroy(loadedScene);
            sceneDestroy(loadedScene);
            loadedScene = NULL;
        }
        if (loadedAnimationsScene) {
            rendererSceneDestroy(loadedAnimationsScene);
            sceneDestroy(loadedAnimationsScene);
            loadedAnimationsScene = NULL;
        }
    }
    // If LOAD_STAGE_READY, assets are transferred to GameState — don't free.
}

// Called by GameState when transitioning LOADING -> GAMEPLAY
void loadingTransferAssets(void) {
    gameStateSetLoadedTerrain(loadedTerrain);
    gameStateSetLoadedScene(loadedScene);
    gameStateSetLoadedAnimationsScene(loadedAnimationsScene);

    // Clear our pointers so removed() won't double-free
    loadedTerrain         = NULL;
    loadedScene           = NULL;
    loadedAnimationsScene = NULL;
}

// ── System lifecycle ────────────────────────────────────────────────────────

void added(void) {
    document = rmlNewDocument("gui/loading/loading.html");
    model    = rmlCreateModel("loading");
    rmlBindCharPointer(model, "stage", &stageTextPtr);
    rmlLoadDocument(document);
    rmlShowDocument(document);
}

void removed(void) {
    // Clean up any remaining assets (safety net)
    if (loadedTerrain) {
        terrainDestroy(loadedTerrain);
        loadedTerrain = NULL;
    }
    if (loadedScene) {
        rendererSceneDestroy(loadedScene);
        sceneDestroy(loadedScene);
        loadedScene = NULL;
    }
    if (loadedAnimationsScene) {
        rendererSceneDestroy(loadedAnimationsScene);
        sceneDestroy(loadedAnimationsScene);
        loadedAnimationsScene = NULL;
    }

    rmlUnloadDocument(document);
    document = NULL;
    rmlUnloadModel(model);
    model = NULL;
}

void update(void) {
    // If the loading state was already exited (e.g. transition to GAMEPLAY
    // happened via a deferred systemRemove), stop processing immediately.
    // Without this guard the loading update runs one more frame before the
    // deferred removal fires, calls gameStateTransition(STATE_GAMEPLAY)
    // again, and double-adds all gameplay systems + HUD.
    if (cancelled) return;

    // Re-check ready every frame — async completions (scene, player) can
    // finish in any order, and cached assets may resolve before the player
    // scene is available.
    checkReady();

    // Update the stage text binding
    snprintf(stageTextBuf, sizeof(stageTextBuf), "%s", stageTexts[loadStage]);
    rmlUpdateDirtyAll(model);

    // ESC to cancel → back to main menu
    if (input.pressed == KEY_ESCAPE) {
        gameStateTransition(STATE_MAIN_MENU);
        return;
    }

    // Auto-transition when everything is loaded and minimum display time has passed
    if (loadStage == LOAD_STAGE_READY &&
        (timer.timeSinceStart - enterTime) >= MIN_LOADING_DISPLAY_TIME) {
        loadingTransferAssets();
        gameStateTransition(STATE_GAMEPLAY);
    }
}
