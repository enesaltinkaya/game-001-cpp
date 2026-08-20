#pragma once

#include "shared/InputEventShared.h"

typedef struct Window {
    struct GLFWwindow* glfwWindowHandle;
    struct SDL_Window* sdlWindowHandle;

    int width;
    int height;
    float ratio;
    float xscale;
    float yscale;
    char wayland;

    /* Render resolution (width/height * renderScale). Updated by rendererUpdateRenderDimensions(). */
    int renderWidth;
    int renderHeight;
} Window;

typedef struct Input {
    // keyboard
    i32 key;
    i32 scancode;
    i32 action;
    i32 mods;
    i32 pressed, released;
    u32 character;
    char repeating[KEY_COUNT];
    char ctrl, shift, alt;

    // mouse
    float lastX;
    float lastY;
    float deltaX;
    float deltaY;
    float xpos;
    float ypos;
    i32 mouseButton;
    i32 mouseAction;
    i32 mouseMods;
    float scrollY;
    float scrollX;

    // gamepad
    u32 gamepadButtonPressed[64];
    u32 gamepadButtonReleased[64];

    char focused;
    char skip;

    InputEvent* events;  // stb_array

} Input;

extern struct Window window;
extern struct Input input;

extern struct System windowSystem;

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
