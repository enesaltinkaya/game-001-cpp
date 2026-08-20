#include "ecs/Ecs.h"
#include "ecs/system/System.h"
#include "ecs/system/window/WindowSystem.h"
#include "platform/Platform.h"
#include "renderer/Renderer.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "renderer/vulkan/Vulkan.h"
#include "rmlui/wrapper/src/crmlui.h"
#include "settings/Settings.h"
#include "timer/Timer.h"

static void added(void);
static void update(void);
static void removed(void);

struct System statsGui = {
    .name    = "statsGui",
    .added   = added,
    .update  = update,
    .removed = removed,
};

static void* document;
static void* model;
static double lastUpdate;
static u64 heap;
static float totalGpuTime;
static size_t systemSize;
static size_t passSize;
static void systemInfo(int index, int type, char* out);
static void passInfo(int index, int type, char* out);
static int framesInFlight = FRAMES_IN_FLIGHT;
static char vsync;
static float fpsLimit;
static float uiScale;
static char isVulkan = 1;
static char* debugReleaseText;
static char* gpuNamePtr;
static int swapchainImageCount;
static double swapchainCpuElapsed;

void added(void) {
    debugReleaseText  = static_cast<char*>(memoryAlloc(200));
    strcpy(debugReleaseText, isDebug() ? "debug mode" : "release mode");

    ecs.showStats = 1;

    document = rmlNewDocument("gui/stats/stats.html");
    model    = rmlCreateModel("stats");

    vsync    = settingsGetBool("vsync");
    fpsLimit = settingsGetDouble("fpsLimit");
    uiScale  = settingsGetDouble("uiScale");

    rmlBind(model, "fps", &timer.fps);
    rmlBind(model, "elapsedFull", &timer.elapsedFull);
    rmlBind(model, "elapsed", &timer.elapsed);
    rmlBind(model, "ups", &timer.ups);
    rmlBind(model, "dt", &timer.dt);
    rmlBind(model, "time", &timer.timeSinceStart);
    rmlBind(model, "frame", &timer.frameCounter);
    rmlBind(model, "isVulkan", &isVulkan);
    rmlBind(model, "drawCalls", &renderer.drawCalls);
    rmlBind(model, "instanceCount", &renderer.instanceCount);
    rmlBind(model, "triangleCount", &renderer.triangleCount);
    rmlBind(model, "vsync", &vsync);
    rmlBind(model, "fpsLimit", &fpsLimit);
    rmlBind(model, "uiScale", &uiScale);
    rmlBind(model, "windowScale", &window.xscale);
    rmlBind(model, "width", &window.width);
    rmlBind(model, "height", &window.height);
    rmlBind(model, "heap", &heap);
    gpuNamePtr = vulkan.deviceProperties.deviceName;
    rmlBind(model, "gpuName", &gpuNamePtr);
    rmlBind(model, "totalGpuTime", &totalGpuTime);
    rmlBind(model, "framesInFlight", &framesInFlight);
    swapchainImageCount = rendererGetSwapchainImageCount();
    rmlBind(model, "swapchainImages", &swapchainImageCount);
    rmlBind(model, "rendererCpu", &renderer.rendererElapsedCpu);
    rmlBind(model, "overallRendererGpu", &renderer.rendererElapsedGpu);
    rmlBind(model, "ecsCpu", &ecs.totalCpuElapsed);
    swapchainCpuElapsed = rendererGetSwapchainCpuElapsed();
    rmlBind(model, "swapchainElapsed", &swapchainCpuElapsed);
    rmlBind(model, "debugReleaseText", &debugReleaseText);

    static char first = 1;
    if (first) {
        first = 0;
        rmlRegisterTransformFunc(model, "systemInfo", systemInfo);
        rmlRegisterTransformFunc(model, "passInfo", passInfo);
    }
    rmlBindArray(model, "passes", &passSize);
    rmlBindArray(model, "systems", &systemSize);

    rmlLoadDocument(document);
    rmlShowDocumentWithoutFocus(document);
}

void removed(void) {
    ecs.showStats = 0;
    memoryFree(debugReleaseText);
    rmlUnloadDocument(document);
    rmlUnloadModel(model);
    document = NULL;  // used for toggling
}

void update(void) {
    double now = nanos();
    if (now > lastUpdate + BILLION / 2.) {  // twice per second
        totalGpuTime = 0;
        for (size_t i = 0; i < arraySize(renderer.passes); i++) {
            totalGpuTime += renderer.passes[i]->gpuElapsed / MILLION;
        }

        heap       = memoryUsage() / 1024;
        lastUpdate = now;
        systemSize = arraySize(ecs.systems);
        passSize   = arraySize(renderer.passes);
        vsync      = settingsGetBool("vsync");
        fpsLimit   = settingsGetDouble("fpsLimit");
        uiScale    = settingsGetDouble("uiScale");

        swapchainImageCount = rendererGetSwapchainImageCount();
        swapchainCpuElapsed = rendererGetSwapchainCpuElapsed();
        rmlUpdateDirtyAll(model);
    }
}

void systemInfo(int index, int type, char* out) {
    if (type == 0) {
        sprintf(out, "%s", ecs.systems[index]->name);
    }

    if (type == 1) {
        sprintf(out, "%.2f", ecs.systems[index]->cpuElapsed / MILLION);
    }
}

void passInfo(int index, int type, char* out) {
    if (type == 0) {
        sprintf(out, "%s", renderer.passes[index]->name);
    }

    if (type == 1) {
        struct System* pass = renderer.passes[index];
        sprintf(out, "%.2f", pass->cpuElapsed / MILLION);
    }

    if (type == 2) {
        sprintf(out, "%.2f", renderer.passes[index]->gpuElapsed / MILLION);
    }
}

void statsGuiToggle(void) {
    if (document) {
        guiManagerRemoveGuiNextFrame(&statsGui);
    } else {
        guiManagerAddGuiNextFrame(&statsGui);
    }
}
