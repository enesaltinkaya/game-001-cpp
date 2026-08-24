#include "PauseMenuGui.h"
#include "PauseMenuGui.h"
#include "../settingsGui/SettingsGui.h"
#include "gameState/GameState.h"
#include "ecs/system/System.h"
#include "ecs/system/lua/LuaSystem.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "rmlui/wrapper/src/crmlui.h"

namespace game {

PauseMenuGui pauseMenuGui;

PauseMenuGui::PauseMenuGui() : engine::System("pauseMenu") {}

static void* document;

static int pauseReturnToGame(void* _);
static int pauseSettingsOpen(void* _);
static int pauseExitGame(void* _);

void PauseMenuGui::added() {
    engine::luaRegisterFunction("pauseReturnToGame", pauseReturnToGame);
    engine::luaRegisterFunction("pauseSettingsOpen", pauseSettingsOpen);
    engine::luaRegisterFunction("pauseExitGame", pauseExitGame);

    document = rmlNewDocument("gui/pauseMenu/pauseMenu.html");
    rmlLoadDocument(document);
    rmlShowDocument(document);
}

void PauseMenuGui::removed() {
    rmlUnloadDocument(document);
    document = nullptr;
}

char pauseMenuGuiIsShowing(void) {
    return document != nullptr;
}

int pauseReturnToGame(void* _) {
    static_cast<void>(_);
    engine::guiManagerRemoveGuiNextFrame(&pauseMenuGui);
    return 0;
}

int pauseSettingsOpen(void* _) {
    static_cast<void>(_);
    if (!settingsGuiIsShowing()) {
        engine::guiManagerRemoveGuiNextFrame(&pauseMenuGui);
        engine::guiManagerAddGuiNextFrame(&settingsGui);
    }
    return 0;
}

int pauseExitGame(void* _) {
    engine::guiManagerRemoveGuiNextFrame(&pauseMenuGui);
    static_cast<void>(_);
    gameStateTransition(STATE_MAIN_MENU);
    return 0;
}
}  // namespace game
