#include "ecs/system/System.h"
#include "renderer/Renderer.h"
#include "rmlui/wrapper/src/crmlui.h"

static void added(void);
static void update(void);
static void removed(void);

struct System rmluiShowFpsGui = {
    .name    = "showFpsGui",
    .added   = added,
    .update  = update,
    .removed = removed,
};

static void* document;
static void* model;

void added(void) {
    document = rmlNewDocument("gui/showFps/showFps.html");
    model    = rmlCreateModel("showFps");
    rmlBindDouble(model, "fps", &timer.fps);
    rmlBindDouble(model, "elapsedCpu", &timer.elapsed);
    rmlBindDouble(model, "elapsedGpu", &renderer.rendererElapsedGpu);
    rmlBindDouble(model, "elapsedCpuTotal", &timer.elapsedFull);

    rmlLoadDocument(document);
    rmlShowDocumentWithoutFocus(document);
}

void removed(void) {
    rmlUnloadModel(model);
    rmlUnloadDocument(document);
}

void update(void) {
    static double lastShown;
    double now = nanos();
    if (now > lastShown + BILLION / 2.) {  // twice per second
        lastShown = now;
        rmlUpdateDirtyAll(model);
    }
}
