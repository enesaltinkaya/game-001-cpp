#include "SettingsVideoGui.h"
#include "ecs/system/System.h"
#include "ecs/system/lua/LuaSystem.h"
#include "ecs/system/window/WindowSystem.h"
#include "events/Events.h"
#include "futuretask/FutureTask.h"
#include "renderer/Renderer.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "rmlui/wrapper/src/crmlui.h"
#include "settings/Settings.h"
#include "timer/Timer.h"
#include "../SettingsGui.h"

namespace game {

SettingsVideoGui settingsVideoGui;

SettingsVideoGui::SettingsVideoGui() : engine::System("settingsVideoGui") {
    menuGui = 1;
}

static void* document;
static void* model;

static char fullScreen;
static char vsync;
static char showFps;
static char fpsLimitChecked;
static float fpsLimit;
static float uiScale;
static float cursorScale;


static void windowResized(void* _);
static int toggleFullScreen(void* _);
static int toggleVsync(void* _);
static int uiScaleChange(void* _);
static int cursorScaleChange(void* _);
static int fpsLimitChange(void* _);
static int fpsLimitCheckedChange(void* _);
static int showFpsChange(void* _);
static int videoClose(void* _);

void SettingsVideoGui::added() {
    utils::signalSubscribe("windowResized", windowResized);

    engine::luaRegisterFunction("toggleFullScreen", toggleFullScreen);
    engine::luaRegisterFunction("toggleVsync", toggleVsync);
    engine::luaRegisterFunction("uiScaleChange", uiScaleChange);
    engine::luaRegisterFunction("cursorScaleChange", cursorScaleChange);
    engine::luaRegisterFunction("fpsLimitChange", fpsLimitChange);
    engine::luaRegisterFunction("fpsLimitCheckedChange", fpsLimitCheckedChange);
    engine::luaRegisterFunction("showFpsChange", showFpsChange);
    engine::luaRegisterFunction("videoClose", videoClose);

    fullScreen      = utils::settingsGetBool("fullScreen");
    vsync           = utils::settingsGetBool("vsync");
    showFps         = utils::settingsGetBool("showFps");
    fpsLimitChecked = utils::settingsGetBool("fpsLimitChecked");
    fpsLimit        = utils::settingsGetDouble("fpsLimit");
    uiScale         = utils::settingsGetDouble("uiScale");
    cursorScale     = utils::settingsGetDouble("cursorScale");

    document = rmlNewDocument("gui/settings/video/video.html");
    rmlDocument = document;
    model    = rmlCreateModel("video");
    rmlBindBool(model, "fullScreen", &fullScreen);
    rmlBindBool(model, "vsync", &vsync);
    rmlBindBool(model, "showFps", &showFps);
    rmlBindBool(model, "fpsLimitChecked", &fpsLimitChecked);
    rmlBindFloat(model, "fpsLimit", &fpsLimit);
    rmlBindFloat(model, "uiScale", &uiScale);
    rmlBindFloat(model, "cursorScale", &cursorScale);
    rmlLoadDocument(document);
    rmlShowDocument(document);
}

void SettingsVideoGui::removed() {
    rmlUnloadDocument(document);
    rmlUnloadModel(model);
    document = nullptr;
    model    = nullptr;
    rmlDocument = nullptr;
    utils::signalRemoveSubscription("windowResized", windowResized);
}

static void toggleFullScreenLater(void* _) {
    if (utils::settingsGetBool("fullScreen") == fullScreen) {
        return;
    }

    utils::settingsSetBool("fullScreen", fullScreen);
    engine::windowSystemToggleFullscreen(fullScreen);
    utils::settingsWrite();
}

int toggleFullScreen(void* _) {
    utils::futureTaskAdd(0, toggleFullScreenLater, nullptr);
    return 0;
}

static void toggleVsyncLater(void* _) {
    if (utils::settingsGetBool("vsync") == vsync) {
        return;
    }

    utils::settingsSetBool("vsync", vsync);
    engine::rendererSetVsync(vsync);
    utils::settingsWrite();
}

int toggleVsync(void* _) {
    utils::futureTaskAdd(0, toggleVsyncLater, nullptr);
    return 0;
}

static void uiScaleChangeLater(void* _) {
    if (utils::settingsGetDouble("uiScale") == uiScale) {
        return;
    }

    utils::settingsSetDouble("uiScale", uiScale);
    utils::signalEmit("uiScaleChanged", nullptr);
    utils::settingsWrite();
}

int uiScaleChange(void* _) {
    utils::futureTaskAdd(0, uiScaleChangeLater, nullptr);
    return 0;
}

static void cursorScaleChangeLater(void* _) {
    if (utils::settingsGetDouble("cursorScale") == cursorScale) {
        return;
    }

    utils::settingsSetDouble("cursorScale", cursorScale);
    engine::windowSystemReloadCursors();
    engine::guiManagerUpdateCursors();
    utils::settingsWrite();
}

int cursorScaleChange(void* _) {
    utils::futureTaskAdd(0, cursorScaleChangeLater, nullptr);
    return 0;
}

static void fpsLimitCheckedChangeLater(void* _) {
    if (utils::settingsGetBool("fpsLimitChecked") == fpsLimitChecked) {
        return;
    }

    utils::settingsSetBool("fpsLimitChecked", fpsLimitChecked);
    utils::timerInit(utils::settingsGetDouble("fpsLimit"), utils::settingsGetBool("fpsLimitChecked"), 0);

    // settingsSetBool("vsync", vsync);
    // rendererSetVsync(vsync);

    utils::settingsWrite();
}

int fpsLimitCheckedChange(void* _) {
    utils::futureTaskAdd(0, fpsLimitCheckedChangeLater, nullptr);
    return 0;
}

static void fpsLimitChangeLater(void* _) {
    if (utils::settingsGetDouble("fpsLimit") == fpsLimit) {
        return;
    }

    utils::settingsSetDouble("fpsLimit", fpsLimit);
    utils::timerInit(utils::settingsGetDouble("fpsLimit"), utils::settingsGetBool("fpsLimitChecked"), 0);
    utils::settingsWrite();
}

int fpsLimitChange(void* _) {
    utils::futureTaskAdd(0, fpsLimitChangeLater, nullptr);
    return 0;
}

static void showFpsChangeLater(void* _) {
    if (utils::settingsGetBool("showFps") == showFps) {
        return;
    }

    utils::settingsSetBool("showFps", showFps);
    engine::guiManagerToggleShowFps();
    utils::settingsWrite();
}

int showFpsChange(void* _) {
    utils::futureTaskAdd(0, showFpsChangeLater, nullptr);
    return 0;
}

void windowResized(void* _) {
    fullScreen = utils::settingsGetBool("fullScreen");
    rmlUpdateDirtyAll(model);
}

int videoClose(void* _) {
    utils::futureTaskAddNoParam(0, settingsGuiShow);
    engine::guiManagerRemoveGuiNextFrame(&settingsVideoGui);
    return 0;
}
}  // namespace game
