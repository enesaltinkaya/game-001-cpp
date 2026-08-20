#pragma once

#include "ecs/system/terrain/Terrain.h"

typedef void (*TerrainLoadCallback)(Terrain* terrain, void* userData);

Terrain* terrainLoad(const char* path);
Terrain* terrainLoadCb(const char* path, TerrainLoadCallback callback, void* userData);
void terrainDestroy(Terrain* terrain);
