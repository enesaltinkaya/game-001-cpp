#include "Utils.h"
#include "ecs/system/System.h"
#include "events/Events.h"
#include "player/Player.h"
#include "azgaar/AzgaarCellTracker.h"
#include "azgaar/AzgaarWorld.h"
#include "loadingAzgaar/LoadingAzgaar.h"

static void added(void);
static void update(void);
static void removed(void);

// A location tracker for the player over the Azgaar world. Each frame it
// resolves the player's pack cell and, when that cell changes, emits the
// "azgaarCellEntered" signal carrying the resolved zone. This is the single
// source of truth for "where is the player on the Azgaar map" so that other
// systems (zone banner, biome hints, encounters, ...) can react without each
// running their own per-frame cell scan.
System azgaarCellTrackerSystem = {
    .name                = "azgaarCellTracker",
    .added               = added,
    .removed             = removed,
    .preUpdate           = nullptr,
    .update              = update,
    .postUpdate          = nullptr,
    .cpuElapsedLastFrame = 0.0,
    .cpuElapsed          = 0.0,
    .gpuElapsed          = 0.0,
    .priority            = 0,
};

#define AZGAAR_CELL_ENTERED_SIGNAL "azgaarCellEntered"

// Re-sample only when the player has moved at least this fraction of the
// average pack-cell spacing (in map pixels). Keeps change detection accurate
// near boundaries while making standing-still frames essentially free.
#define AZGAAR_MOVE_RESAMPLE_FRACTION 0.25f

static AzgaarCellEvent payload;

static bool   initialized;
static u32    lastPackCellIndex;
static float  lastMapX;
static float  lastMapY;
static float  moveThresholdPx;   // cached avg spacing * fraction; 0 until known

static void added(void) {
    initialized     = false;
    lastPackCellIndex = 0u;
    lastMapX        = 0.0f;
    lastMapY        = 0.0f;
    moveThresholdPx = 0.0f;
}

static void removed(void) {
    initialized = false;
}

static void update(void) {
    const AzgaarWorld* world = loadingAzgaarGetWorld();
    if (!world || world->metersPerPixel <= 0.0f) return;

    vec3 playerPos;
    if (!playerGetPosition(playerPos)) return;

    // World meters -> Azgaar map pixels (map centre is the world origin; axes mirrored).
    float invMpp = 1.0f / static_cast<float>(world->metersPerPixel);
    float mapX   = -playerPos[0] * invMpp + static_cast<float>(world->widthPx)  * 0.5f;
    float mapY   = -playerPos[2] * invMpp + static_cast<float>(world->heightPx) * 0.5f;

    // Lazy + safe cache of the re-sample threshold from the world's own metrics.
    if (moveThresholdPx <= 0.0f && world->packCellCount > 0u) {
        float area    = static_cast<float>(world->widthPx) * static_cast<float>(world->heightPx);
        float spacing = sqrtf(area / static_cast<float>(world->packCellCount));
        moveThresholdPx = spacing * AZGAAR_MOVE_RESAMPLE_FRACTION;
        if (moveThresholdPx < 1.0f) moveThresholdPx = 1.0f;
    }

    // Stationary skip: while the player stands still (or moves less than a
    // fraction of a cell), reuse the last result and do no cell scan.
    if (initialized) {
        float dx = mapX - lastMapX;
        float dy = mapY - lastMapY;
        if (dx * dx + dy * dy < moveThresholdPx * moveThresholdPx) return;
    }

    AzgaarZoneInfo zone;
    u32            packCellIndex = 0u;
    azgaarWorldSampleZone(world, mapX, mapY, &zone, &packCellIndex);

    lastMapX = mapX;
    lastMapY = mapY;

    // First resolution or the player crossed into a new cell.
    if (!initialized || packCellIndex != lastPackCellIndex) {
        lastPackCellIndex = packCellIndex;
        initialized       = true;

        payload = AzgaarCellEvent{
            .world         = world,
            .packCellIndex = packCellIndex,
            .zone          = zone,
            .mapX          = mapX,
            .mapY          = mapY,
        };
        signalEmit(AZGAAR_CELL_ENTERED_SIGNAL, &payload);
    }
}
