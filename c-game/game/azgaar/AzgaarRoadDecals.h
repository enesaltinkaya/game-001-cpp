#pragma once
#include "azgaar/AzgaarWorld.h"

namespace game {
void azgaarRoadDecalsBuild(const AzgaarWorld* world);
void azgaarRoadDecalsClear(void);
void azgaarRoadDecalsSetVisible(bool visible);
}  // namespace game
