#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "datamanager/DataManager.h"
#include "ecs/system/System.h"
#include "ecs/system/window/WindowSystem.h"
#include "ecs/system/lua/LuaSystem.h"
#include "events/Events.h"
#include "futuretask/FutureTask.h"
#include "rmlui/wrapper/src/crmlui.h"
#include "settings/Settings.h"
#include "renderer/vulkan/pass/rmlui/VulkanRmluiPass.h"

static void added(void);
static void removed(void);
static void postUpdate(void);

System guiManagerRmlUi = {
    .name                = "gui",
    .added               = added,
    .removed             = removed,
    .preUpdate           = nullptr,
    .update              = nullptr,
    .postUpdate          = postUpdate,
    .cpuElapsedLastFrame = 0.0,
    .cpuElapsed          = 0.0,
    .gpuElapsed          = 0.0,
    .priority            = 0,
};

static RmlParams rmlParams;
static void gmWindowResized(void* _);

struct System** rmluiGuis;  // stb array

void added(void) {
    char version[20] = {};
    rmlGetVersion(version);
    debug("rmlui: RmlUi %s", version);

    rmlParams = RmlParams{
        .window =
            {
                .sdlWindowHandle  = window.sdlWindowHandle,
                .glfwWindowHandle = window.glfwWindowHandle,
                .windowWidth      = window.width,
                .windowHeight     = window.height,
                .arrowCursor      = windowSystemGetArrowCursor(),
                .handCursor       = windowSystemGetPointerCursor(),
                .textCursor       = windowSystemGetTextCursor(),
                .setCursorFn      = windowSystemGetSetCursorFn(),
            },
        .luaState = luaGetState(),
        .log =
            {
                .debugFn = debugRml,
                .errorFn = errorRml,
                .infoFn  = infoRml,
                .warnFn  = warnRml,
            },
        .file =
            {
                .fileOpenFn  = dmRmlopen,
                .fileCloseFn = dmRmlclose,
                .fileReadFn  = dmRmlread,
                .fileSeekFn  = dmRmlseek,
                .fileTellFn  = dmRmltell,
            },
        .vulkan =
            {
                .beginFrame          = rmlBeginFrame,
                .endFrame            = rmlEndFrame,
                .renderGeometry      = rmlRenderGeometry,
                .compileGeometry     = rmlCompileGeometry,
                .releaseGeometry     = rmlReleaseGeometry,
                .loadTexture         = rmlLoadTexture,
                .generateTexture     = rmlGenerateTexture,
                .releaseTexture      = rmlReleaseTexture,
                .enableScissorRegion = rmlEnableScissorRegion,
                .setScissorRegion    = rmlSetScissorRegion,
                .setTransform        = rmlSetTransform,
                .setViewport         = rmlSetViewport,
            },

    };

    if (isDebug()) {
        rmlParams.enableDebugger = 1;
    }

    rmlInitVulkan(&rmlParams);

    float scale = settingsGetDouble("uiScale");
    rmlSetDimensions(window.width, window.height, scale);

    signalSubscribe("swapchainCreated", gmWindowResized);
    signalSubscribe("uiScaleChanged", gmWindowResized);

    if (settingsGetBool("showFps")) {
        guiManagerAddGuiNextFrame(&rmluiShowFpsGui);
    }

    // statsGuiToggle();
    // passStatsGuiToggle();
}

void postUpdate(void) {
    if (input.ctrl && input.pressed == KEY_D && windowSystemIsCursorVisible()) {
        statsGuiToggle();
    }

    if (input.ctrl && input.pressed == KEY_B && windowSystemIsCursorVisible()) {
        debugGuiToggle();
    }

    if (input.ctrl && input.pressed == KEY_P && windowSystemIsCursorVisible()) {
        passStatsGuiToggle();
    }

    if (input.ctrl && input.pressed == KEY_F8 && windowSystemIsCursorVisible()) {
        static char show;
        show = !show;
        rmlToggleDebugger(show);
    }

    if (!input.skip) {
        bool cursorWasVisible = windowSystemIsCursorVisible();
        for (i32 i = 0, si = arraySize(input.events); i < si; i++) {
            InputEvent* ev = &input.events[i];

            // Ignore the first few motion events until pointer state stabilizes.
            static char skipMotionEvent = 10;
            if (skipMotionEvent && ev->type == INPUT_EVENT_MOUSE_MOVE) {
                skipMotionEvent--;
                continue;
            }

            // Always forward mouse button events so RmlUi sees the full
            // press/release cycle even when the cursor was hidden mid-frame.
            bool isMouseButton = (ev->type == INPUT_EVENT_MOUSE_BUTTON_DOWN ||
                                  ev->type == INPUT_EVENT_MOUSE_BUTTON_UP);
            if (!cursorWasVisible && !isMouseButton) {
                if (ev->type == INPUT_EVENT_MOUSE_MOVE || ev->type == INPUT_EVENT_MOUSE_WHEEL) {
                    continue;
                }
            }

            rmlSendInputEvent(ev);
        }
    }
    // Update GUI data models first (dirty variables), then process
    // layout so Rml::Context::Update() sees the current frame's values.
    // Previously rmlUpdate() ran before GUI updates, causing a one-frame
    // lag in element positions (e.g. enemy HP bars lagging behind rotation).
    foreach (struct System* gui, rmluiGuis) {
        if (gui->update) {
            gui->update();
        }
    }

    rmlUpdate();
    rmlRenderVulkan();
}

void removed(void) {
    foreach (struct System* gui, rmluiGuis) {
        warn("remove gui (guimanager removed): %s", gui->name);
        if (gui->removed) {
            gui->removed();
        }
    }
    for (i32 i = 0, si = arraySize(input.events); i < si; i++) {
        rmlSendInputEvent(&input.events[i]);
    }
    rmlUpdate();

    warn("RML SHUTDOWN");
    rmlDestroy();
    arrayFree(rmluiGuis);
}

void gmWindowResized(void* _) {
    rmlSetDimensions(window.width, window.height, settingsGetDouble("uiScale"));
}

static void addGui(void* pGui) {
    struct System* gui  = static_cast<struct System*>(pGui);
    debug("rmlui: showing %s", gui->name);
    arrayPut(rmluiGuis, gui);
    gui->added();
}

static void removeGui(void* pGui) {
    struct System* gui  = static_cast<struct System*>(pGui);
    debug("rmlui: removing gui %s", gui->name);

    for (int i = 0, si = arraySize(rmluiGuis); i < si; i++) {
        if (gui == rmluiGuis[i]) {
            arrayDeleteSlow(rmluiGuis, i);
            if (gui->removed) {
                gui->removed();
            }
            break;
        }
    }
}

void guiManagerAddGuiNextFrame(struct System* gui) {
    futureTaskAdd(0, addGui, gui);
}

void guiManagerRemoveGuiNextFrame(struct System* gui) {
    futureTaskAdd(0, removeGui, gui);
}

void guiManagerUpdateScale(void) {
    rmlSetDimensions(window.width, window.height, settingsGetDouble("uiScale"));
}

void guiManagerUpdateCursors(void) {
    rmlParams.window.arrowCursor = windowSystemGetArrowCursor();
    rmlParams.window.handCursor  = windowSystemGetPointerCursor();
    rmlParams.window.textCursor  = windowSystemGetTextCursor();
    rmlUpdateCursors(&rmlParams);
}

void guiManagerToggleShowFps(void) {
    if (settingsGetBool("showFps")) {
        guiManagerAddGuiNextFrame(&rmluiShowFpsGui);
    } else {
        guiManagerRemoveGuiNextFrame(&rmluiShowFpsGui);
    }
}

void guiManagerReleaseTexture(const char* name) {
    rmlReleaseTextureByName(name);
}

void guiManagerReleaseAllTextures(void) {
    rmlReleaseAllTextures();
}
