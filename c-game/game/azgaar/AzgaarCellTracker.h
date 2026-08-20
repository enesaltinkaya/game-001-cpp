#pragma once

#include "azgaar/AzgaarWorld.h"

struct System;

// Fired (via Events.h) each time the player enters a different Azgaar *pack*
// cell. Payload is a pointer to AzgaarCellEvent (valid only for the duration of
// the signal dispatch). Subscribe with signalSubscribe("azgaarCellEntered", fn).
//
// Note: this fires on pack-cell change, which is more granular than zone change
// (a province typically spans many cells). Listeners that care about higher-
// level regions should compare the resolved AzgaarZoneInfo themselves.
struct AzgaarCellEvent {
    const AzgaarWorld* world;       // retained world; valid during gameplay
    u32                packCellIndex;
    AzgaarZoneInfo     zone;        // resolved province/state at this cell
    float              mapX;        // player position in Azgaar map pixels
    float              mapY;
};

extern System azgaarCellTrackerSystem;
