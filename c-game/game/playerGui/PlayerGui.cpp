#include "Utils.h"
#include "azgaar/AzgaarWorld.h"
#include "loadingAzgaar/LoadingAzgaar.h"
#include "ecs/system/System.h"
#include "player/Player.h"
#include "rmlui/wrapper/src/crmlui.h"

static void added(void);
static void update(void);
static void removed(void);

struct System playerGui = {
    .name    = "playerGui",
    .added   = added,
    .update  = update,
    .removed = removed,
};

static void* document;
static void* model;
static float posX, posY, posZ;
static char* cellText;
static char cellTextBuf[32];

static void worldToMap(const AzgaarWorld* world, float wx, float wz, float* outMapX, float* outMapY) {
    *outMapX = ((-wx) / (float)world->metersPerPixel) + (float)world->widthPx * 0.5f;
    *outMapY = ((-wz) / (float)world->metersPerPixel) + (float)world->heightPx * 0.5f;
}

static void added(void) {
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

static void removed(void) {
    rmlUnloadDocument(document);
    rmlUnloadModel(model);
}

static void update(void) {
    static double lastShown;
    double now = millies();
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
