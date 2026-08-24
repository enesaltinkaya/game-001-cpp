#include "ShowFpsGui.h"
#include "ecs/system/System.h"
#include "renderer/Renderer.h"
#include "rmlui/wrapper/src/crmlui.h"

namespace engine {

RmluiShowFpsGui rmluiShowFpsGui;

RmluiShowFpsGui::RmluiShowFpsGui() : System("showFpsGui") {}

static void* document;
static void* model;

void RmluiShowFpsGui::added() {
    document = rmlNewDocument("gui/showFps/showFps.html");
    model    = rmlCreateModel("showFps");
    rmlBindDouble(model, "fps", &utils::timer.fps);
    rmlBindDouble(model, "elapsedCpu", &utils::timer.elapsed);
    rmlBindDouble(model, "elapsedGpu", &renderer.rendererElapsedGpu);
    rmlBindDouble(model, "elapsedCpuTotal", &utils::timer.elapsedFull);

    rmlLoadDocument(document);
    rmlShowDocumentWithoutFocus(document);
}

void RmluiShowFpsGui::removed() {
    rmlUnloadModel(model);
    rmlUnloadDocument(document);
}

void RmluiShowFpsGui::update() {
    static double lastShown;
    double now = utils::nanos();
    if (now > lastShown + BILLION / 2.) {  // twice per second
        lastShown = now;
        rmlUpdateDirtyAll(model);
    }
}
}  // namespace engine
