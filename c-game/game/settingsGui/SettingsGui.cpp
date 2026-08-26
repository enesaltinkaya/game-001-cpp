#include "SettingsGui.h"
#include "ecs/system/System.h"
#include "ecs/system/lua/LuaSystem.h"
#include "futuretask/FutureTask.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "rmlui/wrapper/src/crmlui.h"
#include "video/SettingsVideoGui.h"
#include "audio/SettingsAudioGui.h"
#include "graphics/SettingsGraphicsGui.h"

namespace game {

SettingsGui settingsGui;

SettingsGui::SettingsGui() : engine::System("settingsGui") {
    menuGui = 1;
}

static void* document;

static int settingsClose(void* _);
static int showAudioSettings(void* _);
static int showVideoSettings(void* _);
static int showGraphicsSettings(void* _);

void SettingsGui::added() {
    document = rmlNewDocument("gui/settings/settings.html");
    rmlDocument = document;
    rmlLoadDocument(document);
    rmlShowDocument(document);

    engine::luaRegisterFunction("settingsClose", settingsClose);
    engine::luaRegisterFunction("showAudioSettings", showAudioSettings);
    engine::luaRegisterFunction("showVideoSettings", showVideoSettings);
    engine::luaRegisterFunction("showGraphicsSettings", showGraphicsSettings);
}

void SettingsGui::removed() {
    rmlUnloadDocument(document);
    document = nullptr;
    rmlDocument = nullptr;
}

void settingsGuiHide(void) {
    rmlHideDocument(document);
}

void settingsGuiShow(void) {
    rmlShowDocument(document);
}

int settingsClose(void* _) {
    engine::guiManagerRemoveGuiNextFrame(&settingsGui);
    return 0;
}

int showAudioSettings(void* _) {
    utils::futureTaskAddNoParam(0, settingsGuiHide);
    engine::guiManagerAddGuiNextFrame(&settingsAudioGui);
    return 0;
}

int showVideoSettings(void* _) {
    utils::futureTaskAddNoParam(0, settingsGuiHide);
    engine::guiManagerAddGuiNextFrame(&settingsVideoGui);
    return 0;
}

int showGraphicsSettings(void* _) {
    utils::futureTaskAddNoParam(0, settingsGuiHide);
    engine::guiManagerAddGuiNextFrame(&settingsGraphicsGui);
    return 0;
}

char settingsGuiIsShowing(void) {
    return document != nullptr;
}
}  // namespace game
