#pragma once

#include "ecs/system/scene/SceneSystem.h"
#include "ecs/system/System.h"  // IWYU pragma: keep

struct RcNavMesh;
struct DtNavMeshQuery;
namespace game {

struct NavMeshData {
    RcNavMesh*     navMesh;
    DtNavMeshQuery* query;
};

class NavMeshSystem : public engine::System {
public:
    NavMeshSystem();
    void added() override;
    void removed() override;
    void update() override;
};

extern NavMeshSystem navMeshSystem;

/// Find a path from start to end in the given scene's navmesh.
/// outPath: float[3] waypoint array. Returns waypoint count (0 = no path).
uint32_t navMeshFindPath(engine::Scene* scene,
                          const float* startPos,
                          const float* endPos,
                          float* outPath,
                          uint32_t maxPath);

/// Get the nearest point on the navmesh to the given world position.
/// Returns 1 on success.
int navMeshClosestPoint(engine::Scene* scene, const float* pos, float* outPoint);

/// Get the loaded navmesh (for debug visualization). Returns nullptr if not loaded.
RcNavMesh* navMeshGetMesh(void);
}  // namespace game
