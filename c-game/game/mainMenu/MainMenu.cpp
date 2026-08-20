#include "MainMenu.h"

#include "MainMenuGui.h"
#include "ecs/system/System.h"
#include "ecs/system/window/WindowSystem.h"
#include "ecs/system/sound/SoundSystem.h"
#include "gameState/GameState.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "settings/Settings.h"
#include "thread/Thread.h"

namespace game {
static void loadMusic(void* _);
static void adoptMusic(void* _);

MainMenu mainMenu;

MainMenu::MainMenu() : engine::System("mainMenu") {}

// Main-thread-owned: points at the music sound of the current menu session,
// nullptr once it is destroyed.  The async load (loadMusic) hands its result to
// adoptMusic via a future task so this pointer is never touched from a worker
// thread (re-entering the menu while a previous load is still in flight used
// to destroy a dangling sound and crash in SoLoud).
static engine::Sound* music;

void MainMenu::added() {
    utils::threadPoolAddWork(nullptr, loadMusic, 0);
    engine::guiManagerAddGuiNextFrame(&mainMenuGui);
}

void MainMenu::preUpdate() {
    if (engine::input.pressed == KEY_ESCAPE) {
        settingsOpen(0);
    }
}

void MainMenu::removed() {
    engine::guiManagerRemoveGuiNextFrame(&mainMenuGui);
    if (music) {
        engine::soundDestroy(music);
        music = nullptr;
    }
}

// Runs on a worker thread: only the slow part (pak read + decode).  The
// Sound travels to the main thread through the future-task queue.
static void loadMusic(void* _) {
    engine::Sound* s = engine::soundLoad("sound/Dark Descent (Extended Cut).ogg");
    utils::futureTaskAdd(0, adoptMusic, s);
}

// Runs on the main thread: adopt the freshly loaded music for the current
// menu session, or discard it if the menu was already left.
static void adoptMusic(void* pSound) {
    engine::Sound* s  = static_cast<engine::Sound*>(pSound);
    if (gameStateCurrent() != STATE_MAIN_MENU) {
        engine::soundDestroy(s);
        return;
    }
    // A stale load from a previous session may have been adopted first;
    // replace it so at most one menu music is ever alive.
    if (music) {
        engine::soundDestroy(music);
        music = nullptr;
    }
    music   = s;
    engine::soundPlay(music, utils::settingsGetDouble("music") / 100., 1);
}
}  // namespace game
