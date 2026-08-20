#include "LatencyCircleGui.h"
#include "Utils.h"
#include "ecs/system/System.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/transform/TransformComponent.h"
#include "ecs/system/transform/TransformSystem.h"
#include "ecs/system/window/WindowSystem.h"
#include "rmlui/wrapper/src/crmlui.h"

namespace game {

LatencyCircleGui latencyCircleGui;

LatencyCircleGui::LatencyCircleGui() : engine::System("latencyCircleGui") {}

static void* document;
static void* model;
static float posX, posY;

void LatencyCircleGui::added() {
    document = rmlNewDocument("gui/latencyCircle/latencyCircle.html");
    model    = rmlCreateModel("latencyCircle");
    rmlBindFloat(model, "posX", &posX);
    rmlBindFloat(model, "posY", &posY);

    rmlLoadDocument(document);
    rmlShowDocument(document);
}

void LatencyCircleGui::removed() {
    rmlUnloadDocument(document);
    rmlUnloadModel(model);
}

void LatencyCircleGui::update() {
    // static double lastShown;
    // double now = millies();
    posX = engine::input.xpos;
    posY = engine::input.ypos;
    rmlUpdateDirtyAll(model);

    // if (now > lastShown + 50) {  // 50ms
    //     lastShown = now;
    // }
}
}  // namespace game
