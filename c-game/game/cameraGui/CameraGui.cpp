#include "CameraGui.h"
#include "Utils.h"
#include "ecs/Ecs.h"
#include "ecs/system/System.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/transform/TransformComponent.h"
#include "ecs/system/transform/TransformSystem.h"
#include "rmlui/wrapper/src/crmlui.h"

namespace game {

CameraGui cameraGui;

CameraGui::CameraGui() : engine::System("cameraGui") {}

static void* document;
static void* model;
static float posX, posY, posZ;
static float dirX, dirY, dirZ;
static float rotX, rotY, rotZ, rotW;

void CameraGui::added() {
    document = rmlNewDocument("gui/camera/camera.html");
    model    = rmlCreateModel("camera");
    rmlBindFloat(model, "posX", &posX);
    rmlBindFloat(model, "posY", &posY);
    rmlBindFloat(model, "posZ", &posZ);
    rmlBindFloat(model, "dirX", &dirX);
    rmlBindFloat(model, "dirY", &dirY);
    rmlBindFloat(model, "dirZ", &dirZ);
    rmlBindFloat(model, "rotX", &rotX);
    rmlBindFloat(model, "rotY", &rotY);
    rmlBindFloat(model, "rotZ", &rotZ);
    rmlBindFloat(model, "rotW", &rotW);

    rmlLoadDocument(document);
    rmlShowDocument(document);
}

void CameraGui::removed() {
    rmlUnloadDocument(document);
    rmlUnloadModel(model);
}

void CameraGui::update() {
    static double lastShown;
    double now = utils::millies();
    if (now > lastShown + 50) {  // 50ms

        engine::Entity* camEntity         = engine::cameraGetEntity();
        engine::WorldTransform* transform = engine::transformGetWorld(camEntity->scene, camEntity->id);

        posX = transform->pos[0];
        posY = transform->pos[1];
        posZ = transform->pos[2];
        rotX = transform->rot[0];
        rotY = transform->rot[1];
        rotZ = transform->rot[2];
        rotW = transform->rot[3];

        // vec3s dir = transformGetDirection(cameraEntity);
        // dirX      = dir.x;
        // dirY      = dir.y;
        // dirZ      = dir.z;

        // char* curr = terrainSystemGetCurrentNode();
        // strcpy(currentMapNode, curr);

        lastShown = now;
        rmlUpdateDirtyAll(model);
    }
}
}  // namespace game
