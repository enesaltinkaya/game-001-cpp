#include "Utils.h"
#include "azgaar/AzgaarWorld.h"
#include "ecs/system/System.h"
#include "ecs/system/lua/LuaSystem.h"
#include "loadingAzgaar/LoadingAzgaar.h"
#include "player/Player.h"
#include "rmlui/wrapper/src/crmlui.h"

static void added(void);
static void removed(void);
static void update(void);
static int teleportToCell(void* _);
static int teleportToOrigin(void* _);
static int playerActionsToggle(void* _);

struct System playerActionsGui = {
    .name    = "playerActionsGui",
    .added   = added,
    .update  = update,
    .removed = removed,
};

static void* document;
static void* model;
static float cellId;
static char* statusText;
static char statusTextBuf[128];

static void setStatus(const char* text) {
    snprintf(statusTextBuf, sizeof(statusTextBuf), "%s", text);
    statusText = statusTextBuf;
    if (model) rmlUpdateDirtyAll(model);
}

static void added(void) {
    luaRegisterFunction("playerActionTeleportToCell", teleportToCell);
    luaRegisterFunction("playerActionTeleportToOrigin", teleportToOrigin);
    luaRegisterFunction("playerActionsToggle", playerActionsToggle);

    cellId = 0.0f;
    statusText = statusTextBuf;
    snprintf(statusTextBuf, sizeof(statusTextBuf), "Enter Azgaar cell id");

    document = rmlNewDocument("gui/playerActions/playerActions.html");
    model    = rmlCreateModel("playerActions");
    rmlBindFloat(model, "cellId", &cellId);
    rmlBindCharPointer(model, "statusText", &statusText);

    rmlLoadDocument(document);
    rmlShowDocument(document);
}

static void removed(void) {
    rmlUnloadDocument(document);
    rmlUnloadModel(model);
    document = NULL;
    model = NULL;
}

static void update(void) {}

static int playerActionsToggle(void* _) {
    (void)_;
    void* body = rmlGetElementById(document, "playerActionsBody");
    if (!body) return 0;
    if (rmlElementHasClass(body, "collapsed")) {
        rmlRemoveElementClass(body, "collapsed");
    } else {
        rmlSetElementClass(body, "collapsed");
    }
    return 0;
}

static int teleportToOrigin(void* _) {
    (void)_;

    vec3 pos = {0.0f, 0.0f, 0.0f};
    if (!playerTeleportTo(pos)) {
        setStatus("Player is not ready");
        return 0;
    }

    setStatus("Teleported to 0, 0, 0");
    info("playerActionsGui: teleported player to origin (0.00 0.00 0.00)");
    return 0;
}

static int teleportToCell(void* _) {
    (void)_;

    const AzgaarWorld* world = loadingAzgaarGetWorld();
    if (!world || !world->cells || world->cellCount == 0u) {
        setStatus("Azgaar world is not loaded");
        return 0;
    }

    u32 id = (u32)(cellId + 0.5f);
    if (id >= world->cellCount) {
        snprintf(statusTextBuf, sizeof(statusTextBuf), "Invalid cell %u (max %u)", id, world->cellCount - 1u);
        statusText = statusTextBuf;
        rmlUpdateDirtyAll(model);
        return 0;
    }

    const AzgaarCell* cell = &world->cells[id];
    float wx = 0.0f;
    float wz = 0.0f;
    azgaarMapToWorld(world, cell->x, cell->y, &wx, &wz);

    float h = azgaarHeightToMeters(world, cell->height);
    vec3 pos = {wx, h + 1.0f, wz};
    if (!playerTeleportTo(pos)) {
        setStatus("Player is not ready");
        return 0;
    }

    snprintf(statusTextBuf, sizeof(statusTextBuf), "Teleported to cell %u", id);
    statusText = statusTextBuf;
    rmlUpdateDirtyAll(model);
    info("playerActionsGui: teleported player to Azgaar cell %u (%.2f %.2f %.2f)", id, pos[0], pos[1], pos[2]);
    return 0;
}
