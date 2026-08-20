#include "ecs/system/window/WindowSystem.h"
#include "ecs/system/System.h"

Window window;
struct Input input = {.focused = 1};

struct WindowBackendApi {
    const char* name;
    void (*added)(void);
    void (*removed)(void);
    void (*preUpdate)(void);
    void (*postUpdate)(void);
    void (*hide)(void);
    void (*show)(void);
    void (*toggleFullscreen)(char fullScreen);
    void (*reloadCursors)(void);
    void* (*getPointerCursor)(void);
    void* (*getArrowCursor)(void);
    void* (*getTextCursor)(void);
    void (*updateDimensions)(void);
    void (*warpCenter)(void);
    void (*warp)(float x, float y);
    void (*hideCursor)(void);
    void (*showCursor)(void);
    bool (*isCursorVisible)(void);
    void (*getRelativeMouseDelta)(float* dx, float* dy);
    bool (*isLeftMouseDown)(void);
    bool (*isRightMouseDown)(void);
    bool (*isMiddleMouseDown)(void);
    char const* const* (*getRequiredVulkanExtensions)(u32* extensionCount);
    bool (*createVulkanSurface)(VkInstance instance, VkSurfaceKHR* surface);
    SetCursorFn (*getSetCursorFn)(void);
};

extern WindowBackendApi glfwWindowBackendApi;
extern WindowBackendApi sdlWindowBackendApi;
#ifdef __linux__
extern WindowBackendApi x11WindowBackendApi;
#endif

static WindowBackendApi* activeBackend;

static WindowBackendApi* windowSystemSelectBackend(void) {
    const char* env = getenv("ENGINE_WINDOW_BACKEND");
    if (env && *env) {
        if (strequals(env, "sdl") || strequals(env, "SDL")) {
            return &sdlWindowBackendApi;
        }
        if (strequals(env, "glfw") || strequals(env, "GLFW")) {
            return &glfwWindowBackendApi;
        }
#ifdef __linux__
        if (strequals(env, "x11") || strequals(env, "X11")) {
            return &x11WindowBackendApi;
        }
#endif
        warn("windowSystem: unknown ENGINE_WINDOW_BACKEND='%s', falling back to sdl", env);
    }

    // SDL is the safest default on Linux/Wayland/XWayland: its relative mouse
    // mode hides the cursor for drag-look and restores the original cursor
    // position when relative mode is disabled. The X11 backend recenters the
    // pointer while hidden, which can leak through on some compositors.
    return &sdlWindowBackendApi;
}

static void added(void) {
    activeBackend = windowSystemSelectBackend();
    info("windowSystem: selected backend %s", activeBackend->name);
    activeBackend->added();
}

static void removed(void) {
    activeBackend->removed();
}

static void preUpdate(void) {
    activeBackend->preUpdate();
}

static void postUpdate(void) {
    activeBackend->postUpdate();
}

struct System windowSystem = {
    .name       = "window",
    .added      = added,
    .removed    = removed,
    .preUpdate  = preUpdate,
    .postUpdate = postUpdate,
};

void windowSystemHide(void) {
    activeBackend->hide();
}

void windowSystemShow(void) {
    activeBackend->show();
}

void windowSystemToggleFullscreen(char fullScreen) {
    activeBackend->toggleFullscreen(fullScreen);
}

void windowSystemReloadCursors(void) {
    activeBackend->reloadCursors();
}

void* windowSystemGetPointerCursor(void) {
    return activeBackend->getPointerCursor();
}

void* windowSystemGetArrowCursor(void) {
    return activeBackend->getArrowCursor();
}

void* windowSystemGetTextCursor(void) {
    return activeBackend->getTextCursor();
}

void windowSystemUpdateDimensions(void) {
    activeBackend->updateDimensions();
}

void windowSystemWarpCenter(void) {
    activeBackend->warpCenter();
}

void windowSystemWarp(float x, float y) {
    activeBackend->warp(x, y);
}

void windowSystemHideCursor(void) {
    activeBackend->hideCursor();
}

void windowSystemShowCursor(void) {
    activeBackend->showCursor();
}

bool windowSystemIsCursorVisible(void) {
    return activeBackend->isCursorVisible();
}

void windowSystemGetRelativeMouseDelta(float* dx, float* dy) {
    activeBackend->getRelativeMouseDelta(dx, dy);
}

bool windowSystemIsLeftMouseDown(void) {
    return activeBackend->isLeftMouseDown();
}

bool windowSystemIsRightMouseDown(void) {
    return activeBackend->isRightMouseDown();
}

bool windowSystemIsMiddleMouseDown(void) {
    return activeBackend->isMiddleMouseDown();
}

char const* const* windowSystemGetRequiredVulkanExtensions(u32* extensionCount) {
    return activeBackend->getRequiredVulkanExtensions(extensionCount);
}

bool windowSystemCreateVulkanSurface(VkInstance instance, VkSurfaceKHR* surface) {
    return activeBackend->createVulkanSurface(instance, surface);
}

SetCursorFn windowSystemGetSetCursorFn(void) {
    if (activeBackend->getSetCursorFn) return activeBackend->getSetCursorFn();
    return nullptr;
}

void inputReset(void) {
    input.key         = -1;
    input.scancode    = -1;
    input.action      = -1;
    input.mods        = -1;
    input.pressed     = -1;
    input.released    = -1;
    input.character   = -1;
    input.mouseButton = -1;
    input.mouseAction = -1;
    input.mouseMods   = -1;
    input.scrollY     = 0;
    input.scrollX     = 0;
    input.lastX       = input.xpos;
    input.lastY       = input.ypos;
    input.deltaX      = 0;
    input.deltaY      = 0;
    input.skip        = 0;
}

char inputShouldProcess(void) {
    return input.focused && !input.skip;
}
