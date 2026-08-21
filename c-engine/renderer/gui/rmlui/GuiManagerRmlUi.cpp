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

namespace engine {

GuiManagerRmlUi guiManagerRmlUi;

GuiManagerRmlUi::GuiManagerRmlUi() : System("gui") {}

static RmlParams rmlParams;
static void gmWindowResized(void* _);

std::vector<System*> rmluiGuis;

void GuiManagerRmlUi::added() {
    char version[20] = {};
    rmlGetVersion(version);
    utils::debug("rmlui: RmlUi %s", version);

    rmlParams = RmlParams{
        .window =
            {
                .sdlWindowHandle = window.sdlWindowHandle,
                .windowWidth      = window.width,
                .windowHeight     = window.height,
                .arrowCursor      = windowSystemGetArrowCursor(),
                .handCursor       = windowSystemGetPointerCursor(),
                .textCursor       = windowSystemGetTextCursor(),
                .setCursorFn      = windowSystemGetSetCursorFn(),
            },
        .luaState = luaGetState(),
        .enableDebugger = 0,
        .log =
            {
                .debugFn = utils::debugRml,
                .errorFn = utils::errorRml,
                .infoFn  = utils::infoRml,
                .warnFn  = utils::warnRml,
            },
        .file =
            {
                .fileOpenFn  = utils::dmRmlopen,
                .fileCloseFn = utils::dmRmlclose,
                .fileReadFn  = utils::dmRmlread,
                .fileSeekFn  = utils::dmRmlseek,
                .fileTellFn  = utils::dmRmltell,
            },
        .vulkan =
            {
                .beginFrame          = rmlBeginFrame,
                .endFrame            = rmlEndFrame,
                .renderGeometry      = rmlRenderGeometry,
                .compileGeometry     = rmlCompileGeometry,
                .releaseGeometry     = rmlReleaseGeometry,
                .renderCompiledGeometry = nullptr,
                .loadTexture         = rmlLoadTexture,
                .generateTexture     = rmlGenerateTexture,
                .releaseTexture      = rmlReleaseTexture,
                .enableScissorRegion = rmlEnableScissorRegion,
                .setScissorRegion    = rmlSetScissorRegion,
                .setTransform        = rmlSetTransform,
                .setViewport         = rmlSetViewport,
            },

    };

    if (utils::isDebug()) {
        rmlParams.enableDebugger = 1;
    }

    rmlInitVulkan(&rmlParams);

    float scale = utils::settingsGetDouble("uiScale");
    rmlSetDimensions(window.width, window.height, scale);

    utils::signalSubscribe("swapchainCreated", gmWindowResized);
    utils::signalSubscribe("uiScaleChanged", gmWindowResized);

    if (utils::settingsGetBool("showFps")) {
        guiManagerAddGuiNextFrame(&rmluiShowFpsGui);
    }

    // statsGuiToggle();
    // passStatsGuiToggle();
}

void GuiManagerRmlUi::postUpdate() {
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
        for (i32 i = 0, si = static_cast<i32>(input.events.size()); i < si; i++) {
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
    for (System* gui : rmluiGuis) {
        gui->update();
    }

    rmlUpdate();
    rmlRenderVulkan();
}

void GuiManagerRmlUi::removed() {
    for (System* gui : rmluiGuis) {
        utils::warn("remove gui (guimanager removed): %s", gui->name);
        gui->removed();
    }
    // Drop the list so pending future-task removals (e.g.
    // guiManagerRemoveGuiNextFrame queued right before exit) can't
    // remove the same GUI a second time after RML is destroyed below.
    rmluiGuis.clear();
    for (i32 i = 0, si = static_cast<i32>(input.events.size()); i < si; i++) {
        rmlSendInputEvent(&input.events[i]);
    }
    rmlUpdate();

    utils::warn("RML SHUTDOWN");
    rmlDestroy();
}

void gmWindowResized(void* _) {
    rmlSetDimensions(window.width, window.height, utils::settingsGetDouble("uiScale"));
}

static void addGui(void* pGui) {
    System* gui  = static_cast<System*>(pGui);
    utils::debug("rmlui: showing %s", gui->name);
    rmluiGuis.push_back(gui);
    gui->added();
}

static void removeGui(void* pGui) {
    System* gui  = static_cast<System*>(pGui);
    utils::debug("rmlui: removing gui %s", gui->name);

    for (int i = 0, si = static_cast<i32>(rmluiGuis.size()); i < si; i++) {
        if (gui == rmluiGuis[i]) {
            rmluiGuis.erase(rmluiGuis.begin() + i);
            gui->removed();
            break;
        }
    }
}

void guiManagerAddGuiNextFrame(System* gui) {
    utils::futureTaskAdd(0, addGui, gui);
}

void guiManagerRemoveGuiNextFrame(System* gui) {
    utils::futureTaskAdd(0, removeGui, gui);
}

void guiManagerUpdateScale(void) {
    rmlSetDimensions(window.width, window.height, utils::settingsGetDouble("uiScale"));
}

void guiManagerUpdateCursors(void) {
    rmlParams.window.arrowCursor = windowSystemGetArrowCursor();
    rmlParams.window.handCursor  = windowSystemGetPointerCursor();
    rmlParams.window.textCursor  = windowSystemGetTextCursor();
    rmlUpdateCursors(&rmlParams);
}

void guiManagerToggleShowFps(void) {
    if (utils::settingsGetBool("showFps")) {
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
}  // namespace engine
