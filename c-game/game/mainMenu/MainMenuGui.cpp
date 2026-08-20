#include "MainMenuGui.h"
#include "credits/Credits.h"
#include "../settingsGui/SettingsGui.h"
#include "gameState/GameState.h"
#include "ecs/system/System.h"
#include "ecs/system/lua/LuaSystem.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "rmlui/wrapper/src/crmlui.h"

static void added(void);
static void removed(void);

System mainMenuGui = {
    .name                = "mainMenu",
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
static void* model;

static int creditsOpen(void* _);
static int luaPlayGame(void* _);

void added(void) {
    luaRegisterFunction("settingsOpen", settingsOpen);
    luaRegisterFunction("creditsOpen", creditsOpen);
    luaRegisterFunction("playGame", luaPlayGame);

    document = rmlNewDocument("gui/mainMenu/mainMenu.html");
    model    = rmlCreateModel("mainMenu");

    rmlLoadDocument(document);
    rmlShowDocument(document);
}

void removed(void) {
    warn("remove main menu gui");
    rmlUnloadDocument(document);
    document = nullptr;
    rmlUnloadModel(model);
    model = nullptr;
}

int settingsOpen(void* _) {
    if (!settingsGuiIsShowing()) {
        guiManagerAddGuiNextFrame(&settingsGui);
    }
    return 0;
}

int creditsOpen(void* _) {
    guiManagerAddGuiNextFrame(&creditsGui);
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
