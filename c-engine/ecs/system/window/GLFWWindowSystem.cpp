#include "ecs/system/window/WindowSystem.h"
#include "Engine.h"
#include "GLFW/glfw3.h"
#include "Utils.h"
#include "image/Image.h"

namespace engine {
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

static void initGLFW(void);
static void setDimensions(void);
static void createWindow(void);
static void centerWindow(void);
static void loadCursors(void);
static GLFWcursor* loadCursor(const char* path, float xHot, float yHot);
static KeyCode windowSystemMapGLFWKey(int key);
static MouseButton windowSystemMapGLFWMouseButton(int button);
static void windowSystemPushInputEvent(InputEvent event);

static GLFWcursor *cursorArrow, *cursorHand, *cursorText;
static const char* title  = "Mini";
static bool cursorVisible = true;
static double cursorSaveX, cursorSaveY;
static float pendingRelativeMouseDx, pendingRelativeMouseDy;
static int windowedX, windowedY, windowedWidth, windowedHeight;

static void glfwWindowSystemAdded(void);
static void glfwWindowSystemRemoved(void);
static void glfwWindowSystemPreUpdate(void);
static void glfwWindowSystemPostUpdate(void);
void glfwWindowSystemHide(void);
void glfwWindowSystemShow(void);
void glfwWindowSystemToggleFullscreen(char fullScreen);
void glfwWindowSystemReloadCursors(void);
void* glfwWindowSystemGetPointerCursor(void);
void* glfwWindowSystemGetArrowCursor(void);
void* glfwWindowSystemGetTextCursor(void);
void glfwWindowSystemUpdateDimensions(void);
void glfwWindowSystemWarpCenter(void);
void glfwWindowSystemWarp(float x, float y);
void glfwWindowSystemHideCursor(void);
void glfwWindowSystemShowCursor(void);
bool glfwWindowSystemIsCursorVisible(void);
void glfwWindowSystemGetRelativeMouseDelta(float* dx, float* dy);
bool glfwWindowSystemIsLeftMouseDown(void);
bool glfwWindowSystemIsRightMouseDown(void);
bool glfwWindowSystemIsMiddleMouseDown(void);
char const* const* glfwWindowSystemGetRequiredVulkanExtensions(u32* extensionCount);
bool glfwWindowSystemCreateVulkanSurface(VkInstance instance, VkSurfaceKHR* surface);
void glfwInputReset(void);
char glfwInputShouldProcess(void);

WindowBackendApi glfwWindowBackendApi = {
    .name                        = "glfw",
    .added                       = glfwWindowSystemAdded,
    .removed                     = glfwWindowSystemRemoved,
    .preUpdate                   = glfwWindowSystemPreUpdate,
    .postUpdate                  = glfwWindowSystemPostUpdate,
    .hide                        = glfwWindowSystemHide,
    .show                        = glfwWindowSystemShow,
    .toggleFullscreen            = glfwWindowSystemToggleFullscreen,
    .reloadCursors               = glfwWindowSystemReloadCursors,
    .getPointerCursor            = glfwWindowSystemGetPointerCursor,
    .getArrowCursor              = glfwWindowSystemGetArrowCursor,
    .getTextCursor               = glfwWindowSystemGetTextCursor,
    .updateDimensions            = glfwWindowSystemUpdateDimensions,
    .warpCenter                  = glfwWindowSystemWarpCenter,
    .warp                        = glfwWindowSystemWarp,
    .hideCursor                  = glfwWindowSystemHideCursor,
    .showCursor                  = glfwWindowSystemShowCursor,
    .isCursorVisible             = glfwWindowSystemIsCursorVisible,
    .getRelativeMouseDelta       = glfwWindowSystemGetRelativeMouseDelta,
    .isLeftMouseDown             = glfwWindowSystemIsLeftMouseDown,
    .isRightMouseDown            = glfwWindowSystemIsRightMouseDown,
    .isMiddleMouseDown           = glfwWindowSystemIsMiddleMouseDown,
    .getRequiredVulkanExtensions = glfwWindowSystemGetRequiredVulkanExtensions,
    .createVulkanSurface         = glfwWindowSystemCreateVulkanSurface,
};

static void glfwWindowSizeCallback(GLFWwindow* glfwWindow, int width, int height) {
    (void)glfwWindow;
    window.width  = width;
    window.height = height;
    window.ratio  = static_cast<float>(width) / static_cast<float>(height);

    InputEvent event         = {};
    event.type               = INPUT_EVENT_WINDOW_RESIZED;
    event.data.resize.width  = width;
    event.data.resize.height = height;
    windowSystemPushInputEvent(event);
    utils::signalEmit("windowResized", nullptr);
}

static void glfwKeyCallback(GLFWwindow* glfwWindow, int key, int scancode, int action, int mods) {
    (void)glfwWindow;
    KeyCode mappedKey = windowSystemMapGLFWKey(key);

    input.scancode = scancode;
    input.mods     = mods;
    input.action   = action;
    input.key      = mappedKey;
    input.ctrl     = (mods & GLFW_MOD_CONTROL) != 0;
    input.shift    = (mods & GLFW_MOD_SHIFT) != 0;
    input.alt      = (mods & GLFW_MOD_ALT) != 0;

    if (mappedKey > KEY_NONE && mappedKey < KEY_COUNT) {
        if (action == GLFW_PRESS || action == GLFW_REPEAT) {
            input.repeating[mappedKey] = 1;
            if (action == GLFW_PRESS) {
                input.pressed = mappedKey;
            }
        } else if (action == GLFW_RELEASE) {
            input.repeating[mappedKey] = 0;
            input.released             = mappedKey;
        }
    }

    InputEvent event      = {};
    event.ctrl            = input.ctrl;
    event.shift           = input.shift;
    event.alt             = input.alt;
    event.data.key.key    = mappedKey;
    event.data.key.repeat = (action == GLFW_REPEAT);
    event.type            = (action == GLFW_RELEASE) ? INPUT_EVENT_KEY_UP : INPUT_EVENT_KEY_DOWN;
    windowSystemPushInputEvent(event);
}

static void glfwCharCallback(GLFWwindow* glfwWindow, unsigned int codepoint) {
    (void)glfwWindow;
    input.character = codepoint;

    InputEvent event = {};
    event.type       = INPUT_EVENT_TEXT_INPUT;
    if (codepoint < 0x80) {
        event.data.text.text[0] = static_cast<char>(codepoint);
        event.data.text.text[1] = '\0';
    }
    windowSystemPushInputEvent(event);
}

static void glfwCursorPosCallback(GLFWwindow* glfwWindow, double xpos, double ypos) {
    (void)glfwWindow;
    input.lastX  = input.xpos;
    input.lastY  = input.ypos;
    input.xpos   = static_cast<float>(xpos);
    input.ypos   = static_cast<float>(ypos);
    input.deltaX = input.xpos - input.lastX;
    input.deltaY = input.ypos - input.lastY;
    pendingRelativeMouseDx += input.deltaX;
    pendingRelativeMouseDy += input.deltaY;

    InputEvent event     = {};
    event.type           = INPUT_EVENT_MOUSE_MOVE;
    event.ctrl           = input.ctrl;
    event.shift          = input.shift;
    event.alt            = input.alt;
    event.data.motion.x  = input.xpos;
    event.data.motion.y  = input.ypos;
    event.data.motion.dx = input.deltaX;
    event.data.motion.dy = input.deltaY;
    windowSystemPushInputEvent(event);
}

static void glfwMouseButtonCallback(GLFWwindow* glfwWindow, int button, int action, int mods) {
    (void)glfwWindow;
    input.mouseButton = button;
    input.mouseAction = action;
    input.mouseMods   = mods;
    input.ctrl        = (mods & GLFW_MOD_CONTROL) != 0;
    input.shift       = (mods & GLFW_MOD_SHIFT) != 0;
    input.alt         = (mods & GLFW_MOD_ALT) != 0;

    InputEvent event              = {};
    event.ctrl                    = input.ctrl;
    event.shift                   = input.shift;
    event.alt                     = input.alt;
    event.data.mouseButton.button = windowSystemMapGLFWMouseButton(button);
    event.type                    = (action == GLFW_RELEASE) ? INPUT_EVENT_MOUSE_BUTTON_UP
                                                             : INPUT_EVENT_MOUSE_BUTTON_DOWN;
    windowSystemPushInputEvent(event);
}

static void glfwScrollCallback(GLFWwindow* glfwWindow, double xoffset, double yoffset) {
    (void)glfwWindow;
    input.scrollX = static_cast<float>(xoffset);
    input.scrollY = static_cast<float>(yoffset);

    InputEvent event   = {};
    event.type         = INPUT_EVENT_MOUSE_WHEEL;
    event.ctrl         = input.ctrl;
    event.shift        = input.shift;
    event.alt          = input.alt;
    event.data.wheel.x = input.scrollX;
    event.data.wheel.y = input.scrollY;
    windowSystemPushInputEvent(event);
}

static void glfwWindowFocusCallback(GLFWwindow* glfwWindow, int focused) {
    (void)glfwWindow;
    input.focused = focused != 0;
}

static void glfwWindowCloseCallback(GLFWwindow* glfwWindow) {
    (void)glfwWindow;
    InputEvent event         = {};
    event.type               = INPUT_EVENT_QUIT;
    windowSystemPushInputEvent(event);
    engineStop();
}

void glfwWindowSystemAdded(void) {
    initGLFW();
    setDimensions();
    createWindow();
    loadCursors();
    utils::info("windowSystem: initialized GLFW backend");
    utils::debug("windowSystem: dimensions    %dx%d", window.width, window.height);
}

static void windowRemovedDelayed(void* _) {
    (void)_;
    if (cursorArrow) glfwDestroyCursor(cursorArrow);
    if (cursorHand) glfwDestroyCursor(cursorHand);
    if (cursorText) glfwDestroyCursor(cursorText);
    if (window.glfwWindowHandle) glfwDestroyWindow(window.glfwWindowHandle);
    glfwTerminate();
}

void glfwWindowSystemRemoved(void) {
    utils::futureTaskAdd(0, windowRemovedDelayed, nullptr);
}

void initGLFW(void) {
    if (!glfwInit()) {
        utils::terminate("windowSystem: failed to initialize GLFW");
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    if (utils::settingsGetBool("fullScreen")) {
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    }
}

void setDimensions(void) {
    GLFWmonitor* primaryMonitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode     = glfwGetVideoMode(primaryMonitor);

    if (utils::settingsGetBool("fullScreen")) {
        window.width  = mode->width;
        window.height = mode->height;
    } else {
        window.width  = mode->width * 0.75F;
        window.height = window.width / 1.77F;
    }

    window.ratio = static_cast<float>(window.width) / static_cast<float>(window.height);
}

void createWindow(void) {
    double elapsed          = utils::elapsedBegin();
    GLFWmonitor* monitor    = utils::settingsGetBool("fullScreen") ? glfwGetPrimaryMonitor() : nullptr;
    window.glfwWindowHandle = glfwCreateWindow(window.width, window.height, title, monitor, nullptr);
    elapsed                 = utils::elapsedEnd(elapsed);
    if (!window.glfwWindowHandle) {
        utils::terminate("windowSystem: failed to create GLFW window");
    }
    utils::info("windowSystem: initialized in %.02f ms", elapsed);

    glfwSetWindowSizeCallback(window.glfwWindowHandle, glfwWindowSizeCallback);
    glfwSetKeyCallback(window.glfwWindowHandle, glfwKeyCallback);
    glfwSetCharCallback(window.glfwWindowHandle, glfwCharCallback);
    glfwSetCursorPosCallback(window.glfwWindowHandle, glfwCursorPosCallback);
    glfwSetMouseButtonCallback(window.glfwWindowHandle, glfwMouseButtonCallback);
    glfwSetScrollCallback(window.glfwWindowHandle, glfwScrollCallback);
    glfwSetWindowFocusCallback(window.glfwWindowHandle, glfwWindowFocusCallback);
    glfwSetWindowCloseCallback(window.glfwWindowHandle, glfwWindowCloseCallback);

    centerWindow();
    glfwGetWindowPos(window.glfwWindowHandle, &windowedX, &windowedY);
    glfwGetWindowSize(window.glfwWindowHandle, &windowedWidth, &windowedHeight);

    float xscale = 1.0f;
    float yscale = 1.0f;
    glfwGetWindowContentScale(window.glfwWindowHandle, &xscale, &yscale);
    window.xscale = xscale;
    window.yscale = yscale;

    if (utils::settingsGetDouble("uiScale") == 0) {
        utils::settingsSetDouble("uiScale", window.xscale);
        utils::settingsSetDouble("cursorScale", window.xscale);
        utils::settingsWrite();
    }
}

void centerWindow(void) {
    if (utils::settingsGetBool("fullScreen")) return;

    GLFWmonitor* monitor    = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = glfwGetVideoMode(monitor);
    int monitorX            = 0;
    int monitorY            = 0;
    glfwGetMonitorPos(monitor, &monitorX, &monitorY);
    glfwSetWindowPos(window.glfwWindowHandle,
                     monitorX + (mode->width - window.width) / 2,
                     monitorY + (mode->height - window.height) / 2);
}

GLFWcursor* loadCursor(const char* path, float xHot, float yHot) {
    utils::Image image = utils::imageLoadKtx(path, utils::KTX_FORMAT_RGBA32);

    u64 resizedWidth  = static_cast<int>(image.width / 2.5F * utils::settingsGetDouble("cursorScale"));
    u64 resizedHeight = static_cast<int>(image.height / 2.5F * utils::settingsGetDouble("cursorScale"));
    u64 resizedHotX   = static_cast<int>(xHot / 2.5F * utils::settingsGetDouble("cursorScale"));
    u64 resizedHotY   = static_cast<int>(yHot / 2.5F * utils::settingsGetDouble("cursorScale"));

    utils::Image resizedImage  = utils::imageResize(&image, resizedWidth, resizedHeight);
    GLFWimage glfwImage = {
        .width  = static_cast<int>(resizedWidth),
        .height = static_cast<int>(resizedHeight),
        .pixels = reinterpret_cast<unsigned char*>(resizedImage.data),
    };
    GLFWcursor* cursor = glfwCreateCursor(&glfwImage, static_cast<int>(resizedHotX), static_cast<int>(resizedHotY));

    utils::imageDestory(&image);
    utils::imageDestory(&resizedImage);
    return cursor;
}

void loadCursors(void) {
    if (cursorArrow) glfwDestroyCursor(cursorArrow);
    if (cursorHand) glfwDestroyCursor(cursorHand);
    if (cursorText) glfwDestroyCursor(cursorText);

    cursorArrow = loadCursor("images/cursorArrow.png.ktx2", 8, 8);
    cursorHand  = loadCursor("images/cursorHand.png.ktx2", 25, 4);
    cursorText  = glfwCreateStandardCursor(GLFW_IBEAM_CURSOR);
    glfwSetCursor(window.glfwWindowHandle, cursorArrow);
}

void* glfwWindowSystemGetPointerCursor(void) {
    return cursorHand;
}

void* glfwWindowSystemGetArrowCursor(void) {
    return cursorArrow;
}

void* glfwWindowSystemGetTextCursor(void) {
    return cursorText;
}

void glfwWindowSystemHide(void) {
    glfwHideWindow(window.glfwWindowHandle);
}

void glfwWindowSystemShow(void) {
    glfwShowWindow(window.glfwWindowHandle);
}

void glfwWindowSystemReloadCursors(void) {
    loadCursors();
}

void glfwWindowSystemToggleFullscreen(char fullScreen) {
    GLFWmonitor* monitor    = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = glfwGetVideoMode(monitor);
    if (fullScreen) {
        glfwGetWindowPos(window.glfwWindowHandle, &windowedX, &windowedY);
        glfwGetWindowSize(window.glfwWindowHandle, &windowedWidth, &windowedHeight);
        glfwSetWindowMonitor(window.glfwWindowHandle,
                             monitor,
                             0,
                             0,
                             mode->width,
                             mode->height,
                             mode->refreshRate);
    } else {
        int width  = windowedWidth > 0 ? windowedWidth : static_cast<int>(mode->width * 0.75F);
        int height = windowedHeight > 0 ? windowedHeight : static_cast<int>(width / 1.77F);
        int x      = windowedX;
        int y      = windowedY;
        if (width <= 0 || height <= 0) {
            width  = mode->width * 0.75F;
            height = width / 1.77F;
        }
        if (x == 0 && y == 0) {
            int monitorX = 0;
            int monitorY = 0;
            glfwGetMonitorPos(monitor, &monitorX, &monitorY);
            x = monitorX + (mode->width - width) / 2;
            y = monitorY + (mode->height - height) / 2;
        }
        glfwSetWindowMonitor(window.glfwWindowHandle, nullptr, x, y, width, height, 0);
    }
}

void glfwWindowSystemUpdateDimensions(void) {
    glfwGetWindowSize(window.glfwWindowHandle, &window.width, &window.height);
}

void glfwWindowSystemWarpCenter(void) {
    glfwSetCursorPos(window.glfwWindowHandle, window.width / 2.0, window.height / 2.0);
}

void glfwWindowSystemWarp(float x, float y) {
    glfwSetCursorPos(window.glfwWindowHandle, x, y);
}

void glfwWindowSystemHideCursor(void) {
    cursorVisible = false;
    glfwGetCursorPos(window.glfwWindowHandle, &cursorSaveX, &cursorSaveY);
    pendingRelativeMouseDx = 0.0f;
    pendingRelativeMouseDy = 0.0f;
    input.deltaX           = 0.0f;
    input.deltaY           = 0.0f;

    if (glfwRawMouseMotionSupported()) {
        glfwSetInputMode(window.glfwWindowHandle, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    }
    glfwSetInputMode(window.glfwWindowHandle, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
}

void glfwWindowSystemShowCursor(void) {
    cursorVisible = true;
    glfwSetInputMode(window.glfwWindowHandle, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    if (glfwRawMouseMotionSupported()) {
        glfwSetInputMode(window.glfwWindowHandle, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
    }
    pendingRelativeMouseDx = 0.0f;
    pendingRelativeMouseDy = 0.0f;
    input.deltaX           = 0.0f;
    input.deltaY           = 0.0f;
    glfwSetCursorPos(window.glfwWindowHandle, cursorSaveX, cursorSaveY);
    // futureTaskAdd(1000, showCursorDelayed, nullptr);
}

bool glfwWindowSystemIsCursorVisible(void) {
    return cursorVisible;
}

static KeyCode windowSystemMapGLFWKey(int key) {
    switch (key) {
        case GLFW_KEY_A:
            return KEY_A;
        case GLFW_KEY_B:
            return KEY_B;
        case GLFW_KEY_C:
            return KEY_C;
        case GLFW_KEY_D:
            return KEY_D;
        case GLFW_KEY_E:
            return KEY_E;
        case GLFW_KEY_F:
            return KEY_F;
        case GLFW_KEY_M:
            return KEY_M;
        case GLFW_KEY_P:
            return KEY_P;
        case GLFW_KEY_R:
            return KEY_R;
        case GLFW_KEY_S:
            return KEY_S;
        case GLFW_KEY_T:
            return KEY_T;
        case GLFW_KEY_W:
            return KEY_W;
        case GLFW_KEY_X:
            return KEY_X;
        case GLFW_KEY_1:
            return KEY_1;
        case GLFW_KEY_2:
            return KEY_2;
        case GLFW_KEY_5:
            return KEY_5;
        case GLFW_KEY_ESCAPE:
            return KEY_ESCAPE;
        case GLFW_KEY_SPACE:
            return KEY_SPACE;
        case GLFW_KEY_ENTER:
            return KEY_RETURN;
        case GLFW_KEY_KP_ENTER:
            return KEY_KP_ENTER;
        case GLFW_KEY_KP_ADD:
            return KEY_KP_PLUS;
        case GLFW_KEY_KP_SUBTRACT:
            return KEY_KP_MINUS;
        case GLFW_KEY_F8:
            return KEY_F8;
        case GLFW_KEY_LEFT_ALT:
            return KEY_LALT;
        case GLFW_KEY_RIGHT_ALT:
            return KEY_RALT;
        case GLFW_KEY_LEFT_SHIFT:
            return KEY_LSHIFT;
        case GLFW_KEY_RIGHT_SHIFT:
            return KEY_RSHIFT;
        case GLFW_KEY_LEFT_CONTROL:
            return KEY_LCTRL;
        case GLFW_KEY_RIGHT_CONTROL:
            return KEY_RCTRL;
        case GLFW_KEY_TAB:
            return KEY_TAB;
        case GLFW_KEY_H:
            return KEY_H;
        case GLFW_KEY_N:
            return KEY_N;
        case GLFW_KEY_F1:
            return KEY_F1;
        case GLFW_KEY_F2:
            return KEY_F2;
        case GLFW_KEY_F3:
            return KEY_F3;
        case GLFW_KEY_BACKSPACE:
            return KEY_BACKSPACE;
        case GLFW_KEY_DELETE:
            return KEY_DELETE;
        default:
            return KEY_NONE;
    }
}

static MouseButton windowSystemMapGLFWMouseButton(int button) {
    switch (button) {
        case GLFW_MOUSE_BUTTON_LEFT:
            return MOUSE_BUTTON_LEFT;
        case GLFW_MOUSE_BUTTON_RIGHT:
            return MOUSE_BUTTON_RIGHT;
        case GLFW_MOUSE_BUTTON_MIDDLE:
            return MOUSE_BUTTON_MIDDLE;
        case GLFW_MOUSE_BUTTON_4:
            return MOUSE_BUTTON_X1;
        case GLFW_MOUSE_BUTTON_5:
            return MOUSE_BUTTON_X2;
        default:
            return MOUSE_BUTTON_NONE;
    }
}

static void windowSystemPushInputEvent(InputEvent event) {
    input.events.push_back(event);
}

void glfwWindowSystemGetRelativeMouseDelta(float* dx, float* dy) {
    if (dx) *dx = pendingRelativeMouseDx;
    if (dy) *dy = pendingRelativeMouseDy;
    pendingRelativeMouseDx = 0.0f;
    pendingRelativeMouseDy = 0.0f;
}

bool glfwWindowSystemIsLeftMouseDown(void) {
    return glfwGetMouseButton(window.glfwWindowHandle, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
}

bool glfwWindowSystemIsRightMouseDown(void) {
    return glfwGetMouseButton(window.glfwWindowHandle, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
}

bool glfwWindowSystemIsMiddleMouseDown(void) {
    return glfwGetMouseButton(window.glfwWindowHandle, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
}

char const* const* glfwWindowSystemGetRequiredVulkanExtensions(u32* extensionCount) {
    return glfwGetRequiredInstanceExtensions(extensionCount);
}

bool glfwWindowSystemCreateVulkanSurface(VkInstance instance, VkSurfaceKHR* surface) {
    return glfwCreateWindowSurface(instance, window.glfwWindowHandle, nullptr, surface) == VK_SUCCESS;
}

void glfwWindowSystemPreUpdate(void) {
    glfwInputReset();
    input.events.clear();
    input.focused = glfwGetWindowAttrib(window.glfwWindowHandle, GLFW_FOCUSED) != 0;

    glfwPollEvents();

    if (input.alt && (input.pressed == KEY_RETURN || input.pressed == KEY_KP_ENTER)) {
        input.skip      = 1;
        char fullScreen = utils::settingsGetBool("fullScreen");
        utils::settingsSetBool("fullScreen", !fullScreen);
        utils::settingsWrite();
        glfwWindowSystemToggleFullscreen(!fullScreen);
    }

    if (input.alt && input.pressed == KEY_E) {
        engineStop();
    }

    if (glfwWindowShouldClose(window.glfwWindowHandle)) {
        engineStop();
    }
}

void glfwWindowSystemPostUpdate(void) {}

void glfwInputReset(void) {
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

char glfwInputShouldProcess(void) {
    return input.focused && !input.skip;
}
}  // namespace engine
