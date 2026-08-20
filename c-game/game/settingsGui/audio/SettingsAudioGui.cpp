#include "ecs/system/System.h"
#include "ecs/system/lua/LuaSystem.h"
#include "ecs/system/sound/SoundSystem.h"
#include "futuretask/FutureTask.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "rmlui/wrapper/src/crmlui.h"
#include "settings/Settings.h"
#include "../SettingsGui.h"

static void added(void);
static void removed(void);

System settingsAudioGui = {
    .name                = "settingsAudioGui",
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
static float effects;
static float music;
static int effectsChange(void* _);
static int musicChange(void* _);
static int audioSettingsClose(void* _);

void added(void) {
    luaRegisterFunction("effectsChange", effectsChange);
    luaRegisterFunction("musicChange", musicChange);
    luaRegisterFunction("audioSettingsClose", audioSettingsClose);
    effects = settingsGetDouble("effects");
    music   = settingsGetDouble("music");

    document = rmlNewDocument("gui/settings/audio/audio.html");
    model    = rmlCreateModel("audio");
    rmlBindFloat(model, "effects", &effects);
    rmlBindFloat(model, "music", &music);
    rmlLoadDocument(document);
    rmlShowDocument(document);
}

void removed(void) {
    rmlUnloadDocument(document);
    rmlUnloadModel(model);
}

int audioSettingsClose(void* _) {
    futureTaskAddNoParam(0, settingsGuiShow);
    guiManagerRemoveGuiNextFrame(&settingsAudioGui);
    return 0;
}

static void effectsChangeLater(void* _) {
    settingsSetDouble("effects", effects);
    soundPlayClick();
    settingsWrite();
}

static void musicChangeLater(void* _) {
    settingsSetDouble("music", music);
    soundPlayClickOnMusicLevel();
    settingsWrite();
}

int effectsChange(void* _) {
    // new value of bound variable is updated next frame (rmlBindFloat(model, ...))
    // so update settings next frame
    static int key;
    if (key) {
        futureTaskRemove(key);
    }
    key = futureTaskAdd(50, effectsChangeLater, nullptr);
    return 0;
}

int musicChange(void* _) {
    static int key;
    if (key) {
        futureTaskRemove(key);
    }
    key = futureTaskAdd(50, musicChangeLater, nullptr);
    return 0;
}
