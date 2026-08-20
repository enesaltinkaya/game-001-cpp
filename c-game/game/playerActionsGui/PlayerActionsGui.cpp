#include "PlayerActionsGui.h"
#include "Utils.h"
#include "azgaar/AzgaarWorld.h"
#include "ecs/system/System.h"
#include "ecs/system/lua/LuaSystem.h"
#include "loadingAzgaar/LoadingAzgaar.h"
#include "player/Player.h"
#include "rmlui/wrapper/src/crmlui.h"

namespace game {
static int teleportToCell(void* _);
static int teleportToOrigin(void* _);
static int playerActionsToggle(void* _);

PlayerActionsGui playerActionsGui;

PlayerActionsGui::PlayerActionsGui() : engine::System("playerActionsGui") {}

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

void PlayerActionsGui::added() {
    engine::luaRegisterFunction("playerActionTeleportToCell", teleportToCell);
    engine::luaRegisterFunction("playerActionTeleportToOrigin", teleportToOrigin);
    engine::luaRegisterFunction("playerActionsToggle", playerActionsToggle);

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

void PlayerActionsGui::removed() {
    rmlUnloadDocument(document);
    rmlUnloadModel(model);
    document = nullptr;
    model = nullptr;
}

void PlayerActionsGui::update() {}

static int playerActionsToggle(void* _) {
    static_cast<void>(_);
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
    static_cast<void>(_);

    vec3 pos = {0.0f, 0.0f, 0.0f};
    if (!playerTeleportTo(pos)) {
        setStatus("Player is not ready");
        return 0;
    }

    setStatus("Teleported to 0, 0, 0");
    utils::info("playerActionsGui: teleported player to origin (0.00 0.00 0.00)");
    return 0;
}

static int teleportToCell(void* _) {
    static_cast<void>(_);

    const AzgaarWorld* world = loadingAzgaarGetWorld();
    if (!world || world->cells.empty() || world->cellCount == 0u) {
        setStatus("Azgaar world is not loaded");
        return 0;
    }

    u32 id = static_cast<u32>(cellId + 0.5f);
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
    utils::info("playerActionsGui: teleported player to Azgaar cell %u (%.2f %.2f %.2f)", id, pos[0], pos[1], pos[2]);
    return 0;
}
}  // namespace game
