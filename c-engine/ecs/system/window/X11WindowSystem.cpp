#ifdef __linux__

#include "ecs/system/window/WindowSystem.h"
#include "ecs/system/window/X11Backend.h"
#include "Engine.h"
#include "Utils.h"
#include "image/Image.h"

#include "SDL3/SDL_gamepad.h"
#include "SDL3/SDL_init.h"

// X keysyms (duplicated here to avoid including X11 headers)
#define XK_a 0x0061
#define XK_A 0x0041
#define XK_b 0x0062
#define XK_B 0x0042
#define XK_c 0x0063
#define XK_C 0x0043
#define XK_d 0x0064
#define XK_D 0x0044
#define XK_e 0x0065
#define XK_E 0x0045
#define XK_f 0x0066
#define XK_F 0x0046
#define XK_m 0x006d
#define XK_M 0x004d
#define XK_p 0x0070
#define XK_P 0x0050
#define XK_r 0x0072
#define XK_R 0x0052
#define XK_s 0x0073
#define XK_S 0x0053
#define XK_t 0x0074
#define XK_T 0x0054
#define XK_w 0x0077
#define XK_W 0x0057
#define XK_x 0x0078
#define XK_X 0x0058
#define XK_1 0x0031
#define XK_2 0x0032
#define XK_5 0x0035
#define XK_Escape 0xff1b
#define XK_space 0x0020
#define XK_Return 0xff0d
#define XK_KP_Enter 0xff8d
#define XK_KP_Add 0xffab
#define XK_KP_Subtract 0xffad
#define XK_F8 0xffc5
#define XK_Alt_L 0xffe9
#define XK_Alt_R 0xffea
#define XK_Shift_L 0xffe1
#define XK_Shift_R 0xffe2
#define XK_Control_L 0xffe3
#define XK_Control_R 0xffe4
#define XK_Up 0xff52
#define XK_Down 0xff54
#define XK_Left 0xff51
#define XK_Right 0xff53
#define XK_Tab 0xff09
#define XK_H 0x0068
#define XK_N 0x006e
#define XK_F1 0xffbe
#define XK_F2 0xffbf
#define XK_F3 0xffc0
#define XK_BackSpace 0xff08
#define XK_Delete 0xffff

// ── WindowBackendApi ────────────────────────────────────────────────────────
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

// ── State ───────────────────────────────────────────────────────────────────

static X11Backend* backend;
static void x11LoadCursors(void);
static bool cursorVisible = true;
static float cursorSaveX, cursorSaveY;
static float pendingRelDx, pendingRelDy;
static u32 mouseButtonState;
static Array(SDL_Gamepad*) gamepads;

// ── Key mapping ─────────────────────────────────────────────────────────────

static KeyCode mapKeySym(u32 sym) {
    switch (sym) {
        case XK_a:
        case XK_A:
            return KEY_A;
        case XK_b:
        case XK_B:
            return KEY_B;
        case XK_c:
        case XK_C:
            return KEY_C;
        case XK_d:
        case XK_D:
            return KEY_D;
        case XK_e:
        case XK_E:
            return KEY_E;
        case XK_f:
        case XK_F:
            return KEY_F;
        case XK_m:
        case XK_M:
            return KEY_M;
        case XK_p:
        case XK_P:
            return KEY_P;
        case XK_r:
        case XK_R:
            return KEY_R;
        case XK_s:
        case XK_S:
            return KEY_S;
        case XK_t:
        case XK_T:
            return KEY_T;
        case XK_w:
        case XK_W:
            return KEY_W;
        case XK_x:
        case XK_X:
            return KEY_X;
        case XK_1:
            return KEY_1;
        case XK_2:
            return KEY_2;
        case XK_5:
            return KEY_5;
        case XK_Escape:
            return KEY_ESCAPE;
        case XK_space:
            return KEY_SPACE;
        case XK_Return:
            return KEY_RETURN;
        case XK_KP_Enter:
            return KEY_KP_ENTER;
        case XK_KP_Add:
            return KEY_KP_PLUS;
        case XK_KP_Subtract:
            return KEY_KP_MINUS;
        case XK_F8:
            return KEY_F8;
        case XK_Alt_L:
            return KEY_LALT;
        case XK_Alt_R:
            return KEY_RALT;
        case XK_Shift_L:
            return KEY_LSHIFT;
        case XK_Shift_R:
            return KEY_RSHIFT;
        case XK_Control_L:
            return KEY_LCTRL;
        case XK_Control_R:
            return KEY_RCTRL;
        case XK_Up:
            return KEY_UP;
        case XK_Down:
            return KEY_DOWN;
        case XK_Left:
            return KEY_LEFT;
        case XK_Right:
            return KEY_RIGHT;
        case XK_Tab:
            return KEY_TAB;
        case XK_H:
            return KEY_H;
        case XK_N:
            return KEY_N;
        case XK_F1:
            return KEY_F1;
        case XK_F2:
            return KEY_F2;
        case XK_F3:
            return KEY_F3;
        case XK_BackSpace:
            return KEY_BACKSPACE;
        case XK_Delete:
            return KEY_DELETE;
        default:
            return KEY_NONE;
    }
}

static MouseButton mapButton(unsigned int button) {
    switch (button) {
        case 1:
            return MOUSE_BUTTON_LEFT;
        case 2:
            return MOUSE_BUTTON_MIDDLE;
        case 3:
            return MOUSE_BUTTON_RIGHT;
        case 8:
            return MOUSE_BUTTON_X1;
        case 9:
            return MOUSE_BUTTON_X2;
        default:
            return MOUSE_BUTTON_NONE;
    }
}

static u32 buttonToMask(unsigned int button) {
    switch (button) {
        case 1:
            return (1u << 0);
        case 2:
            return (1u << 1);
        case 3:
            return (1u << 2);
        default:
            return 0;
    }
}

// ── InputEvent push helpers ─────────────────────────────────────────────────

static void pushKeyEvent(int type, KeyCode key, bool repeat) {
    InputEvent ev      = {};
    ev.ctrl            = input.ctrl;
    ev.shift           = input.shift;
    ev.alt             = input.alt;
    ev.type            = static_cast<InputEventType>(type);
    ev.data.key.key    = key;
    ev.data.key.repeat = repeat;
    arrayPut(input.events, ev);
}

static void pushMouseMoveEvent(float x, float y, float dx, float dy) {
    InputEvent ev     = {};
    ev.ctrl           = input.ctrl;
    ev.shift          = input.shift;
    ev.alt            = input.alt;
    ev.type           = INPUT_EVENT_MOUSE_MOVE;
    ev.data.motion.x  = x;
    ev.data.motion.y  = y;
    ev.data.motion.dx = dx;
    ev.data.motion.dy = dy;
    arrayPut(input.events, ev);
}

static void pushMouseButtonEvent(int type, MouseButton button) {
    InputEvent ev              = {};
    ev.ctrl                    = input.ctrl;
    ev.shift                   = input.shift;
    ev.alt                     = input.alt;
    ev.type                    = static_cast<InputEventType>(type);
    ev.data.mouseButton.button = button;
    arrayPut(input.events, ev);
}

static void pushMouseWheelEvent(float x, float y) {
    InputEvent ev   = {};
    ev.ctrl         = input.ctrl;
    ev.shift        = input.shift;
    ev.alt          = input.alt;
    ev.type         = INPUT_EVENT_MOUSE_WHEEL;
    ev.data.wheel.x = x;
    ev.data.wheel.y = y;
    arrayPut(input.events, ev);
}

static void pushTextEvent(const char* text) {
    InputEvent ev = {};
    ev.ctrl       = input.ctrl;
    ev.shift      = input.shift;
    ev.alt        = input.alt;
    ev.type       = INPUT_EVENT_TEXT_INPUT;
    strncpy(ev.data.text.text, text, sizeof(ev.data.text.text) - 1);
    arrayPut(input.events, ev);
}

static void pushResizeEvent(int w, int h) {
    InputEvent ev         = {};
    ev.type               = INPUT_EVENT_WINDOW_RESIZED;
    ev.data.resize.width  = w;
    ev.data.resize.height = h;
    arrayPut(input.events, ev);
}

static void pushQuitEvent(void) {
    InputEvent ev = {};
    ev.type       = INPUT_EVENT_QUIT;
    arrayPut(input.events, ev);
}

// ── Backend API ─────────────────────────────────────────────────────────────

static void x11Added(void) {
    int screenW, screenH;

    // Create a temporary backend to get screen size, then the real one
    // Actually, let backend handle dimensions
    int width, height;
    bool fullScreen = settingsGetBool("fullScreen");

    // We need screen size first — create backend, it'll figure it out
    double elapsed = elapsedBegin();
    // Temporary: get screen size from a quick XOpenDisplay
    // Actually the backend needs width/height. Compute here.
    // For now, create with dummy size, then query screen
    backend = x11BackendCreate("Mini", 800, 600, false);
    x11BackendGetScreenSize(backend, &screenW, &screenH);
    x11BackendDestroy(backend);

    if (fullScreen) {
        width  = screenW;
        height = screenH;
    } else {
        width  = screenW * 0.85f;
        height = width / 1.77f;
    }
    window.width  = width;
    window.height = height;
    window.ratio  = (float)width / (float)height;

    backend = x11BackendCreate("Mini", width, height, fullScreen);
    elapsed = elapsedEnd(elapsed);

    window.xscale           = 1.0f;
    window.yscale           = 1.0f;
    window.sdlWindowHandle  = NULL;
    window.glfwWindowHandle = NULL;

    if (settingsGetDouble("uiScale") == 0) {
        settingsSetDouble("uiScale", 1.0);
        settingsSetDouble("cursorScale", 1.0);
        settingsWrite();
    }

    info("windowSystem: initialized in %.02f ms (X11)", elapsed);
    debug("windowSystem: dimensions    %dx%d", window.width, window.height);

    x11LoadCursors();

    // SDL for gamepad only
    SDL_SetHint("SDL_JOYSTICK_LINUX_CLASSIC", "1");
    SDL_Init(SDL_INIT_GAMEPAD);
}

static void x11RemovedDelayed(void* _) {
    (void)_;
    foreach (SDL_Gamepad* gp, gamepads) {
        SDL_CloseGamepad(gp);
    }
    arrayFree(gamepads);
    arrayFree(input.events);
    x11BackendDestroy(backend);
    backend = NULL;
    SDL_Quit();
}

static void x11Removed(void) {
    futureTaskAdd(0, x11RemovedDelayed, NULL);
}

static void x11Hide(void) {
    x11BackendHide(backend);
}

static void x11Show(void) {
    x11BackendShow(backend);
}

static void x11ToggleFullscreen(char fullScreen) {
    x11BackendToggleFullscreen(backend, fullScreen);
    if (!fullScreen) {
        int screenW, screenH;
        x11BackendGetScreenSize(backend, &screenW, &screenH);
        int w = screenW * 0.85f;
        int h = w / 1.77f;
        int x = (screenW - w) / 2;
        int y = (screenH - h) / 2;
        x11BackendMoveResize(backend, x, y, w, h);
    }
}

static void x11SetCursorType(int cursorType) {
    if (!backend) return;
    switch (cursorType) {
        case 1:
            x11BackendDefineCursorHand(backend);
            break;  // pointer
        case 2:
            x11BackendDefineCursorText(backend);
            break;  // text
        default:
            x11BackendDefineCursorArrow(backend);
            break;  // arrow and others
    }
}

static void x11LoadCursors(void) {
    double scale = settingsGetDouble("cursorScale");
    if (scale <= 0) scale = 1.0;

    Image arrowImg     = imageLoadKtx("images/cursorArrow.png.ktx2", KTX_FORMAT_RGBA32);
    u64 aw             = (u64)(arrowImg.width / 2.5 * scale);
    u64 ah             = (u64)(arrowImg.height / 2.5 * scale);
    u64 ahx            = (u64)(8 / 2.5 * scale);
    u64 ahy            = (u64)(8 / 2.5 * scale);
    Image arrowResized = imageResize(&arrowImg, aw, ah);
    x11BackendSetCustomCursorArrow(backend,
                                   reinterpret_cast<const unsigned char*>(arrowResized.data),
                                   (int)aw,
                                   (int)ah,
                                   (int)ahx,
                                   (int)ahy);
    imageDestory(&arrowImg);
    imageDestory(&arrowResized);

    Image handImg     = imageLoadKtx("images/cursorHand.png.ktx2", KTX_FORMAT_RGBA32);
    u64 hw            = (u64)(handImg.width / 2.5 * scale);
    u64 hh            = (u64)(handImg.height / 2.5 * scale);
    u64 hhx           = (u64)(25 / 2.5 * scale);
    u64 hhy           = (u64)(4 / 2.5 * scale);
    Image handResized = imageResize(&handImg, hw, hh);
    x11BackendSetCustomCursorHand(backend, reinterpret_cast<const unsigned char*>(handResized.data), (int)hw, (int)hh, (int)hhx, (int)hhy);
    imageDestory(&handImg);
    imageDestory(&handResized);
}

static void x11ReloadCursors(void) {
    x11LoadCursors();
}

static void* x11GetPointerCursor(void) {
    return NULL;
}  // cursors managed internally

static void* x11GetArrowCursor(void) {
    return NULL;
}

static void* x11GetTextCursor(void) {
    return NULL;
}

static void x11UpdateDimensions(void) {
    x11BackendGetSize(backend, &window.width, &window.height);
}

static void x11WarpCenter(void) {
    x11BackendWarp(backend, window.width / 2, window.height / 2);
}

static void x11Warp(float x, float y) {
    x11BackendWarp(backend, (int)x, (int)y);
}

static void x11HideCursor(void) {
    int wx, wy;
    x11BackendQueryPointer(backend, &wx, &wy);
    cursorSaveX   = (float)wx;
    cursorSaveY   = (float)wy;
    cursorVisible = false;
    x11BackendSetCursorVisible(backend, false);
    x11BackendWarp(backend, window.width / 2, window.height / 2);
}

static void showCursorDelayed(void*) {
    x11BackendSetCursorVisible(backend, true);
}

static void x11ShowCursor(void) {
    cursorVisible = true;
    x11BackendWarp(backend, (int)cursorSaveX, (int)cursorSaveY);
    futureTaskAdd(10, showCursorDelayed, NULL);
    // x11BackendSetCursorVisible(backend, true);
}

static bool x11IsCursorVisible(void) {
    return cursorVisible;
}

static void x11GetRelativeMouseDelta(float* dx, float* dy) {
    if (!cursorVisible) {
        if (dx) *dx = pendingRelDx;
        if (dy) *dy = pendingRelDy;
        pendingRelDx = 0;
        pendingRelDy = 0;
    } else {
        if (dx) *dx = 0;
        if (dy) *dy = 0;
    }
}

static bool x11IsLeftMouseDown(void) {
    return (mouseButtonState & (1u << 0)) != 0;
}

static bool x11IsRightMouseDown(void) {
    return (mouseButtonState & (1u << 2)) != 0;
}

static bool x11IsMiddleMouseDown(void) {
    return (mouseButtonState & (1u << 1)) != 0;
}

static char const* const* x11GetRequiredVulkanExtensions(u32* extensionCount) {
    return x11BackendGetVulkanExtensions(extensionCount);
}

static bool x11CreateVulkanSurface(VkInstance instance, VkSurfaceKHR* surface) {
    return x11BackendCreateVulkanSurface(backend, (void*)instance, (void*)surface);
}

static SetCursorFn x11GetSetCursorFn(void) {
    return x11SetCursorType;
}

// ── Event processing ────────────────────────────────────────────────────────

static void x11PreUpdate(void) {
    // Reset input
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
    memset(input.gamepadButtonPressed, 0, sizeof(input.gamepadButtonPressed));
    memset(input.gamepadButtonReleased, 0, sizeof(input.gamepadButtonReleased));

    input.focused = x11BackendHasFocus(backend);
    arrayClear(input.events);

    // SDL gamepad events
    SDL_Event sdlEvent;
    while (SDL_PollEvent(&sdlEvent)) {
        if (sdlEvent.type == SDL_EVENT_JOYSTICK_ADDED && SDL_IsGamepad(sdlEvent.adevice.which)) {
            SDL_Gamepad* gp = SDL_OpenGamepad(sdlEvent.adevice.which);
            arrayPut(gamepads, gp);
            debug("  controller    : %s", SDL_GetGamepadName(gp));
        }
        if (sdlEvent.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
            input.gamepadButtonPressed[sdlEvent.gbutton.button] = 1;
        if (sdlEvent.type == SDL_EVENT_GAMEPAD_BUTTON_UP)
            input.gamepadButtonReleased[sdlEvent.gbutton.button] = 1;
    }

    // X11 events
    X11Event events[256];
    int count = x11BackendPollEvents(backend, events, 256);

    for (int i = 0; i < count; i++) {
        X11Event* e = &events[i];
        switch (e->type) {
            case X11_EVENT_KEY_DOWN: {
                KeyCode key  = mapKeySym(e->key.keysym);
                input.action = 1;
                input.key    = key;
                if (key > KEY_NONE && key < KEY_COUNT) input.repeating[key] = 1;
                if (!e->key.repeat) input.pressed = key;
                if (key == KEY_LALT || key == KEY_RALT) input.alt = 1;
                if (key == KEY_LSHIFT || key == KEY_RSHIFT) input.shift = 1;
                if (key == KEY_LCTRL || key == KEY_RCTRL) input.ctrl = 1;
                pushKeyEvent(INPUT_EVENT_KEY_DOWN, key, e->key.repeat);
            } break;

            case X11_EVENT_KEY_UP: {
                KeyCode key  = mapKeySym(e->key.keysym);
                input.action = 0;
                input.key    = key;
                if (key > KEY_NONE && key < KEY_COUNT) input.repeating[key] = 0;
                input.released = key;
                if (key == KEY_LALT || key == KEY_RALT) input.alt = 0;
                if (key == KEY_LSHIFT || key == KEY_RSHIFT) input.shift = 0;
                if (key == KEY_LCTRL || key == KEY_RCTRL) input.ctrl = 0;
                pushKeyEvent(INPUT_EVENT_KEY_UP, key, false);
            } break;

            case X11_EVENT_MOTION: {
                if (cursorVisible) {
                    input.lastX  = input.xpos;
                    input.lastY  = input.ypos;
                    input.xpos   = e->motion.x;
                    input.ypos   = e->motion.y;
                    input.deltaX = input.xpos - input.lastX;
                    input.deltaY = input.ypos - input.lastY;
                    pushMouseMoveEvent(e->motion.x, e->motion.y, input.deltaX, input.deltaY);
                }
            } break;

            case X11_EVENT_RAW_MOTION: {
                if (!cursorVisible) {
                    input.deltaX += (float)e->rawMotion.dx;
                    input.deltaY += (float)e->rawMotion.dy;
                    pendingRelDx += (float)e->rawMotion.dx;
                    pendingRelDy += (float)e->rawMotion.dy;
                }
            } break;

            case X11_EVENT_BUTTON_DOWN: {
                mouseButtonState |= buttonToMask(e->button.button);
                MouseButton mb    = mapButton(e->button.button);
                input.mouseButton = mb;
                input.mouseAction = 1;
                pushMouseButtonEvent(INPUT_EVENT_MOUSE_BUTTON_DOWN, mb);
            } break;

            case X11_EVENT_BUTTON_UP: {
                mouseButtonState &= ~buttonToMask(e->button.button);
                MouseButton mb    = mapButton(e->button.button);
                input.mouseButton = mb;
                input.mouseAction = 0;
                pushMouseButtonEvent(INPUT_EVENT_MOUSE_BUTTON_UP, mb);
            } break;

            case X11_EVENT_SCROLL: {
                input.scrollX = e->scroll.x;
                input.scrollY = e->scroll.y;
                pushMouseWheelEvent(e->scroll.x, e->scroll.y);
            } break;

            case X11_EVENT_TEXT: {
                input.character = (unsigned char)e->text.text[0];
                pushTextEvent(e->text.text);
            } break;

            case X11_EVENT_RESIZE: {
                if (e->resize.width != window.width || e->resize.height != window.height) {
                    window.width  = e->resize.width;
                    window.height = e->resize.height;
                    window.ratio  = (float)window.width / (float)window.height;
                    signalEmit("windowResized", NULL);
                    pushResizeEvent(window.width, window.height);
                }
            } break;

            case X11_EVENT_FOCUS_IN:
                input.focused = 1;
                break;
            case X11_EVENT_FOCUS_OUT:
                input.focused = 0;
                break;

            case X11_EVENT_CLOSE: {
                engineStop();
                pushQuitEvent();
            } break;

            default:
                break;
        }
    }

    // Warp to center while cursor hidden
    if (!cursorVisible) {
        x11BackendWarp(backend, window.width / 2, window.height / 2);
    }

    // Alt+Enter fullscreen
    if (input.alt && (input.pressed == KEY_RETURN || input.pressed == KEY_KP_ENTER)) {
        input.skip      = 1;
        char fullScreen = settingsGetBool("fullScreen");
        settingsSetBool("fullScreen", !fullScreen);
        settingsWrite();
        x11ToggleFullscreen(!fullScreen);
    }

    // Alt+E quit
    if (input.alt && input.pressed == KEY_E) {
        engineStop();
    }
}

static void x11PostUpdate(void) {}

// ── Backend API table ───────────────────────────────────────────────────────

WindowBackendApi x11WindowBackendApi = {
    .name                        = "x11",
    .added                       = x11Added,
    .removed                     = x11Removed,
    .preUpdate                   = x11PreUpdate,
    .postUpdate                  = x11PostUpdate,
    .hide                        = x11Hide,
    .show                        = x11Show,
    .toggleFullscreen            = x11ToggleFullscreen,
    .reloadCursors               = x11ReloadCursors,
    .getPointerCursor            = x11GetPointerCursor,
    .getArrowCursor              = x11GetArrowCursor,
    .getTextCursor               = x11GetTextCursor,
    .updateDimensions            = x11UpdateDimensions,
    .warpCenter                  = x11WarpCenter,
    .warp                        = x11Warp,
    .hideCursor                  = x11HideCursor,
    .showCursor                  = x11ShowCursor,
    .isCursorVisible             = x11IsCursorVisible,
    .getRelativeMouseDelta       = x11GetRelativeMouseDelta,
    .isLeftMouseDown             = x11IsLeftMouseDown,
    .isRightMouseDown            = x11IsRightMouseDown,
    .isMiddleMouseDown           = x11IsMiddleMouseDown,
    .getRequiredVulkanExtensions = x11GetRequiredVulkanExtensions,
    .createVulkanSurface         = x11CreateVulkanSurface,
    .getSetCursorFn              = x11GetSetCursorFn,
};

#endif  // __linux__
