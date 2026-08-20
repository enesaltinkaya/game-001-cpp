#include "ecs/system/window/WindowSystem.h"
#include "Engine.h"
#include "SDL3/SDL_events.h"
#include "SDL3/SDL_keycode.h"
#include "SDL3/SDL_gamepad.h"
#include "SDL3/SDL_hints.h"
#include "SDL3/SDL_init.h"
#include "SDL3/SDL_version.h"
#include "SDL3/SDL_video.h"
#include "Utils.h"
#include "image/Image.h"
#include "stb/git/stb_image.h"          // IWYU pragma: keep
#include "stb/git/stb_image_resize2.h"  // IWYU pragma: keep

typedef struct WindowBackendApi {
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
} WindowBackendApi;

static void initSDL(void);
static void setDimensions(void);
static void createWindow(void);
static void centerWindow(void);
static void loadCursors(void);
static KeyCode windowSystemMapSDLKey(SDL_Keycode key);
static MouseButton windowSystemMapSDLMouseButton(u8 button);
static void windowSystemPushInputEvent(SDL_Event* event);

SDL_Event e;
static SDL_Cursor *cursorArrow, *cursorHand, *cursorText;
static const char* title = "Mini";
static Array(SDL_Gamepad*) gamepads;

static void sdlWindowSystemAdded(void);
static void sdlWindowSystemRemoved(void);
static void sdlWindowSystemPreUpdate(void);
static void sdlWindowSystemPostUpdate(void);
void sdlWindowSystemHide(void);
void sdlWindowSystemShow(void);
void sdlWindowSystemToggleFullscreen(char fullScreen);
void sdlWindowSystemReloadCursors(void);
void* sdlWindowSystemGetPointerCursor(void);
void* sdlWindowSystemGetArrowCursor(void);
void* sdlWindowSystemGetTextCursor(void);
void sdlWindowSystemUpdateDimensions(void);
void sdlWindowSystemWarpCenter(void);
void sdlWindowSystemWarp(float x, float y);
void sdlWindowSystemHideCursor(void);
void sdlWindowSystemShowCursor(void);
bool sdlWindowSystemIsCursorVisible(void);
void sdlWindowSystemGetRelativeMouseDelta(float* dx, float* dy);
bool sdlWindowSystemIsLeftMouseDown(void);
bool sdlWindowSystemIsRightMouseDown(void);
bool sdlWindowSystemIsMiddleMouseDown(void);
char const* const* sdlWindowSystemGetRequiredVulkanExtensions(u32* extensionCount);
bool sdlWindowSystemCreateVulkanSurface(VkInstance instance, VkSurfaceKHR* surface);
void sdlInputReset(void);
char sdlInputShouldProcess(void);

WindowBackendApi sdlWindowBackendApi = {
    .name                        = "sdl",
    .added                       = sdlWindowSystemAdded,
    .removed                     = sdlWindowSystemRemoved,
    .preUpdate                   = sdlWindowSystemPreUpdate,
    .postUpdate                  = sdlWindowSystemPostUpdate,
    .hide                        = sdlWindowSystemHide,
    .show                        = sdlWindowSystemShow,
    .toggleFullscreen            = sdlWindowSystemToggleFullscreen,
    .reloadCursors               = sdlWindowSystemReloadCursors,
    .getPointerCursor            = sdlWindowSystemGetPointerCursor,
    .getArrowCursor              = sdlWindowSystemGetArrowCursor,
    .getTextCursor               = sdlWindowSystemGetTextCursor,
    .updateDimensions            = sdlWindowSystemUpdateDimensions,
    .warpCenter                  = sdlWindowSystemWarpCenter,
    .warp                        = sdlWindowSystemWarp,
    .hideCursor                  = sdlWindowSystemHideCursor,
    .showCursor                  = sdlWindowSystemShowCursor,
    .isCursorVisible             = sdlWindowSystemIsCursorVisible,
    .getRelativeMouseDelta       = sdlWindowSystemGetRelativeMouseDelta,
    .isLeftMouseDown             = sdlWindowSystemIsLeftMouseDown,
    .isRightMouseDown            = sdlWindowSystemIsRightMouseDown,
    .isMiddleMouseDown           = sdlWindowSystemIsMiddleMouseDown,
    .getRequiredVulkanExtensions = sdlWindowSystemGetRequiredVulkanExtensions,
    .createVulkanSurface         = sdlWindowSystemCreateVulkanSurface,
};

// ── Window ──────────────────────────────────────────────────────────────────

void sdlWindowSystemAdded(void) {
    initSDL();
    setDimensions();
    createWindow();
    loadCursors();
    SDL_StartTextInput(window.sdlWindowHandle);

    const int linked = SDL_GetVersion();
    debug("windowSystem: window engine SDL %d.%d.%d",
          SDL_VERSIONNUM_MAJOR(linked),
          SDL_VERSIONNUM_MINOR(linked),
          SDL_VERSIONNUM_MICRO(linked));
    debug("windowSystem: dimensions    %dx%d", window.width, window.height);
}

static void windowRemovedDelayed(void* _) {
    // input cleanup
    foreach (SDL_Gamepad* gamepad, gamepads) {
        SDL_CloseGamepad(gamepad);
    }
    arrayFree(gamepads);
    arrayFree(input.events);

    // window cleanup
    SDL_DestroyCursor(cursorArrow);
    SDL_DestroyCursor(cursorHand);
    SDL_DestroyCursor(cursorText);
    SDL_DestroyWindow(window.sdlWindowHandle);
    SDL_Quit();
}

void sdlWindowSystemRemoved(void) {
    // let render system handle it's freeing first
    futureTaskAdd(0, windowRemovedDelayed, NULL);
}

void initSDL(void) {
    SDL_SetHint(SDL_HINT_JOYSTICK_LINUX_CLASSIC, "1");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "vulkan");
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD);
}

void setDimensions(void) {
    int primaryDisplayId                  = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode* primaryDisplay = SDL_GetCurrentDisplayMode(primaryDisplayId);

    if (settingsGetBool("fullScreen")) {
        window.width  = primaryDisplay->w;
        window.height = primaryDisplay->h;
    } else {
        window.width  = primaryDisplay->w * 0.75F;
        window.height = window.width / 1.77F;

        // window.width  = 1280;
        // window.height = 720;
    }

    window.ratio = (float)window.width / (float)window.height;
}

void createWindow(void) {
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
    if (settingsGetBool("fullScreen")) {
        flags |= SDL_WINDOW_BORDERLESS | SDL_WINDOW_FULLSCREEN;
    }
    SDL_SetHint(SDL_HINT_MOUSE_EMULATE_WARP_WITH_RELATIVE, "1");

    double elapsed         = elapsedBegin();
    window.sdlWindowHandle = SDL_CreateWindow(title, window.width, window.height, flags);
    elapsed                = elapsedEnd(elapsed);
    info("windowSystem: initialized in %.02f ms", elapsed);

    centerWindow();

    float scale   = SDL_GetWindowDisplayScale(window.sdlWindowHandle);
    window.xscale = scale;
    window.yscale = scale;

    if (settingsGetDouble("uiScale") == 0) {
        settingsSetDouble("uiScale", window.xscale);
        if (!window.wayland) {
            settingsSetDouble("cursorScale", window.xscale);
        }
        settingsWrite();
    }
}

void centerWindow(void) {
    int window_x            = 0;
    int window_y            = 0;
    int window_width        = 0;
    int window_height       = 0;
    SDL_Rect rect           = {};
    SDL_DisplayID displayId = SDL_GetDisplayForWindow(window.sdlWindowHandle);
    SDL_GetWindowPosition(window.sdlWindowHandle, &window_x, &window_y);
    SDL_GetWindowSize(window.sdlWindowHandle, &window_width, &window_height);
    SDL_GetDisplayBounds(displayId, &rect);

    window_width  = window_width * 0.5F;
    window_height = window_height * 0.5F;
    window_x      = window_x + window_width;
    window_y      = window_y + window_height;
    SDL_SetWindowPosition(window.sdlWindowHandle,
                          (rect.x + (rect.w * 0.5F) - window_width),
                          (rect.y + (rect.h * 0.5F) - window_height));
}

SDL_Cursor* loadCursor(const char* path, float xHot, float yHot) {
    Image image = imageLoadKtx(path, KTX_FORMAT_RGBA32);

    u64 resizedWidth  = (int)(image.width / 2.5F * settingsGetDouble("cursorScale"));
    u64 resizedHeight = (int)(image.height / 2.5F * settingsGetDouble("cursorScale"));
    u64 resizedHotX   = (int)(xHot / 2.5F * settingsGetDouble("cursorScale"));
    u64 resizedHotY   = (int)(yHot / 2.5F * settingsGetDouble("cursorScale"));

    Image resizedImage = imageResize(&image, resizedWidth, resizedHeight);

    SDL_Surface* surface = SDL_CreateSurfaceFrom(resizedWidth,
                                                 resizedHeight,
                                                 SDL_PIXELFORMAT_RGBA32,
                                                 resizedImage.data,
                                                 4 * resizedWidth);
    SDL_Cursor* cursor   = SDL_CreateColorCursor(surface, resizedHotX, resizedHotY);

    imageDestory(&image);
    imageDestory(&resizedImage);
    SDL_DestroySurface(surface);

    return cursor;
}

void loadCursors(void) {
    if (cursorArrow) {
        SDL_DestroyCursor(cursorArrow);
        SDL_DestroyCursor(cursorHand);
        SDL_DestroyCursor(cursorText);
    }

    cursorArrow = loadCursor("images/cursorArrow.png.ktx2", 8, 8);
    cursorHand  = loadCursor("images/cursorHand.png.ktx2", 25, 4);
    cursorText  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
    SDL_SetCursor(cursorArrow);
}

void* sdlWindowSystemGetPointerCursor(void) {
    return cursorHand;
}

void* sdlWindowSystemGetArrowCursor(void) {
    return cursorArrow;
}

void* sdlWindowSystemGetTextCursor(void) {
    return cursorText;
}

void sdlWindowSystemHide(void) {
    SDL_HideWindow(window.sdlWindowHandle);
}

void sdlWindowSystemShow(void) {
    SDL_ShowWindow(window.sdlWindowHandle);
}

void sdlWindowSystemReloadCursors(void) {
    loadCursors();
}

void sdlWindowSystemToggleFullscreen(char fullScreen) {
    SDL_Rect rect           = {};
    SDL_DisplayID displayId = SDL_GetDisplayForWindow(window.sdlWindowHandle);
    SDL_GetDisplayBounds(displayId, &rect);

    if (fullScreen) {
        SDL_SetWindowFullscreen(window.sdlWindowHandle, 1);
    } else {
        SDL_SetWindowFullscreen(window.sdlWindowHandle, 0);

        int width  = rect.w * 0.75F;
        int height = width / 1.77F;
        int x      = (rect.w - width) / 2;
        int y      = (rect.h - height) / 2;

        SDL_SetWindowBordered(window.sdlWindowHandle, true);
        SDL_SetWindowSize(window.sdlWindowHandle, width, height);
        SDL_SetWindowPosition(window.sdlWindowHandle, x, y);
        SDL_RestoreWindow(window.sdlWindowHandle);
    }
}

void sdlWindowSystemUpdateDimensions(void) {
    SDL_GetWindowSize(window.sdlWindowHandle, &window.width, &window.height);
}

void sdlWindowSystemWarpCenter(void) {
    SDL_WarpMouseInWindow(window.sdlWindowHandle, window.width / 2.0F, window.height / 2.0F);
}

void sdlWindowSystemWarp(float x, float y) {
    SDL_WarpMouseInWindow(window.sdlWindowHandle, x, y);
}

static bool cursorVisible = true;
static float cursorSaveX, cursorSaveY;

void sdlWindowSystemHideCursor(void) {
    cursorVisible = false;
    SDL_GetMouseState(&cursorSaveX, &cursorSaveY);
    SDL_HideCursor();
    SDL_SetWindowRelativeMouseMode(window.sdlWindowHandle, true);
}

static void showCursorDelayed(void*) {
    SDL_ShowCursor();
}

void sdlWindowSystemShowCursor(void) {
    cursorVisible = true;
    SDL_SetWindowRelativeMouseMode(window.sdlWindowHandle, false);
    SDL_WarpMouseInWindow(window.sdlWindowHandle, cursorSaveX, cursorSaveY);
    futureTaskAdd(10, showCursorDelayed, NULL);
}

bool sdlWindowSystemIsCursorVisible(void) {
    return cursorVisible;
}

static MouseButton windowSystemMapSDLMouseButton(u8 button) {
    switch (button) {
        case SDL_BUTTON_LEFT:
            return MOUSE_BUTTON_LEFT;
        case SDL_BUTTON_RIGHT:
            return MOUSE_BUTTON_RIGHT;
        case SDL_BUTTON_MIDDLE:
            return MOUSE_BUTTON_MIDDLE;
        case SDL_BUTTON_X1:
            return MOUSE_BUTTON_X1;
        case SDL_BUTTON_X2:
            return MOUSE_BUTTON_X2;
        default:
            return MOUSE_BUTTON_NONE;
    }
}

static KeyCode windowSystemMapSDLKey(SDL_Keycode key) {
    switch (key) {
        case SDLK_A:
            return KEY_A;
        case SDLK_B:
            return KEY_B;
        case SDLK_C:
            return KEY_C;
        case SDLK_D:
            return KEY_D;
        case SDLK_E:
            return KEY_E;
        case SDLK_F:
            return KEY_F;
        case SDLK_M:
            return KEY_M;
        case SDLK_P:
            return KEY_P;
        case SDLK_R:
            return KEY_R;
        case SDLK_S:
            return KEY_S;
        case SDLK_T:
            return KEY_T;
        case SDLK_W:
            return KEY_W;
        case SDLK_X:
            return KEY_X;
        case SDLK_1:
            return KEY_1;
        case SDLK_2:
            return KEY_2;
        case SDLK_5:
            return KEY_5;
        case SDLK_ESCAPE:
            return KEY_ESCAPE;
        case SDLK_SPACE:
            return KEY_SPACE;
        case SDLK_RETURN:
            return KEY_RETURN;
        case SDLK_KP_ENTER:
            return KEY_KP_ENTER;
        case SDLK_KP_PLUS:
            return KEY_KP_PLUS;
        case SDLK_KP_MINUS:
            return KEY_KP_MINUS;
        case SDLK_F8:
            return KEY_F8;
        case SDLK_LALT:
            return KEY_LALT;
        case SDLK_RALT:
            return KEY_RALT;
        case SDLK_LSHIFT:
            return KEY_LSHIFT;
        case SDLK_RSHIFT:
            return KEY_RSHIFT;
        case SDLK_LCTRL:
            return KEY_LCTRL;
        case SDLK_RCTRL:
            return KEY_RCTRL;
        case SDLK_UP:
            return KEY_UP;
        case SDLK_DOWN:
            return KEY_DOWN;
        case SDLK_LEFT:
            return KEY_LEFT;
        case SDLK_RIGHT:
            return KEY_RIGHT;
        case SDLK_TAB:
            return KEY_TAB;
        case SDLK_H:
            return KEY_H;
        case SDLK_N:
            return KEY_N;
        case SDLK_F1:
            return KEY_F1;
        case SDLK_F2:
            return KEY_F2;
        case SDLK_F3:
            return KEY_F3;
        case SDLK_BACKSPACE:
            return KEY_BACKSPACE;
        case SDLK_DELETE:
            return KEY_DELETE;
        default:
            return KEY_NONE;
    }
}

void sdlWindowSystemGetRelativeMouseDelta(float* dx, float* dy) {
    float mouseDx = 0.0f;
    float mouseDy = 0.0f;
    SDL_GetRelativeMouseState(&mouseDx, &mouseDy);
    if (dx) *dx = mouseDx;
    if (dy) *dy = mouseDy;
}

static bool windowSystemIsMouseButtonMaskSet(u32 mask) {
    u32 mouseState = SDL_GetMouseState(NULL, NULL);
    return (mouseState & mask) != 0;
}

bool sdlWindowSystemIsLeftMouseDown(void) {
    return windowSystemIsMouseButtonMaskSet(SDL_BUTTON_LMASK);
}

bool sdlWindowSystemIsRightMouseDown(void) {
    return windowSystemIsMouseButtonMaskSet(SDL_BUTTON_RMASK);
}

bool sdlWindowSystemIsMiddleMouseDown(void) {
    return windowSystemIsMouseButtonMaskSet(SDL_BUTTON_MMASK);
}

char const* const* sdlWindowSystemGetRequiredVulkanExtensions(u32* extensionCount) {
    return SDL_Vulkan_GetInstanceExtensions(extensionCount);
}

bool sdlWindowSystemCreateVulkanSurface(VkInstance instance, VkSurfaceKHR* surface) {
    return SDL_Vulkan_CreateSurface(window.sdlWindowHandle, instance, NULL, surface);
}

static void windowSystemPushInputEvent(SDL_Event* event) {
    InputEvent inputEvent = {};
    inputEvent.ctrl       = input.ctrl;
    inputEvent.shift      = input.shift;
    inputEvent.alt        = input.alt;

    switch (event->type) {
        case SDL_EVENT_KEY_DOWN:
            inputEvent.type            = INPUT_EVENT_KEY_DOWN;
            inputEvent.data.key.key    = windowSystemMapSDLKey(event->key.key);
            inputEvent.data.key.repeat = event->key.repeat;
            break;
        case SDL_EVENT_KEY_UP:
            inputEvent.type         = INPUT_EVENT_KEY_UP;
            inputEvent.data.key.key = windowSystemMapSDLKey(event->key.key);
            break;
        case SDL_EVENT_MOUSE_MOTION:
            inputEvent.type           = INPUT_EVENT_MOUSE_MOVE;
            inputEvent.data.motion.x  = event->motion.x;
            inputEvent.data.motion.y  = event->motion.y;
            inputEvent.data.motion.dx = event->motion.xrel;
            inputEvent.data.motion.dy = event->motion.yrel;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            inputEvent.type = INPUT_EVENT_MOUSE_BUTTON_DOWN;
            inputEvent.data.mouseButton.button =
                windowSystemMapSDLMouseButton(event->button.button);
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            inputEvent.type = INPUT_EVENT_MOUSE_BUTTON_UP;
            inputEvent.data.mouseButton.button =
                windowSystemMapSDLMouseButton(event->button.button);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            inputEvent.type         = INPUT_EVENT_MOUSE_WHEEL;
            inputEvent.data.wheel.x = event->wheel.x;
            inputEvent.data.wheel.y = event->wheel.y;
            break;
        case SDL_EVENT_TEXT_INPUT:
            inputEvent.type = INPUT_EVENT_TEXT_INPUT;
            strncpy(inputEvent.data.text.text,
                    event->text.text,
                    sizeof(inputEvent.data.text.text) - 1);
            break;
        case SDL_EVENT_WINDOW_RESIZED:
            inputEvent.type               = INPUT_EVENT_WINDOW_RESIZED;
            inputEvent.data.resize.width  = event->window.data1;
            inputEvent.data.resize.height = event->window.data2;
            break;
        case SDL_EVENT_QUIT:
            inputEvent.type = INPUT_EVENT_QUIT;
            break;
        default:
            return;
    }

    arrayPut(input.events, inputEvent);
}

// ── Input ───────────────────────────────────────────────────────────────────

static char windowHasFocus(void) {
    return (SDL_GetWindowFlags(window.sdlWindowHandle) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

void sdlWindowSystemPreUpdate(void) {
    sdlInputReset();
    input.focused = windowHasFocus();
    arrayClear(input.events);

    SDL_Event event = {};
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_JOYSTICK_ADDED && SDL_IsGamepad(event.adevice.which)) {
            SDL_Gamepad* gamepad = SDL_OpenGamepad(event.adevice.which);
            arrayPut(gamepads, gamepad);
            debug("  controller    : %s", SDL_GetGamepadName(gamepad));
        }

        if (event.type == SDL_EVENT_KEY_DOWN /* && !event.key.repeat */) {
            KeyCode key  = windowSystemMapSDLKey(event.key.key);
            input.action = event.type;
            input.key    = key;
            if (key > KEY_NONE && key < KEY_COUNT) {
                input.repeating[key] = 1;
            }
            if (!event.key.repeat) {
                input.pressed = key;
            }
            if (key == KEY_LALT || key == KEY_RALT) {
                input.alt = 1;
            }
            if (key == KEY_LSHIFT || key == KEY_RSHIFT) {
                input.shift = 1;
            }
            if (key == KEY_LCTRL || key == KEY_RCTRL) {
                input.ctrl = 1;
            }
        } else if (event.type == SDL_EVENT_KEY_UP) {
            KeyCode key  = windowSystemMapSDLKey(event.key.key);
            input.action = event.type;
            input.key    = key;
            if (key > KEY_NONE && key < KEY_COUNT) {
                input.repeating[key] = 0;
            }
            input.released = key;
            if (key == KEY_LALT || key == KEY_RALT) {
                input.alt = 0;
            }
            if (key == KEY_LSHIFT || key == KEY_RSHIFT) {
                input.shift = 0;
            }
            if (key == KEY_LCTRL || key == KEY_RCTRL) {
                input.ctrl = 0;
            }
        } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
            if (cursorVisible) {
                input.lastX  = input.xpos;
                input.lastY  = input.ypos;
                input.xpos   = event.motion.x;
                input.ypos   = event.motion.y;
                input.deltaX = input.xpos - input.lastX;
                input.deltaY = input.ypos - input.lastY;
            } else {
                input.deltaX += event.motion.xrel;
                input.deltaY += event.motion.yrel;
            }
        }

        if (event.type == SDL_EVENT_TEXT_INPUT) {
            input.character = (unsigned char)event.text.text[0];
        }

        if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            input.scrollX = event.wheel.x;
            input.scrollY = event.wheel.y;
        }

        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            input.gamepadButtonPressed[event.gbutton.button] = 1;
        }
        if (event.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
            input.gamepadButtonReleased[event.gbutton.button] = 1;
        }

        if (event.type == SDL_EVENT_WINDOW_RESIZED) {
            SDL_GetWindowSize(window.sdlWindowHandle, &window.width, &window.height);
            window.ratio = (float)window.width / (float)window.height;
            signalEmit("windowResized", NULL);
        }

        if (event.type == SDL_EVENT_QUIT) {
            engineStop();
        }

        windowSystemPushInputEvent(&event);
    }

    if (input.alt && (input.pressed == KEY_RETURN || input.pressed == KEY_KP_ENTER)) {
        input.skip      = 1;
        char fullScreen = settingsGetBool("fullScreen");
        settingsSetBool("fullScreen", !fullScreen);
        settingsWrite();
        sdlWindowSystemToggleFullscreen(!fullScreen);
    }

    if (input.alt && input.pressed == KEY_E) {
        engineStop();
    }
}

void sdlWindowSystemPostUpdate(void) {}

void sdlInputReset(void) {
    input.key       = -1;
    input.scancode  = -1;
    input.action    = -1;
    input.mods      = -1;
    input.pressed   = -1;
    input.released  = -1;
    input.character = -1;

    input.mouseButton = -1;
    input.mouseAction = -1;
    input.mouseMods   = -1;

    input.scrollY = 0;
    input.scrollX = 0;

    input.lastX = input.xpos;
    input.lastY = input.ypos;

    input.deltaX = input.xpos - input.lastX;
    input.deltaY = input.ypos - input.lastY;

    input.skip = 0;
}

char sdlInputShouldProcess(void) {
    return input.focused && !input.skip;
}
