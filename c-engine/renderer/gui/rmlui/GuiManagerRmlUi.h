#pragma once
#include "guis/debugGui/DebugGui.h"                    // IWYU pragma: keep
#include "guis/passStatsGui/PassStatsGui.h"            // IWYU pragma: keep
#include "guis/showFpsGui/ShowFpsGui.h"               // IWYU pragma: keep
#include "guis/statsGui/StatsGui.h"                    // IWYU pragma: keep

struct System;
extern struct System guiManagerRmlUi;

void guiManagerAddGuiNextFrame(struct System* gui);
void guiManagerRemoveGuiNextFrame(struct System* gui);

extern struct System** rmluiGuis;  // stb array

///////////////////////////////////
void guiManagerUpdateScale(void);
void guiManagerUpdateCursors(void);
void guiManagerToggleShowFps(void);
void guiManagerReleaseTexture(const char* name);
void guiManagerReleaseAllTextures(void);
