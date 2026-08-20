#pragma once

#include "ecs/system/System.h"

typedef struct RcNavMesh RcNavMesh;

extern System vulkanDebugNavMeshPass;

/// Toggle navmesh debug visualization on/off.
void vulkanDebugNavMeshSetEnabled(char enabled);
char vulkanDebugNavMeshIsEnabled(void);

/// Register the navmesh for debug visualization. Pass NULL to unregister.
void vulkanDebugNavMeshSetMesh(RcNavMesh* mesh);
