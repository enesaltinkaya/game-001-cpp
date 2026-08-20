#ifdef __linux__

// This file includes X11 headers — do NOT include engine headers here.
// The X11 `Window`, `Bool`, `KeyCode` types conflict with engine types.

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/Xcursor/Xcursor.h>

#include <vulkan/vulkan.h>

#include <stdlib.h>
#include <string.h>

#include "ecs/system/window/X11Backend.h"

// Vulkan Xlib surface — declared manually to avoid needing VK_USE_PLATFORM_XLIB_KHR globally
namespace engine {
typedef VkFlags VkXlibSurfaceCreateFlagsKHR;
struct VkXlibSurfaceCreateInfoKHR {
    VkStructureType              sType;
    const void*                  pNext;
    VkXlibSurfaceCreateFlagsKHR  flags;
    Display*                     dpy;
    Window                       window;
};
typedef VkResult (*PFN_vkCreateXlibSurfaceKHR)(VkInstance, const VkXlibSurfaceCreateInfoKHR*, const VkAllocationCallbacks*, VkSurfaceKHR*);
#define MY_VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR (static_cast<VkStructureType>(1000004000))

struct X11Backend {
    Display*    dpy;
    int         screen;
    Window      win;
    XIM         xim;
    XIC         xic;

    Atom        wmDeleteWindow;
    Atom        wmState;
    Atom        wmStateFullscreen;
    Atom        wmNetWmName;
    Atom        utf8String;

    Cursor      cursorArrow;
    Cursor      cursorHand;
    Cursor      cursorText;
    Cursor      cursorBlank;

    int         xi2Opcode;
};

// ── Helpers ─────────────────────────────────────────────────────────────────

static Cursor createBlankCursor(Display* dpy, Window win) {
    static char data[1] = {};
    XColor dummy        = {};
    Pixmap blank        = XCreateBitmapFromData(dpy, win, data, 1, 1);
    Cursor cur          = XCreatePixmapCursor(dpy, blank, blank, &dummy, &dummy, 0, 0);
    XFreePixmap(dpy, blank);
    return cur;
}

static bool initXInput2(X11Backend* b) {
    int event, error;
    if (!XQueryExtension(b->dpy, "XInputExtension", &b->xi2Opcode, &event, &error))
        return false;
    int major = 2, minor = 2;
    if (XIQueryVersion(b->dpy, &major, &minor) != Success)
        return false;

    unsigned char mask_bytes[(XI_LASTEVENT + 7) / 8];
    memset(mask_bytes, 0, sizeof(mask_bytes));
    XISetMask(mask_bytes, XI_RawMotion);

    XIEventMask evmask = {
        .deviceid = XIAllMasterDevices,
        .mask_len = sizeof(mask_bytes),
        .mask     = mask_bytes,
    };
    XISelectEvents(b->dpy, DefaultRootWindow(b->dpy), &evmask, 1);
    return true;
}

// ── Create / Destroy ────────────────────────────────────────────────────────

X11Backend* x11BackendCreate(const char* title, int width, int height, bool fullscreen) {
    X11Backend* b = static_cast<X11Backend*>(calloc(1, sizeof(X11Backend)));

    b->dpy    = XOpenDisplay(nullptr);
    b->screen = DefaultScreen(b->dpy);

    b->wmDeleteWindow    = XInternAtom(b->dpy, "WM_DELETE_WINDOW", False);
    b->wmState           = XInternAtom(b->dpy, "_NET_WM_STATE", False);
    b->wmStateFullscreen = XInternAtom(b->dpy, "_NET_WM_STATE_FULLSCREEN", False);
    b->wmNetWmName       = XInternAtom(b->dpy, "_NET_WM_NAME", False);
    b->utf8String        = XInternAtom(b->dpy, "UTF8_STRING", False);

    XSetWindowAttributes swa = {};
    swa.event_mask = ExposureMask | StructureNotifyMask |
                     KeyPressMask | KeyReleaseMask |
                     ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | FocusChangeMask;

    b->win = XCreateWindow(b->dpy, RootWindow(b->dpy, b->screen),
                           0, 0, width, height, 0,
                           CopyFromParent, InputOutput, CopyFromParent,
                           CWEventMask, &swa);

    XSetWMProtocols(b->dpy, b->win, &b->wmDeleteWindow, 1);

    XChangeProperty(b->dpy, b->win, b->wmNetWmName, b->utf8String, 8, PropModeReplace,
                    (unsigned char*)title, strlen(title));
    XStoreName(b->dpy, b->win, title);

{ char resClass[] = "c-game";
  XClassHint hint = {.res_name = const_cast<char*>(title), .res_class = resClass};
  XSetClassHint(b->dpy, b->win, &hint); }

    // Center
    int screenW = DisplayWidth(b->dpy, b->screen);
    int screenH = DisplayHeight(b->dpy, b->screen);
    XMoveWindow(b->dpy, b->win, (screenW - width) / 2, (screenH - height) / 2);

    if (fullscreen) {
        XChangeProperty(b->dpy, b->win, b->wmState, XA_ATOM, 32, PropModeReplace,
                        (unsigned char*)&b->wmStateFullscreen, 1);
    }

    XMapWindow(b->dpy, b->win);
    XFlush(b->dpy);

    // Input method
    b->xim = XOpenIM(b->dpy, nullptr, nullptr, nullptr);
    if (b->xim) {
        b->xic = XCreateIC(b->xim,
                            XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                            XNClientWindow, b->win,
                            XNFocusWindow, b->win,
                            nullptr);
    }

    initXInput2(b);

    // Cursors
    b->cursorArrow = XCreateFontCursor(b->dpy, XC_left_ptr);
    b->cursorHand  = XCreateFontCursor(b->dpy, XC_hand2);
    b->cursorText  = XCreateFontCursor(b->dpy, XC_xterm);
    b->cursorBlank = createBlankCursor(b->dpy, b->win);
    XDefineCursor(b->dpy, b->win, b->cursorArrow);

    return b;
}

void x11BackendDestroy(X11Backend* b) {
    if (b->cursorArrow) XFreeCursor(b->dpy, b->cursorArrow);
    if (b->cursorHand)  XFreeCursor(b->dpy, b->cursorHand);
    if (b->cursorText)  XFreeCursor(b->dpy, b->cursorText);
    if (b->cursorBlank) XFreeCursor(b->dpy, b->cursorBlank);
    if (b->xic) XDestroyIC(b->xic);
    if (b->xim) XCloseIM(b->xim);
    XDestroyWindow(b->dpy, b->win);
    XCloseDisplay(b->dpy);
    free(b);
}

// ── Window operations ───────────────────────────────────────────────────────

void x11BackendShow(X11Backend* b)  { XMapWindow(b->dpy, b->win); XFlush(b->dpy); }
void x11BackendHide(X11Backend* b)  { XUnmapWindow(b->dpy, b->win); XFlush(b->dpy); }

void x11BackendToggleFullscreen(X11Backend* b, bool fullscreen) {
    XEvent ev        = {};
    ev.type          = ClientMessage;
    ev.xclient.window       = b->win;
    ev.xclient.message_type = b->wmState;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = fullscreen ? 1 : 0;
    ev.xclient.data.l[1]    = (long)b->wmStateFullscreen;
    ev.xclient.data.l[2]    = 0;
    XSendEvent(b->dpy, DefaultRootWindow(b->dpy), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(b->dpy);
}

void x11BackendMoveResize(X11Backend* b, int x, int y, int w, int h) {
    XMoveResizeWindow(b->dpy, b->win, x, y, w, h);
    XFlush(b->dpy);
}

void x11BackendGetSize(X11Backend* b, int* w, int* h) {
    XWindowAttributes attr;
    XGetWindowAttributes(b->dpy, b->win, &attr);
    *w = attr.width;
    *h = attr.height;
}

void x11BackendGetScreenSize(X11Backend* b, int* w, int* h) {
    *w = DisplayWidth(b->dpy, b->screen);
    *h = DisplayHeight(b->dpy, b->screen);
}

// ── Cursor ──────────────────────────────────────────────────────────────────

void x11BackendSetCursorVisible(X11Backend* b, bool visible) {
    XDefineCursor(b->dpy, b->win, visible ? b->cursorArrow : b->cursorBlank);
}

void x11BackendDefineCursorArrow(X11Backend* b) { XDefineCursor(b->dpy, b->win, b->cursorArrow); }
void x11BackendDefineCursorHand(X11Backend* b)  { XDefineCursor(b->dpy, b->win, b->cursorHand); }
void x11BackendDefineCursorText(X11Backend* b)  { XDefineCursor(b->dpy, b->win, b->cursorText); }

static Cursor createCursorFromRGBA(Display* dpy, const unsigned char* rgba, int w, int h, int hotX, int hotY) {
    XcursorImage* img = XcursorImageCreate(w, h);
    img->xhot = hotX;
    img->yhot = hotY;
    // Convert RGBA (byte order) to XcursorPixel (ARGB native u32)
    for (int i = 0; i < w * h; i++) {
        unsigned char r = rgba[i * 4 + 0];
        unsigned char g = rgba[i * 4 + 1];
        unsigned char b = rgba[i * 4 + 2];
        unsigned char a = rgba[i * 4 + 3];
        img->pixels[i] = (static_cast<XcursorPixel>(a) << 24) | (static_cast<XcursorPixel>(r) << 16) | (static_cast<XcursorPixel>(g) << 8) | static_cast<XcursorPixel>(b);
    }
    Cursor cur = XcursorImageLoadCursor(dpy, img);
    XcursorImageDestroy(img);
    return cur;
}

void x11BackendSetCustomCursorArrow(X11Backend* b, const unsigned char* rgba, int w, int h, int hotX, int hotY) {
    Cursor newCur = createCursorFromRGBA(b->dpy, rgba, w, h, hotX, hotY);
    Cursor old = b->cursorArrow;
    b->cursorArrow = newCur;
    XDefineCursor(b->dpy, b->win, b->cursorArrow);
    if (old) XFreeCursor(b->dpy, old);
}

void x11BackendSetCustomCursorHand(X11Backend* b, const unsigned char* rgba, int w, int h, int hotX, int hotY) {
    Cursor newCur = createCursorFromRGBA(b->dpy, rgba, w, h, hotX, hotY);
    Cursor old = b->cursorHand;
    b->cursorHand = newCur;
    if (old) XFreeCursor(b->dpy, old);
}

void x11BackendWarp(X11Backend* b, int x, int y) {
    XGrabPointer(b->dpy, b->win, False,
                 PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
                 GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
    XWarpPointer(b->dpy, None, b->win, 0, 0, 0, 0, x, y);
    XUngrabPointer(b->dpy, CurrentTime);
    XFlush(b->dpy);
}

void x11BackendQueryPointer(X11Backend* b, int* x, int* y) {
    Window root, child;
    int rx, ry, wx, wy;
    unsigned int mask;
    XQueryPointer(b->dpy, b->win, &root, &child, &rx, &ry, &wx, &wy, &mask);
    *x = wx;
    *y = wy;
}

bool x11BackendHasFocus(X11Backend* b) {
    Window focused;
    int revert;
    XGetInputFocus(b->dpy, &focused, &revert);
    return focused == b->win;
}

// ── Events ──────────────────────────────────────────────────────────────────

static void processXI2RawMotion(X11Backend* b, XGenericEventCookie* cookie,
                                X11Event* out, int* count, int maxEvents) {
    if (cookie->extension != b->xi2Opcode) return;
    if (cookie->evtype != XI_RawMotion) return;
    if (*count >= maxEvents) return;

    XIRawEvent* raw  = static_cast<XIRawEvent*>(cookie->data);
    double dx = 0, dy = 0;
    int n = 0;
    for (int i = 0; i < raw->valuators.mask_len * 8 && n < 2; i++) {
        if (!XIMaskIsSet(raw->valuators.mask, i)) continue;
        if (n == 0) dx = raw->raw_values[n];
        if (n == 1) dy = raw->raw_values[n];
        n++;
    }

    X11Event* e   = &out[*count];
    e->type       = X11_EVENT_RAW_MOTION;
    e->rawMotion.dx = dx;
    e->rawMotion.dy = dy;
    (*count)++;
}

int x11BackendPollEvents(X11Backend* b, X11Event* out, int maxEvents) {
    int count = 0;

    while (XPending(b->dpy) && count < maxEvents) {
        XEvent ev;
        XNextEvent(b->dpy, &ev);

        // XInput2 generic events
        if (ev.type == GenericEvent && ev.xcookie.extension == b->xi2Opcode) {
            if (XGetEventData(b->dpy, &ev.xcookie)) {
                processXI2RawMotion(b, &ev.xcookie, out, &count, maxEvents);
                XFreeEventData(b->dpy, &ev.xcookie);
            }
            continue;
        }

        X11Event* e = &out[count];
        memset(e, 0, sizeof(*e));

        switch (ev.type) {
            case KeyPress: {
                KeySym sym = XkbKeycodeToKeysym(b->dpy, ev.xkey.keycode, 0, 0);
                e->type          = X11_EVENT_KEY_DOWN;
                e->key.keysym    = (uint32_t)sym;
                e->key.repeat    = false;

                // Text input
                if (b->xic) {
                    char buf[32] = {};
                    KeySym ks;
                    Status status;
                    int len = Xutf8LookupString(b->xic, &ev.xkey, buf, sizeof(buf) - 1, &ks, &status);
                    if (len > 0 && (unsigned char)buf[0] >= 32) {
                        count++;
                        if (count < maxEvents) {
                            X11Event* te = &out[count];
                            memset(te, 0, sizeof(*te));
                            te->type = X11_EVENT_TEXT;
                            memcpy(te->text.text, buf, sizeof(te->text.text));
                        }
                    } else {
                        count++;
                    }
                    break;
                }
                count++;
            } break;

            case KeyRelease: {
                // Detect auto-repeat
                if (XEventsQueued(b->dpy, QueuedAfterReading)) {
                    XEvent next;
                    XPeekEvent(b->dpy, &next);
                    if (next.type == KeyPress &&
                        next.xkey.time == ev.xkey.time &&
                        next.xkey.keycode == ev.xkey.keycode) {
                        XNextEvent(b->dpy, &next);
                        KeySym sym    = XkbKeycodeToKeysym(b->dpy, ev.xkey.keycode, 0, 0);
                        e->type       = X11_EVENT_KEY_DOWN;
                        e->key.keysym = (uint32_t)sym;
                        e->key.repeat = true;
                        count++;
                        break;
                    }
                }
                KeySym sym    = XkbKeycodeToKeysym(b->dpy, ev.xkey.keycode, 0, 0);
                e->type       = X11_EVENT_KEY_UP;
                e->key.keysym = (uint32_t)sym;
                count++;
            } break;

            case MotionNotify: {
                e->type     = X11_EVENT_MOTION;
                e->motion.x = static_cast<float>(ev.xmotion.x);
                e->motion.y = static_cast<float>(ev.xmotion.y);
                count++;
            } break;

            case ButtonPress: {
                unsigned int btn = ev.xbutton.button;
                if (btn >= 4 && btn <= 7) {
                    e->type = X11_EVENT_SCROLL;
                    e->scroll.x = (btn == 6) ? -1.0f : (btn == 7) ? 1.0f : 0;
                    e->scroll.y = (btn == 4) ? 1.0f  : (btn == 5) ? -1.0f : 0;
                } else {
                    e->type          = X11_EVENT_BUTTON_DOWN;
                    e->button.button = btn;
                }
                count++;
            } break;

            case ButtonRelease: {
                unsigned int btn = ev.xbutton.button;
                if (btn >= 4 && btn <= 7) break;
                e->type          = X11_EVENT_BUTTON_UP;
                e->button.button = btn;
                count++;
            } break;

            case ConfigureNotify: {
                e->type          = X11_EVENT_RESIZE;
                e->resize.width  = ev.xconfigure.width;
                e->resize.height = ev.xconfigure.height;
                count++;
            } break;

            case FocusIn: {
                e->type = X11_EVENT_FOCUS_IN;
                if (b->xic) XSetICFocus(b->xic);
                count++;
            } break;

            case FocusOut: {
                e->type = X11_EVENT_FOCUS_OUT;
                if (b->xic) XUnsetICFocus(b->xic);
                count++;
            } break;

            case ClientMessage: {
                if (static_cast<Atom>(ev.xclient.data.l[0]) == b->wmDeleteWindow) {
                    e->type = X11_EVENT_CLOSE;
                    count++;
                }
            } break;

            default:
                break;
        }
    }

    return count;
}

// ── Vulkan ──────────────────────────────────────────────────────────────────

static const char* vulkanExts[] = {
    VK_KHR_SURFACE_EXTENSION_NAME,
    "VK_KHR_xlib_surface",
};

const char** x11BackendGetVulkanExtensions(uint32_t* count) {
    *count = 2;
    return (const char**)vulkanExts;
}

bool x11BackendCreateVulkanSurface(X11Backend* b, void* vkInstance, void* vkSurface) {
    PFN_vkCreateXlibSurfaceKHR fn =
        reinterpret_cast<PFN_vkCreateXlibSurfaceKHR>(vkGetInstanceProcAddr(static_cast<VkInstance>(vkInstance), "vkCreateXlibSurfaceKHR"));
    if (!fn) return false;

    VkXlibSurfaceCreateInfoKHR createInfo = {
        .sType  = MY_VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
        .pNext  = nullptr,
        .flags  = 0,
        .dpy    = b->dpy,
        .window = b->win,
    };
    return fn(static_cast<VkInstance>(vkInstance), &createInfo, nullptr, static_cast<VkSurfaceKHR*>(vkSurface)) == VK_SUCCESS;
}

}  // namespace engine
#endif // __linux__
