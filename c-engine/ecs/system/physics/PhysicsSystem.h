#pragma once
#include "ecs/system/System.h"

#include "ecs/system/scene/SceneSystem.h"

namespace engine {
class PhysicsSystem : public System {
public:
    PhysicsSystem();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
};

extern PhysicsSystem pyhsicsSystem;

/// True while the Jolt physics world is alive (between the physics system's
/// added() and removed()). Consumers that create/destroy Jolt bodies outside
/// the ECS (e.g. the heightmap terrain's streaming heightfields) must check
/// this so they never touch a destroyed world.
bool physicsSystemJoltActive(void);

/// Create a Jolt static mesh body from raw vertex/index data and attach it
/// to the given entity as a Physics component.  Called from the scene parser
/// for nodes marked with the "notexture" glTF extra.
void physicsCreateMesh(Scene* scene,
                       u32 entity,
                       float* positions,
                       u32 positionCount,
                       u32* indices,
                       u32 indexCount,
                       float* pos,
                       float* rot);

/// Create a Jolt static mesh body from a pre-baked binary blob (produced by
/// jolt-shape-builder at export time).  Skips BVH construction.
void physicsCreateMeshFromBlob(Scene* scene,
                               u32 entity,
                               const void* blob,
                               u32 blobSize,
                               float* pos,
                               float* rot);

/// Create any physics body from a pre-baked shape blob + sidecar metadata.
/// shapeTag, motionTag, mass, friction, restitution come from the .jolt.dat
/// sidecar.  Returns the created Physics component.
void physicsCreateFromBlob(Scene* scene,
                            u32 entity,
                            u8 shapeTag,
                            u8 motionTag,
                            float mass,
                            float friction,
                            float restitution,
                            const void* blob,
                            u32 blobSize,
                            float* pos,
                            float* rot,
                            float worldScale);

/// Create a rigid body from glTF extras (rigidBodyShape).
/// shapeType: "SPHERE", "BOX", "CAPSULE", "CYLINDER", "CONVEX_HULL", "MESH".
/// isDynamic: true for ACTIVE bodies, false for PASSIVE.
/// aabb: float[6] = {minX, minY, minZ, maxX, maxY, maxZ}.
/// positions/indices only needed for MESH and CONVEX_HULL.
void physicsCreateRigidBody(Scene* scene,
                            u32 entity,
                            const char* shapeType,
                            bool isDynamic,
                            float mass,
                            float friction,
                            float restitution,
                            float* aabb,
                            float* positions,
                            u32 positionCount,
                            u32* indices,
                            u32 indexCount,
                            float* pos,
                            float* rot);
}  // namespace engine
