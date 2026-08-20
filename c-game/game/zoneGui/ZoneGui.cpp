#include "Utils.h"
#include "ecs/Ecs.h"
#include "ecs/system/System.h"
#include "player/Player.h"
#include "azgaar/AzgaarCellTracker.h"
#include "azgaar/AzgaarSettlements.h"
#include "azgaar/AzgaarWorld.h"
#include "loadingAzgaar/LoadingAzgaar.h"
#include "rmlui/wrapper/src/crmlui.h"
#include "events/Events.h"

static void added(void);
static void update(void);
static void removed(void);

System zoneGui = {
    .name    = "zoneGui",
    .added   = added,
    .update  = update,
    .removed = removed,
};

static void* document;
static void* model;

// ── Fade timing (milliseconds) ─────────────────────
// quick fade in, short hold, slow fade out — matches the WoW zone banner feel.
#define ZONE_FADE_IN_MS   450.0f
#define ZONE_HOLD_MS      2600.0f
#define ZONE_FADE_OUT_MS  1100.0f

// Neutral wildlands (no province and no state) get this label so the player
// always has a sense of place.
static const char* ZONE_NEUTRAL_NAME = "Wildlands";

static char  zoneTextBuf[48];
static char* zoneTextPtr = zoneTextBuf;
static float zoneAlpha;

static char  zoneSubBuf[48];
static char* zoneSubPtr = zoneSubBuf;
static float zoneSubAlpha;

// Current banner state. The zone part is reactive (no per-frame cell scan):
// the azgaarCellTracker system emits "azgaarCellEntered" on cell change; the
// handler decides whether the *zone* (province/state) actually changed and
// (re)starts the banner.  The settlement part (workstream D) IS a per-frame
// query — settlement boundaries are not cells, so update() polls the
// nearest-settlement check every frame (cheap linear scan, ~800 entries).
static u32    curProvinceId;
static u32    curStateId;
static u32    curSettlementId; // 0 = not in a settlement
static bool   initialized;     // has any zone been determined yet?
static double changeTimeMs;   // wall-clock ms when the current banner started
static bool   active;         // is a banner currently on screen

static void pickDisplayText(const AzgaarZoneInfo* z) {
    if (z->provinceName[0] != '\0') {
        snprintf(zoneTextBuf, sizeof(zoneTextBuf), "%s", z->provinceName);
        snprintf(zoneSubBuf, sizeof(zoneSubBuf), "%s", z->stateName);
    } else if (z->stateName[0] != '\0') {
        snprintf(zoneTextBuf, sizeof(zoneTextBuf), "%s", z->stateName);
        zoneSubBuf[0] = '\0';
    } else {
        snprintf(zoneTextBuf, sizeof(zoneTextBuf), "%s", ZONE_NEUTRAL_NAME);
        zoneSubBuf[0] = '\0';
    }
}

// Signal handler for "azgaarCellEntered". Fires on every pack-cell change, but
// the banner only (re)starts when the resolved zone differs from the last one
// shown — exactly the "get cell details, see if zone changed" pattern.
static void onCellEntered(void* userData) {
    const AzgaarCellEvent* ev  = static_cast<const AzgaarCellEvent*>(userData);
    if (!ev) return;
    const AzgaarWorld* world = ev->world;

    // Workstream D: a settlement banner takes priority over a zone banner.
    // If the player is inside a settlement's footprint, don't let the zone
    // change overwrite the settlement banner.
    if (world && world->metersPerPixel > 0.0f) {
        float mpp = (float)world->metersPerPixel;
        float wx = ((float)world->widthPx * 0.5f - ev->mapX) * mpp;
        float wz = ((float)world->heightPx * 0.5f - ev->mapY) * mpp;
        if (azgaarSettlementsNearest(world, wx, wz)) return;
    }

    // The first determination always starts the banner (so spawning into
    // neutral wildlands, province 0 / state 0, still announces something).
    // After that we only (re)start on an actual zone change.
    if (initialized && ev->zone.provinceId == curProvinceId && ev->zone.stateId == curStateId) return;

    curProvinceId = ev->zone.provinceId;
    curStateId    = ev->zone.stateId;
    initialized   = true;
    changeTimeMs  = millies();
    active        = true;
    pickDisplayText(&ev->zone);

    info("zoneGui: entered '%s'%s%s",
         zoneTextBuf,
         zoneSubBuf[0] ? ", " : "",
         zoneSubBuf[0] ? zoneSubBuf : "");
}

static void added(void) {
    document = rmlNewDocument("gui/zone/zone.html");
    model    = rmlCreateModel("zone");

    rmlBindCharPointer(model, "zoneText", &zoneTextPtr);
    rmlBindFloat(model, "zoneAlpha", &zoneAlpha);
    rmlBindCharPointer(model, "zoneSub", &zoneSubPtr);
    rmlBindFloat(model, "zoneSubAlpha", &zoneSubAlpha);

    zoneTextBuf[0] = '\0';
    zoneSubBuf[0]  = '\0';
    zoneAlpha      = 0.0f;
    zoneSubAlpha   = 0.0f;
    curProvinceId  = 0u;
    curStateId     = 0u;
    curSettlementId = 0u;
    initialized    = false;
    active         = false;

    signalSubscribe("azgaarCellEntered", onCellEntered);

    rmlLoadDocument(document);
    rmlShowDocumentWithoutFocus(document);

    // Seed the initial zone. The tracker may have emitted its first cell event
    // before this GUI subscribed (GUIs are added a frame later than systems),
    // so we resolve the current zone once here instead of relying on a future
    // cell change. This is a single query at startup, not per-frame.
    const AzgaarWorld* world = loadingAzgaarGetWorld();
    vec3 playerPos;
    if (world && world->metersPerPixel > 0.0f && playerGetPosition(playerPos)) {
        float invMpp = 1.0f / (float)world->metersPerPixel;
        float mapX   = -playerPos[0] * invMpp + (float)world->widthPx  * 0.5f;
        float mapY   = -playerPos[2] * invMpp + (float)world->heightPx * 0.5f;
        AzgaarZoneInfo zone;
        azgaarWorldSampleZone(world, mapX, mapY, &zone, NULL);
        AzgaarCellEvent seed = {
            .world = world,
            .zone  = zone,
            .mapX  = mapX,
            .mapY  = mapY,
        };
        onCellEntered(&seed);

        // Seed the settlement state (workstream D): if the spawn point is
        // inside a settlement, the banner should announce the settlement.
        const AzgaarSettlement* s = azgaarSettlementsNearest(world, playerPos[0], playerPos[2]);
        curSettlementId = s ? s->id : 0u;
        if (s) {
            snprintf(zoneTextBuf, sizeof(zoneTextBuf), "%s", s->name);
            if (s->stateId < world->stateCount) {
                snprintf(zoneSubBuf, sizeof(zoneSubBuf), "%s", world->states[s->stateId].name);
            } else {
                zoneSubBuf[0] = '\0';
            }
            changeTimeMs = millies();
            active       = true;
            info("zoneGui: spawned in settlement '%s'%s%s",
                 s->name,
                 zoneSubBuf[0] ? " - " : "",
                 zoneSubBuf[0] ? zoneSubBuf : "");
        }
    }
}

static void removed(void) {
    signalRemoveSubscription("azgaarCellEntered", onCellEntered);

    rmlUnloadDocument(document);
    rmlUnloadModel(model);
    document = NULL;
    model    = NULL;
    initialized     = false;
    active          = false;
    curSettlementId = 0u;
}

static void update(void) {
    // Workstream D: poll the nearest settlement every frame (settlement
    // boundaries are not cells, so the cell signal can't detect entering or
    // leaving a town).  Cheap: one linear scan of ~800 entries.
    const AzgaarWorld* world = loadingAzgaarGetWorld();
    vec3 p;
    if (world && world->metersPerPixel > 0.0f && playerGetPosition(p)) {
        const AzgaarSettlement* s = azgaarSettlementsNearest(world, p[0], p[2]);
        u32 sid = s ? s->id : 0u;
        if (sid != curSettlementId) {
            curSettlementId = sid;
            changeTimeMs    = millies();
            active          = true;
            if (s) {
                snprintf(zoneTextBuf, sizeof(zoneTextBuf), "%s", s->name);
                if (s->stateId < world->stateCount) {
                    snprintf(zoneSubBuf, sizeof(zoneSubBuf), "%s", world->states[s->stateId].name);
                } else {
                    zoneSubBuf[0] = '\0';
                }
                info("zoneGui: entered settlement '%s'%s%s",
                     s->name,
                     zoneSubBuf[0] ? " - " : "",
                     zoneSubBuf[0] ? zoneSubBuf : "");
            } else {
                // Left the settlement: restore the zone-based label at the
                // player's current position.
                float invMpp = 1.0f / (float)world->metersPerPixel;
                float mapX = -p[0] * invMpp + (float)world->widthPx * 0.5f;
                float mapY = -p[2] * invMpp + (float)world->heightPx * 0.5f;
                AzgaarZoneInfo zone;
                azgaarWorldSampleZone(world, mapX, mapY, &zone, NULL);
                curProvinceId = zone.provinceId;
                curStateId    = zone.stateId;
                initialized   = true;
                pickDisplayText(&zone);
            }
        }
    }

    if (!active) {
        zoneAlpha    = 0.0f;
        zoneSubAlpha = 0.0f;
        rmlUpdateDirtyAll(model);
        return;
    }

    double elapsed = millies() - changeTimeMs;
    float  total   = ZONE_FADE_IN_MS + ZONE_HOLD_MS + ZONE_FADE_OUT_MS;

    if (elapsed >= total) {
        active       = false;
        zoneAlpha    = 0.0f;
        zoneSubAlpha = 0.0f;
    } else if (elapsed < ZONE_FADE_IN_MS) {
        zoneAlpha = (float)elapsed / ZONE_FADE_IN_MS;
    } else if (elapsed < ZONE_FADE_IN_MS + ZONE_HOLD_MS) {
        zoneAlpha = 1.0f;
    } else {
        zoneAlpha = 1.0f - (float)(elapsed - ZONE_FADE_IN_MS - ZONE_HOLD_MS) / ZONE_FADE_OUT_MS;
        if (zoneAlpha < 0.0f) zoneAlpha = 0.0f;
    }

    // Subtitle tracks the main banner alpha but is hidden when empty.
    zoneSubAlpha = (zoneSubBuf[0] != '\0') ? zoneAlpha : 0.0f;

    rmlUpdateDirtyAll(model);
}
