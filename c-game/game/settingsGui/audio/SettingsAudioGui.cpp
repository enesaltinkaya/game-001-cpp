#include "SettingsAudioGui.h"
#include "ecs/system/System.h"
#include "ecs/system/lua/LuaSystem.h"
#include "ecs/system/sound/SoundSystem.h"
#include "futuretask/FutureTask.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "rmlui/wrapper/src/crmlui.h"
#include "settings/Settings.h"
#include "../SettingsGui.h"

namespace game {

SettingsAudioGui settingsAudioGui;

SettingsAudioGui::SettingsAudioGui() : engine::System("settingsAudioGui") {
    menuGui = 1;
}

static void* document;
static void* model;
static float effects;
static float music;
static int effectsChange(void* _);
static int musicChange(void* _);
static int audioSettingsClose(void* _);

void SettingsAudioGui::added() {
    engine::luaRegisterFunction("effectsChange", effectsChange);
    engine::luaRegisterFunction("musicChange", musicChange);
    engine::luaRegisterFunction("audioSettingsClose", audioSettingsClose);
    effects = utils::settingsGetDouble("effects");
    music   = utils::settingsGetDouble("music");

    document = rmlNewDocument("gui/settings/audio/audio.html");
    rmlDocument = document;
    model    = rmlCreateModel("audio");
    rmlBindFloat(model, "effects", &effects);
    rmlBindFloat(model, "music", &music);
    rmlLoadDocument(document);
    rmlShowDocument(document);
}

void SettingsAudioGui::removed() {
    rmlUnloadDocument(document);
    rmlUnloadModel(model);
    rmlDocument = nullptr;
}

int audioSettingsClose(void* _) {
    utils::futureTaskAddNoParam(0, settingsGuiShow);
    engine::guiManagerRemoveGuiNextFrame(&settingsAudioGui);
    return 0;
}

static void effectsChangeLater(void* _) {
    utils::settingsSetDouble("effects", effects);
    engine::soundPlayClick();
    utils::settingsWrite();
}

static void musicChangeLater(void* _) {
    utils::settingsSetDouble("music", music);
    engine::soundPlayClickOnMusicLevel();
    utils::settingsWrite();
}

int effectsChange(void* _) {
    // new value of bound variable is updated next frame (rmlBindFloat(model, ...))
    // so update settings next frame
    static int key;
    if (key) {
        utils::futureTaskRemove(key);
    }
    key = utils::futureTaskAdd(50, effectsChangeLater, nullptr);
    return 0;
}

int musicChange(void* _) {
    static int key;
    if (key) {
        utils::futureTaskRemove(key);
    }
    key = utils::futureTaskAdd(50, musicChangeLater, nullptr);
    return 0;
}
}  // namespace game
