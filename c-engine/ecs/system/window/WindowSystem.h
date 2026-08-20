#pragma once
#include "ecs/system/System.h"

#include <vector>
#include "shared/InputEventShared.h"

struct SDL_Window;

namespace engine {
struct Window {
    ::SDL_Window* sdlWindowHandle;

    int width;
    int height;
    float ratio;
    float xscale;
    float yscale;
    char wayland;

    /* Render resolution (width/height * renderScale). Updated by rendererUpdateRenderDimensions(). */
    int renderWidth;
    int renderHeight;
};

struct Input {
    // keyboard
    i32 key = 0;
    i32 scancode = 0;
    i32 action = 0;
    i32 mods = 0;
    i32 pressed = 0, released = 0;
    u32 character = 0;
    char repeating[KEY_COUNT] = {};
    char ctrl = 0, shift = 0, alt = 0;

    // mouse
    float lastX = 0.0f;
    float lastY = 0.0f;
    float deltaX = 0.0f;
    float deltaY = 0.0f;
    float xpos = 0.0f;
    float ypos = 0.0f;
    i32 mouseButton = 0;
    i32 mouseAction = 0;
    i32 mouseMods = 0;
    float scrollY = 0.0f;
    float scrollX = 0.0f;

    // gamepad
    u32 gamepadButtonPressed[64] = {};
    u32 gamepadButtonReleased[64] = {};

    char focused = 0;
    char skip = 0;

    std::vector<InputEvent> events = {};

};

extern struct Window window;
extern struct Input input;

class WindowSystem : public System {
public:
    WindowSystem();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void postUpdate() override;
};

extern WindowSystem windowSystem;

void windowSystemHide(void);
void windowSystemShow(void);

void windowSystemToggleFullscreen(char fullScreen);
void windowSystemReloadCursors(void);

void* windowSystemGetPointerCursor(void);
void* windowSystemGetArrowCursor(void);
void* windowSystemGetTextCursor(void);

void windowSystemUpdateDimensions(void);
void windowSystemWarpCenter(void);
void windowSystemWarp(float x, float y);

void windowSystemHideCursor(void);
void windowSystemShowCursor(void);
bool windowSystemIsCursorVisible(void);

void windowSystemGetRelativeMouseDelta(float* dx, float* dy);
bool windowSystemIsLeftMouseDown(void);
bool windowSystemIsRightMouseDown(void);
bool windowSystemIsMiddleMouseDown(void);

char const* const* windowSystemGetRequiredVulkanExtensions(u32* extensionCount);
bool windowSystemCreateVulkanSurface(VkInstance instance, VkSurfaceKHR* surface);

typedef void (*SetCursorFn)(int cursorType);
SetCursorFn windowSystemGetSetCursorFn(void);

void inputReset(void);
char inputShouldProcess(void);
}  // namespace engine
