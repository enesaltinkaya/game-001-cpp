#include "MainMenuGui.h"
#include "MainMenuGui.h"
#include "credits/Credits.h"
#include "../settingsGui/SettingsGui.h"
#include "gameState/GameState.h"
#include "ecs/system/System.h"
#include "ecs/system/lua/LuaSystem.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "rmlui/wrapper/src/crmlui.h"

namespace game {

MainMenuGui mainMenuGui;

MainMenuGui::MainMenuGui() : engine::System("mainMenu") {}

static void* document;
static void* model;

static int creditsOpen(void* _);
static int luaPlayGame(void* _);

void MainMenuGui::added() {
    engine::luaRegisterFunction("settingsOpen", settingsOpen);
    engine::luaRegisterFunction("creditsOpen", creditsOpen);
    engine::luaRegisterFunction("playGame", luaPlayGame);

    document = rmlNewDocument("gui/mainMenu/mainMenu.html");
    model    = rmlCreateModel("mainMenu");

    rmlLoadDocument(document);
    rmlShowDocument(document);
}

void MainMenuGui::removed() {
    utils::warn("remove main menu gui");
    rmlUnloadDocument(document);
    document = nullptr;
    rmlUnloadModel(model);
    model = nullptr;
}

int settingsOpen(void* _) {
    if (!settingsGuiIsShowing()) {
        engine::guiManagerAddGuiNextFrame(&settingsGui);
    }
    return 0;
}

int creditsOpen(void* _) {
    engine::guiManagerAddGuiNextFrame(&creditsGui);
    return 0;
}

int luaPlayGame(void* _) {
    static_cast<void>(_);
    playGame();
    return 0;
}

void playGame(void) {
    gameStateTransition(STATE_LOADING_AZGAAR);
}
}  // namespace game
