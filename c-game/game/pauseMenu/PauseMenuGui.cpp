#include "PauseMenuGui.h"
#include "../settingsGui/SettingsGui.h"
#include "gameState/GameState.h"
#include "ecs/system/System.h"
#include "ecs/system/lua/LuaSystem.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "rmlui/wrapper/src/crmlui.h"

static void added(void);
static void removed(void);

System pauseMenuGui = {
    .name                = "pauseMenu",
    .added               = added,
    .removed             = removed,
    .preUpdate           = nullptr,
    .update              = nullptr,
    .postUpdate          = nullptr,
    .cpuElapsedLastFrame = 0.0,
    .cpuElapsed          = 0.0,
    .gpuElapsed          = 0.0,
    .priority            = 0,
};

static void* document;

static int pauseReturnToGame(void* _);
static int pauseSettingsOpen(void* _);
static int pauseExitGame(void* _);

void added(void) {
    luaRegisterFunction("pauseReturnToGame", pauseReturnToGame);
    luaRegisterFunction("pauseSettingsOpen", pauseSettingsOpen);
    luaRegisterFunction("pauseExitGame", pauseExitGame);

    document = rmlNewDocument("gui/pauseMenu/pauseMenu.html");
    rmlLoadDocument(document);
    rmlShowDocument(document);
}

void removed(void) {
    rmlUnloadDocument(document);
    document = nullptr;
}

char pauseMenuGuiIsShowing(void) {
    return document != nullptr;
}

int pauseReturnToGame(void* _) {
    static_cast<void>(_);
    guiManagerRemoveGuiNextFrame(&pauseMenuGui);
    return 0;
}

int pauseSettingsOpen(void* _) {
    static_cast<void>(_);
    if (!settingsGuiIsShowing()) {
        guiManagerRemoveGuiNextFrame(&pauseMenuGui);
        guiManagerAddGuiNextFrame(&settingsGui);
    }
    return 0;
}

int pauseExitGame(void* _) {
    guiManagerRemoveGuiNextFrame(&pauseMenuGui);
    static_cast<void>(_);
    gameStateTransition(STATE_MAIN_MENU);
    return 0;
}
