#include "Utils.h"
#include "ecs/system/System.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/transform/TransformComponent.h"
#include "ecs/system/transform/TransformSystem.h"
#include "ecs/system/window/WindowSystem.h"
#include "rmlui/wrapper/src/crmlui.h"

static void added(void);
static void update(void);
static void removed(void);

struct System latencyCircleGui = {
    .name    = "latencyCircleGui",
    .added   = added,
    .update  = update,
    .removed = removed,
};

static void* document;
static void* model;
static float posX, posY;

static void added(void) {
    document = rmlNewDocument("gui/latencyCircle/latencyCircle.html");
    model    = rmlCreateModel("latencyCircle");
    rmlBindFloat(model, "posX", &posX);
    rmlBindFloat(model, "posY", &posY);

    rmlLoadDocument(document);
    rmlShowDocument(document);
}

static void removed(void) {
    rmlUnloadDocument(document);
    rmlUnloadModel(model);
}

static void update(void) {
    // static double lastShown;
    // double now = millies();
    posX = input.xpos;
    posY = input.ypos;
    rmlUpdateDirtyAll(model);

    // if (now > lastShown + 50) {  // 50ms
    //     lastShown = now;
    // }
}
