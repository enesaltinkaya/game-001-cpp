#pragma once

typedef struct System System;

// Gameplay HUD overlay that announces the current Azgaar region (province /
// state) with a World-of-Warcraft-style fade in / hold / fade out each time the
// player crosses into a new zone.
extern System zoneGui;
