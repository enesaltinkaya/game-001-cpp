#pragma once

#include "ecs/system/System.h"

typedef struct RcNavMesh RcNavMesh;

namespace engine {
class VulkanDebugNavMeshPass : public System {
public:
    VulkanDebugNavMeshPass();
    void added() override;
    void removed() override;
    void update() override;
};

extern VulkanDebugNavMeshPass vulkanDebugNavMeshPass;

/// Toggle navmesh debug visualization on/off.
void vulkanDebugNavMeshSetEnabled(char enabled);
char vulkanDebugNavMeshIsEnabled(void);

/// Register the navmesh for debug visualization. Pass NULL to unregister.
void vulkanDebugNavMeshSetMesh(RcNavMesh* mesh);
}  // namespace engine
