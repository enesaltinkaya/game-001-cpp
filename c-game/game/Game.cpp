#include "Game.h"
#include "ecs/Ecs.h"
#include "ecs/system/System.h"
#include "events/Events.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "c-game/game/cameraGui/CameraGui.h"
#include "c-game/game/loadingAzgaar/LoadingAzgaar.h"
#include "c-game/game/playerGui/PlayerGui.h"
#include "c-game/game/playerActionsGui/PlayerActionsGui.h"

static void added(void);
static void removed(void);
static void preUpdate(void);
static void update(void);
static void postUpdate(void);
static void onAzgaarMapLoaded(void* _);

struct System gameSystem = {
    .name       = "game",
    .added      = added,
    .removed    = removed,
    .preUpdate  = preUpdate,
    .update     = update,
    .postUpdate = postUpdate,
};

static void autoEnter(void*) {
    gameStateTransition(STATE_LOADING_AZGAAR);
}

static void gameRendererInitialized(void* _) {
    (void)_;
    // futureTaskAdd(500, autoEnter, NULL);
    guiManagerAddGuiNextFrame(&cameraGui);
    guiManagerAddGuiNextFrame(&playerGui);
}

// The player actions GUI is only useful once the Azgaar map is loaded, so add
// it when the "azgaarMapLoaded" signal fires instead of at renderer init
// (which would show it over the main menu). The flag guards against aw
// second Azgaar load re-adding an already-active GUI (addGui does not dedupe).
static char playerActionsGuiAdded;

static void onAzgaarMapLoaded(void* _) {
    if (playerActionsGuiAdded) return;
    playerActionsGuiAdded = 1;
    guiManagerAddGuiNextFrame(&playerActionsGui);
}

// The retained Azgaar world is only released through the loading state's exit
// path — which a plain shutdown during gameplay never goes through — leaving
// its climate GPU textures alive when the renderer destroys the device.
// Release it at teardown instead. Higher priority than the render system
// (10000), so ecsDestroy() (reverse order) runs this before the renderer
// disposes Vulkan.
static void azgaarWorldCleanupRemoved(void);

struct System azgaarWorldCleanupSystem = {
    .name    = "azgaarWorldCleanup",
    .removed = azgaarWorldCleanupRemoved,
};

static void azgaarWorldCleanupRemoved(void) {
    loadingAzgaarReleaseWorld();
}

void added(void) {
    gameStateInit();
    signalSubscribe("rendererInitialized", gameRendererInitialized);
    signalSubscribe("azgaarMapLoaded", onAzgaarMapLoaded);
    systemAddNow(10500, &azgaarWorldCleanupSystem);
}

void removed(void) {}

void preUpdate(void) {}

void update(void) {
    gameStateUpdate();
}

void postUpdate(void) {}
