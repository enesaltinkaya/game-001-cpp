#include "PlayerGui.h"
#include "Utils.h"
#include "azgaar/AzgaarWorld.h"
#include "loadingAzgaar/LoadingAzgaar.h"
#include "ecs/system/System.h"
#include "player/Player.h"
#include "rmlui/wrapper/src/crmlui.h"

namespace game {

PlayerGui playerGui;

PlayerGui::PlayerGui() : engine::System("playerGui") {}

static void* document;
static void* model;
static float posX, posY, posZ;
static char* cellText;
static char cellTextBuf[32];

static void worldToMap(const AzgaarWorld* world, float wx, float wz, float* outMapX, float* outMapY) {
    *outMapX = ((-wx) / static_cast<float>(world->metersPerPixel)) + static_cast<float>(world->widthPx) * 0.5f;
    *outMapY = ((-wz) / static_cast<float>(world->metersPerPixel)) + static_cast<float>(world->heightPx) * 0.5f;
}

void PlayerGui::added() {
    document = rmlNewDocument("gui/player/player.html");
    model    = rmlCreateModel("player");
    cellText = cellTextBuf;

    rmlBindFloat(model, "posX", &posX);
    rmlBindFloat(model, "posY", &posY);
    rmlBindFloat(model, "posZ", &posZ);
    rmlBindCharPointer(model, "cellText", &cellText);

    rmlLoadDocument(document);
    rmlShowDocument(document);
}

void PlayerGui::removed() {
    rmlUnloadDocument(document);
    rmlUnloadModel(model);
}

void PlayerGui::update() {
    static double lastShown;
    double now = utils::millies();
    if (now <= lastShown + 50) return;

    vec3 pos;
    if (!playerGetPosition(pos)) return;

    posX = pos[0];
    posY = pos[1];
    posZ = pos[2];

    snprintf(cellTextBuf, sizeof(cellTextBuf), "n/a");

    const AzgaarWorld* world = loadingAzgaarGetWorld();
    if (world && world->metersPerPixel > 0.0) {
        float mapX = 0.0f;
        float mapY = 0.0f;
        u32 cellIndex = (u32)-1;
        worldToMap(world, posX, posZ, &mapX, &mapY);
        azgaarWorldSampleHeightCell(world, mapX, mapY, &cellIndex);
        if (cellIndex != (u32)-1) {
            snprintf(cellTextBuf, sizeof(cellTextBuf), "%u", cellIndex);
        }
    }

    lastShown = now;
    rmlUpdateDirtyAll(model);
}
}  // namespace game
