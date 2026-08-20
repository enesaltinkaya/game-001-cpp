#pragma once

#include "ecs/system/System.h"

extern struct System loadingSystem;

// Current loading stage text (for the UI).
const char* loadingStageText(void);

// Called by GameState during enter/exit transitions.
void loadingOnEnter(void);
void loadingOnExit(void);
void loadingTransferAssets(void);