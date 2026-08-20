#pragma once

#include "ecs/system/scene/SceneSystem.h"

typedef struct RcNavMesh RcNavMesh;
typedef struct DtNavMeshQuery DtNavMeshQuery;

typedef struct NavMeshData {
    RcNavMesh*     navMesh;
    DtNavMeshQuery* query;
} NavMeshData;

extern struct System navMeshSystem;

/// Find a path from start to end in the given scene's navmesh.
/// outPath: float[3] waypoint array. Returns waypoint count (0 = no path).
uint32_t navMeshFindPath(Scene* scene,
                          const float* startPos,
                          const float* endPos,
                          float* outPath,
                          uint32_t maxPath);

/// Get the nearest point on the navmesh to the given world position.
/// Returns 1 on success.
int navMeshClosestPoint(Scene* scene, const float* pos, float* outPoint);

/// Get the loaded navmesh (for debug visualization). Returns NULL if not loaded.
RcNavMesh* navMeshGetMesh(void);
