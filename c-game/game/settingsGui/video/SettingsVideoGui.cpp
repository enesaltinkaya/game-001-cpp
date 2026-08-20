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

static void added(void);
static void removed(void);

System settingsVideoGui = {
    .name                = "settingsVideoGui",
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

void added(void) {
    signalSubscribe("windowResized", windowResized);

    luaRegisterFunction("toggleFullScreen", toggleFullScreen);
    luaRegisterFunction("toggleVsync", toggleVsync);
    luaRegisterFunction("uiScaleChange", uiScaleChange);
    luaRegisterFunction("cursorScaleChange", cursorScaleChange);
    luaRegisterFunction("fpsLimitChange", fpsLimitChange);
    luaRegisterFunction("fpsLimitCheckedChange", fpsLimitCheckedChange);
    luaRegisterFunction("showFpsChange", showFpsChange);
    luaRegisterFunction("videoClose", videoClose);

    fullScreen      = settingsGetBool("fullScreen");
    vsync           = settingsGetBool("vsync");
    showFps         = settingsGetBool("showFps");
    fpsLimitChecked = settingsGetBool("fpsLimitChecked");
    fpsLimit        = settingsGetDouble("fpsLimit");
    uiScale         = settingsGetDouble("uiScale");
    cursorScale     = settingsGetDouble("cursorScale");

    document = rmlNewDocument("gui/settings/video/video.html");
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

void removed(void) {
    rmlUnloadDocument(document);
    rmlUnloadModel(model);
    document = nullptr;
    model    = nullptr;
    signalRemoveSubscription("windowResized", windowResized);
}

static void toggleFullScreenLater(void* _) {
    if (settingsGetBool("fullScreen") == fullScreen) {
        return;
    }

    settingsSetBool("fullScreen", fullScreen);
    windowSystemToggleFullscreen(fullScreen);
    settingsWrite();
}

int toggleFullScreen(void* _) {
    futureTaskAdd(0, toggleFullScreenLater, nullptr);
    return 0;
}

static void toggleVsyncLater(void* _) {
    if (settingsGetBool("vsync") == vsync) {
        return;
    }

    settingsSetBool("vsync", vsync);
    rendererSetVsync(vsync);
    settingsWrite();
}

int toggleVsync(void* _) {
    futureTaskAdd(0, toggleVsyncLater, nullptr);
    return 0;
}

static void uiScaleChangeLater(void* _) {
    if (settingsGetDouble("uiScale") == uiScale) {
        return;
    }

    settingsSetDouble("uiScale", uiScale);
    signalEmit("uiScaleChanged", nullptr);
    settingsWrite();
}

int uiScaleChange(void* _) {
    futureTaskAdd(0, uiScaleChangeLater, nullptr);
    return 0;
}

static void cursorScaleChangeLater(void* _) {
    if (settingsGetDouble("cursorScale") == cursorScale) {
        return;
    }

    settingsSetDouble("cursorScale", cursorScale);
    windowSystemReloadCursors();
    guiManagerUpdateCursors();
    settingsWrite();
}

int cursorScaleChange(void* _) {
    futureTaskAdd(0, cursorScaleChangeLater, nullptr);
    return 0;
}

static void fpsLimitCheckedChangeLater(void* _) {
    if (settingsGetBool("fpsLimitChecked") == fpsLimitChecked) {
        return;
    }

    settingsSetBool("fpsLimitChecked", fpsLimitChecked);
    timerInit(settingsGetDouble("fpsLimit"), settingsGetBool("fpsLimitChecked"), 0);

    // settingsSetBool("vsync", vsync);
    // rendererSetVsync(vsync);

    settingsWrite();
}

int fpsLimitCheckedChange(void* _) {
    futureTaskAdd(0, fpsLimitCheckedChangeLater, nullptr);
    return 0;
}

static void fpsLimitChangeLater(void* _) {
    if (settingsGetDouble("fpsLimit") == fpsLimit) {
        return;
    }

    settingsSetDouble("fpsLimit", fpsLimit);
    timerInit(settingsGetDouble("fpsLimit"), settingsGetBool("fpsLimitChecked"), 0);
    settingsWrite();
}

int fpsLimitChange(void* _) {
    futureTaskAdd(0, fpsLimitChangeLater, nullptr);
    return 0;
}

static void showFpsChangeLater(void* _) {
    if (settingsGetBool("showFps") == showFps) {
        return;
    }

    settingsSetBool("showFps", showFps);
    guiManagerToggleShowFps();
    settingsWrite();
}

int showFpsChange(void* _) {
    futureTaskAdd(0, showFpsChangeLater, nullptr);
    return 0;
}

void windowResized(void* _) {
    fullScreen = settingsGetBool("fullScreen");
    rmlUpdateDirtyAll(model);
}

int videoClose(void* _) {
    futureTaskAddNoParam(0, settingsGuiShow);
    guiManagerRemoveGuiNextFrame(&settingsVideoGui);
    return 0;
}
