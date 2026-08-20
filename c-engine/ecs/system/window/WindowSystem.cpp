#include "ecs/system/window/WindowSystem.h"
#include "ecs/system/System.h"

namespace engine {
Window window;
struct Input input = {.focused = 1};

struct WindowBackendApi {
    const char* name = nullptr;
    void (*added)(void) = nullptr;
    void (*removed)(void) = nullptr;
    void (*preUpdate)(void) = nullptr;
    void (*postUpdate)(void) = nullptr;
    void (*hide)(void) = nullptr;
    void (*show)(void) = nullptr;
    void (*toggleFullscreen)(char fullScreen) = nullptr;
    void (*reloadCursors)(void) = nullptr;
    void* (*getPointerCursor)(void) = nullptr;
    void* (*getArrowCursor)(void) = nullptr;
    void* (*getTextCursor)(void) = nullptr;
    void (*updateDimensions)(void) = nullptr;
    void (*warpCenter)(void) = nullptr;
    void (*warp)(float x, float y) = nullptr;
    void (*hideCursor)(void) = nullptr;
    void (*showCursor)(void) = nullptr;
    bool (*isCursorVisible)(void) = nullptr;
    void (*getRelativeMouseDelta)(float* dx, float* dy) = nullptr;
    bool (*isLeftMouseDown)(void) = nullptr;
    bool (*isRightMouseDown)(void) = nullptr;
    bool (*isMiddleMouseDown)(void) = nullptr;
    char const* const* (*getRequiredVulkanExtensions)(u32* extensionCount) = nullptr;
    bool (*createVulkanSurface)(VkInstance instance, VkSurfaceKHR* surface) = nullptr;
    SetCursorFn (*getSetCursorFn)(void) = nullptr;
};

extern WindowBackendApi sdlWindowBackendApi;

static WindowBackendApi* activeBackend;

static WindowBackendApi* windowSystemSelectBackend(void) {
    const char* env = getenv("ENGINE_WINDOW_BACKEND");
    if (env && *env && !utils::strequals(env, "sdl") && !utils::strequals(env, "SDL")) {
        utils::warn("windowSystem: unknown ENGINE_WINDOW_BACKEND='%s' (only 'sdl' is available), using sdl", env);
    }
    return &sdlWindowBackendApi;
}

void WindowSystem::added() {
    activeBackend = windowSystemSelectBackend();
    utils::info("windowSystem: selected backend %s", activeBackend->name);
    activeBackend->added();
}

void WindowSystem::removed() {
    activeBackend->removed();
}

void WindowSystem::preUpdate() {
    activeBackend->preUpdate();
}

void WindowSystem::postUpdate() {
    activeBackend->postUpdate();
}

WindowSystem windowSystem;

WindowSystem::WindowSystem() : System("window") {}

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
}  // namespace engine
