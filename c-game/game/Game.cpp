#include "Game.h"
#include "Game.h"
#include "ecs/Ecs.h"
#include "ecs/system/System.h"
#include "events/Events.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "c-game/game/cameraGui/CameraGui.h"
#include "c-game/game/loadingAzgaar/LoadingAzgaar.h"
#include "c-game/game/playerGui/PlayerGui.h"
#include "c-game/game/playerActionsGui/PlayerActionsGui.h"

namespace game {
static void onAzgaarMapLoaded(void* _);

GameSystem gameSystem;

GameSystem::GameSystem() : engine::System("game") {}

[[maybe_unused]]
static void autoEnter(void*) {
    gameStateTransition(STATE_LOADING_AZGAAR);
}

static void gameRendererInitialized(void* _) {
    static_cast<void>(_);
    // futureTaskAdd(500, autoEnter, nullptr);
    engine::guiManagerAddGuiNextFrame(&cameraGui);
    engine::guiManagerAddGuiNextFrame(&playerGui);
}

// The player actions GUI is only useful once the Azgaar map is loaded, so add
// it when the "azgaarMapLoaded" signal fires instead of at renderer init
// (which would show it over the main menu). The flag guards against aw
// second Azgaar load re-adding an already-active GUI (addGui does not dedupe).
static char playerActionsGuiAdded;

static void onAzgaarMapLoaded(void* _) {
    if (playerActionsGuiAdded) return;
    playerActionsGuiAdded = 1;
    engine::guiManagerAddGuiNextFrame(&playerActionsGui);
}

// The retained Azgaar world is only released through the loading state's exit
// path — which a plain shutdown during gameplay never goes through — leaving
// its climate GPU textures alive when the renderer destroys the device.
// Release it at teardown instead. Higher priority than the render system
// (10000), so ecsDestroy() (reverse order) runs this before the renderer
// disposes Vulkan.

class AzgaarWorldCleanupSystem : public engine::System {
public:
    AzgaarWorldCleanupSystem();
    void removed() override;
};

AzgaarWorldCleanupSystem azgaarWorldCleanupSystem;

AzgaarWorldCleanupSystem::AzgaarWorldCleanupSystem() : engine::System("azgaarWorldCleanup") {}

void AzgaarWorldCleanupSystem::removed() {
    loadingAzgaarReleaseWorld();
}

void GameSystem::added() {
    gameStateInit();
    utils::signalSubscribe("rendererInitialized", gameRendererInitialized);
    utils::signalSubscribe("azgaarMapLoaded", onAzgaarMapLoaded);
    engine::systemAddNow(10500, &azgaarWorldCleanupSystem);
}

void GameSystem::removed() {}

void GameSystem::preUpdate() {}

void GameSystem::update() {
    gameStateUpdate();
}

void GameSystem::postUpdate() {}
}  // namespace game
