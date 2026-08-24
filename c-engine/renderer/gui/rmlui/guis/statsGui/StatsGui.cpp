#include "StatsGui.h"
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

namespace engine {

StatsGui statsGui;

StatsGui::StatsGui() : System("statsGui") {}

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
static char debugReleaseText[200];
static char* gpuNamePtr;
static int swapchainImageCount;
static double swapchainCpuElapsed;

void StatsGui::added() {
    strcpy(debugReleaseText, utils::isDebug() ? "debug mode" : "release mode");

    ecs.showStats = 1;

    document = rmlNewDocument("gui/stats/stats.html");
    model    = rmlCreateModel("stats");

    vsync    = utils::settingsGetBool("vsync");
    fpsLimit = utils::settingsGetDouble("fpsLimit");
    uiScale  = utils::settingsGetDouble("uiScale");

    rmlBind(model, "fps", &utils::timer.fps);
    rmlBind(model, "elapsedFull", &utils::timer.elapsedFull);
    rmlBind(model, "elapsed", &utils::timer.elapsed);
    rmlBind(model, "ups", &utils::timer.ups);
    rmlBind(model, "dt", &utils::timer.dt);
    rmlBind(model, "time", &utils::timer.timeSinceStart);
    rmlBind(model, "frame", &utils::timer.frameCounter);
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
    rmlBind(model, "debugReleaseText", &debugReleaseText[0]);

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

void StatsGui::removed() {
    ecs.showStats = 0;
    rmlUnloadDocument(document);
    rmlUnloadModel(model);
    document = nullptr;  // used for toggling
}

void StatsGui::update() {
    double now = utils::nanos();
    if (now > lastUpdate + BILLION / 2.) {  // twice per second
        totalGpuTime = 0;
        for (size_t i = 0; i < renderer.passes.size(); i++) {
            totalGpuTime += renderer.passes[i]->gpuElapsed / MILLION;
        }

        heap       = utils::memoryUsage() / 1024;
        lastUpdate = now;
        systemSize = static_cast<i32>(ecs.systems.size());
        passSize   = static_cast<i32>(renderer.passes.size());
        vsync      = utils::settingsGetBool("vsync");
        fpsLimit   = utils::settingsGetDouble("fpsLimit");
        uiScale    = utils::settingsGetDouble("uiScale");

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
        System* pass = renderer.passes[index];
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
}  // namespace engine
