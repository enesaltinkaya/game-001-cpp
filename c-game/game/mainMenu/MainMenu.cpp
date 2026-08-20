
#include "MainMenuGui.h"
#include "ecs/system/System.h"
#include "ecs/system/window/WindowSystem.h"
#include "ecs/system/sound/SoundSystem.h"
#include "gameState/GameState.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "settings/Settings.h"
#include "thread/Thread.h"

static void added(void);
static void preUpdate(void);
static void removed(void);
static void loadMusic(void* _);
static void adoptMusic(void* _);

struct System mainMenu = {
    .name      = "mainMenu",
    .added     = added,
    .preUpdate = preUpdate,
    .removed   = removed,
};

// Main-thread-owned: points at the music sound of the current menu session,
// NULL once it is destroyed.  The async load (loadMusic) hands its result to
// adoptMusic via a future task so this pointer is never touched from a worker
// thread (re-entering the menu while a previous load is still in flight used
// to destroy a dangling sound and crash in SoLoud).
static struct Sound* music;

void added(void) {
    threadPoolAddWork(NULL, loadMusic, 0);
    guiManagerAddGuiNextFrame(&mainMenuGui);
}

void preUpdate(void) {
    if (input.pressed == KEY_ESCAPE) {
        settingsOpen(0);
    }
}

void removed(void) {
    guiManagerRemoveGuiNextFrame(&mainMenuGui);
    if (music) {
        soundDestroy(music);
        music = NULL;
    }
}

// Runs on a worker thread: only the slow part (pak read + decode).  The
// Sound travels to the main thread through the future-task queue.
static void loadMusic(void* _) {
    struct Sound* s = soundLoad("sound/Dark Descent (Extended Cut).ogg");
    futureTaskAdd(0, adoptMusic, s);
}

// Runs on the main thread: adopt the freshly loaded music for the current
// menu session, or discard it if the menu was already left.
static void adoptMusic(void* pSound) {
    struct Sound* s  = static_cast<struct Sound*>(pSound);
    if (gameStateCurrent() != STATE_MAIN_MENU) {
        soundDestroy(s);
        return;
    }
    // A stale load from a previous session may have been adopted first;
    // replace it so at most one menu music is ever alive.
    if (music) {
        soundDestroy(music);
        music = NULL;
    }
    music   = s;
    soundPlay(music, settingsGetDouble("music") / 100., 1);
}
