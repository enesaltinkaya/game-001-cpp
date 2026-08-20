#pragma once
#ifdef __linux__

#include <stdbool.h>
#include <stdint.h>

// Opaque X11 state — all X11 headers stay in X11Backend.c
typedef struct X11Backend X11Backend;

// Key/button event data passed back to the engine wrapper
enum X11EventType {
    X11_EVENT_NONE = 0,
    X11_EVENT_KEY_DOWN,
    X11_EVENT_KEY_UP,
    X11_EVENT_MOTION,
    X11_EVENT_BUTTON_DOWN,
    X11_EVENT_BUTTON_UP,
    X11_EVENT_SCROLL,
    X11_EVENT_TEXT,
    X11_EVENT_RESIZE,
    X11_EVENT_FOCUS_IN,
    X11_EVENT_FOCUS_OUT,
    X11_EVENT_CLOSE,
    X11_EVENT_RAW_MOTION,
};

typedef struct X11Event {
    X11EventType type;
    union {
        struct { uint32_t keysym; bool repeat; } key;
        struct { float x, y; }                   motion;
        struct { unsigned int button; }           button;
        struct { float x, y; }                    scroll;
        struct { char text[32]; }                 text;
        struct { int width, height; }             resize;
        struct { double dx, dy; }                 rawMotion;
    };
} X11Event;

X11Backend* x11BackendCreate(const char* title, int width, int height, bool fullscreen);
void        x11BackendDestroy(X11Backend* b);

void        x11BackendShow(X11Backend* b);
void        x11BackendHide(X11Backend* b);
void        x11BackendToggleFullscreen(X11Backend* b, bool fullscreen);
void        x11BackendMoveResize(X11Backend* b, int x, int y, int w, int h);
void        x11BackendGetSize(X11Backend* b, int* w, int* h);
void        x11BackendGetScreenSize(X11Backend* b, int* w, int* h);

// Cursor
void        x11BackendSetCursorVisible(X11Backend* b, bool visible);
void        x11BackendDefineCursorArrow(X11Backend* b);
void        x11BackendDefineCursorHand(X11Backend* b);
void        x11BackendDefineCursorText(X11Backend* b);
void        x11BackendSetCustomCursorArrow(X11Backend* b, const unsigned char* rgba, int w, int h, int hotX, int hotY);
void        x11BackendSetCustomCursorHand(X11Backend* b, const unsigned char* rgba, int w, int h, int hotX, int hotY);

// Warp (grab+warp+ungrab)
void        x11BackendWarp(X11Backend* b, int x, int y);
void        x11BackendQueryPointer(X11Backend* b, int* x, int* y);

// Focus
bool        x11BackendHasFocus(X11Backend* b);

// Events — returns number of events written into `out` (up to `maxEvents`)
int         x11BackendPollEvents(X11Backend* b, X11Event* out, int maxEvents);

// Vulkan
const char** x11BackendGetVulkanExtensions(uint32_t* count);
bool         x11BackendCreateVulkanSurface(X11Backend* b, void* vkInstance, void* vkSurface);

#endif // __linux__
