#include "ecs/system/System.h"
#include "ecs/system/lua/LuaSystem.h"
#include "futuretask/FutureTask.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "rmlui/wrapper/src/crmlui.h"
#include "video/SettingsVideoGui.h"
#include "audio/SettingsAudioGui.h"
#include "graphics/SettingsGraphicsGui.h"

static void added(void);
static void removed(void);

System settingsGui = {
    .name                = "settingsGui",
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

static int settingsClose(void* _);
static int showAudioSettings(void* _);
static int showVideoSettings(void* _);
static int showGraphicsSettings(void* _);

void added(void) {
    document = rmlNewDocument("gui/settings/settings.html");
    rmlLoadDocument(document);
    rmlShowDocument(document);

    luaRegisterFunction("settingsClose", settingsClose);
    luaRegisterFunction("showAudioSettings", showAudioSettings);
    luaRegisterFunction("showVideoSettings", showVideoSettings);
    luaRegisterFunction("showGraphicsSettings", showGraphicsSettings);
}

void removed(void) {
    rmlUnloadDocument(document);
    document = nullptr;
}

void settingsGuiHide(void) {
    rmlHideDocument(document);
}

void settingsGuiShow(void) {
    rmlShowDocument(document);
}

int settingsClose(void* _) {
    guiManagerRemoveGuiNextFrame(&settingsGui);
    return 0;
}

int showAudioSettings(void* _) {
    futureTaskAddNoParam(0, settingsGuiHide);
    guiManagerAddGuiNextFrame(&settingsAudioGui);
    return 0;
}

int showVideoSettings(void* _) {
    futureTaskAddNoParam(0, settingsGuiHide);
    guiManagerAddGuiNextFrame(&settingsVideoGui);
    return 0;
}

int showGraphicsSettings(void* _) {
    futureTaskAddNoParam(0, settingsGuiHide);
    guiManagerAddGuiNextFrame(&settingsGraphicsGui);
    return 0;
}

char settingsGuiIsShowing(void) {
    return document != nullptr;
}
