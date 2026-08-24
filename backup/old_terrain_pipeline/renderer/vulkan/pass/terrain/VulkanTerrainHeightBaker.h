#pragma once

#include "ecs/system/terrain/Terrain.h"

void vulkanTerrainHeightBakerBake(Terrain* terrain, float spacing);
void vulkanTerrainHeightBakerDestroy(void);
