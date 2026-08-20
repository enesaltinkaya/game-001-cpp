#include "cglm/git/include/cglm/types-struct.h"
#include "ecs/Ecs.h"
#include "ecs/system/System.h"
#include "ecs/system/window/WindowSystem.h"
#include "ecs/system/transform/TransformComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/transform/TransformSystem.h"
#include "ecs/system/physics/PhysicsComponent.h"
#include "ecs/system/physics/PhysicsSystem.h"

#include "renderer/vulkan/pass/debug_physics/VulkanDebugPhysicsPass.h"
#include "renderer/vulkan/pass/debug_navmesh/VulkanDebugNavMeshPass.h"

static void added(void);
static void removed(void);
static void preUpdate(void);
static void update(void);

struct System pyhsicsSystem = {
    .name      = "physics",
    .added     = added,
    .removed   = removed,
    .preUpdate = preUpdate,
    .update    = update,
};

// entity → Scene* mapping for dynamic bodies so we can look up
// which scene an active body belongs to in O(1).
static Map(u32, Scene*) dynamicBodyScenes;

// Scratch buffer for joltGetActiveTransforms().
// 512 should be more than enough for active dynamic bodies per frame.
#define MAX_ACTIVE_TRANSFORMS 512
static JoltActiveTransform activeTransforms[MAX_ACTIVE_TRANSFORMS];

// True while the Jolt world is alive (between added() and removed()).
// Consumers that create/destroy Jolt bodies outside the ECS (e.g. the
// heightmap terrain's streaming heightfields) check this so they never touch
// a destroyed world.
static bool joltActive = false;

bool physicsSystemJoltActive(void) {
    return joltActive;
}

void added(void) {
    joltInit();
    joltActive = true;
}

void removed(void) {
    joltActive = false;
    joltDestroy();
    mapFree(dynamicBodyScenes);
}

void preUpdate(void) {
    // Toggle all debug visualization with Ctrl+H
    if (input.ctrl && input.pressed == KEY_H) {
        char current = vulkanDebugPhysicsIsEnabled();
        vulkanDebugPhysicsSetEnabled(!current);
        vulkanDebugNavMeshSetEnabled(!current);
        info("debug visualization: %s", !current ? "ON" : "OFF");
    }

    if (input.ctrl && input.pressed == KEY_P) {
        Entity* cameraEntity = cameraGetEntity();
        Transform* transform = getComponent(cameraEntity->scene, Transform, cameraEntity->id);
        vec3 direction;
        transformGetDirection(cameraEntity->scene, cameraEntity->id, direction);

        vec3 touch;
        if (joltCastRay(transform->pos, direction, 5000, touch)) {
            float distance = glm_vec3_distance(touch, transform->pos);
            info("yup you touch my tralala %f %f %f dist:%f",
                 touch[0],
                 touch[1],
                 touch[2],
                 distance);
        } else {
            info("miss!");
        }
    }
}

void update(void) {
    joltUpdate(0.02F);

    // Sync active dynamic body transforms back to ECS
    u32 count = joltGetActiveTransforms(activeTransforms, MAX_ACTIVE_TRANSFORMS);
    for (u32 i = 0; i < count; i++) {
        JoltActiveTransform* at = &activeTransforms[i];
        u32 entity              = (u32)at->userData;

        Scene* scene = mapGet(dynamicBodyScenes, entity);
        if (!scene) continue;

        Transform* transform = getComponent(scene, Transform, entity);
        if (!transform) continue;

        transformSaveLast(scene, entity);

        transform->pos[0] = at->pos[0];
        transform->pos[1] = at->pos[1];
        transform->pos[2] = at->pos[2];
        // pos[3] is scale — physics doesn't change it

        transform->rot[0] = at->rot[0];
        transform->rot[1] = at->rot[1];
        transform->rot[2] = at->rot[2];
        transform->rot[3] = at->rot[3];
    }
}

void physicsCreateMesh(Scene* scene,
                       u32 entity,
                       float* positions,
                       u32 positionCount,
                       u32* indices,
                       u32 indexCount,
                       float* pos,
                       float* rot) {
    Physics* physics   = createComponent(scene, Physics, entity);
    physics->shapeType = PHYSICS_SHAPE_MESH;
    physics->isDynamic = false;
    // indexType 1 = 32-bit indices (we already unpacked to u32 in the parser)
    physics->joltMesh =
        joltCreateMeshShapeNoCache(positions, positionCount, indices, indexCount, pos, rot, 1, (uint64_t)entity);
    info("physics: created mesh body for entity %u (verts=%u, indices=%u)",
         entity,
         positionCount / 3,
         indexCount);
}

void physicsCreateMeshFromBlob(Scene* scene,
                               u32 entity,
                               const void* blob,
                               u32 blobSize,
                               float* pos,
                               float* rot) {
    JoltMesh* joltMesh = joltCreateMeshShapeFromBlob(blob, blobSize, pos, rot, 0);
    if (!joltMesh) {
        warn("physics: failed to restore pre-baked mesh for entity %u", entity);
        return;
    }
    Physics* physics   = createComponent(scene, Physics, entity);
    physics->shapeType = PHYSICS_SHAPE_MESH;
    physics->isDynamic = false;
    physics->joltMesh  = joltMesh;
    info("physics: created mesh body from pre-baked blob for entity %u (%u bytes)",
         entity,
         blobSize);
}

// Shape type tags (must match jolt-shape-builder's ShapeTypeTag enum).
#define PHYSICS_BLOB_TAG_NOTEXTURE   0
#define PHYSICS_BLOB_TAG_BOX         1
#define PHYSICS_BLOB_TAG_SPHERE      2
#define PHYSICS_BLOB_TAG_CAPSULE     3
#define PHYSICS_BLOB_TAG_CYLINDER    4
#define PHYSICS_BLOB_TAG_CONVEX_HULL 5
#define PHYSICS_BLOB_TAG_MESH        6

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
                            float worldScale) {
    Physics* physics   = createComponent(scene, Physics, entity);
    physics->isDynamic = motionTag == 1; // 0=STATIC, 1=DYNAMIC

    int motionType    = physics->isDynamic ? JOLT_MOTION_DYNAMIC : JOLT_MOTION_STATIC;
    uint64_t userData = (uint64_t)entity;

    if (physics->isDynamic) {
        mapPut(dynamicBodyScenes, entity, scene);
    }

    float scale[3] = {worldScale, worldScale, worldScale};
    bool hasScale = worldScale != 1.0f;

    // For static mesh/notexture shapes, use the JoltMesh path.
    // For everything else, use JoltBody (which handles mass/friction/restitution).
    if (!physics->isDynamic && (shapeTag == PHYSICS_BLOB_TAG_NOTEXTURE ||
                                shapeTag == PHYSICS_BLOB_TAG_MESH)) {
        void* result = joltCreateBodyFromShapeBlob(blob, blobSize, motionType,
                                                   mass, friction, restitution,
                                                   hasScale ? scale : NULL,
                                                   pos, rot, userData);
        if (!result) {
            warn("physics: failed to restore pre-baked shape for entity %u", entity);
            return;
        }
        physics->shapeType = PHYSICS_SHAPE_MESH;
        physics->joltMesh  = (JoltMesh*)result;
    } else {
        void* result = joltCreateBodyFromShapeBlob(blob, blobSize, motionType,
                                                   mass, friction, restitution,
                                                   hasScale ? scale : NULL,
                                                   pos, rot, userData);
        if (!result) {
            warn("physics: failed to restore pre-baked shape for entity %u", entity);
            return;
        }
        physics->joltBody = (JoltBody*)result;

        // Map shape tag to component enum for debug vis / serialization.
        switch (shapeTag) {
        case PHYSICS_BLOB_TAG_BOX:         physics->shapeType = PHYSICS_SHAPE_BOX;         break;
        case PHYSICS_BLOB_TAG_SPHERE:      physics->shapeType = PHYSICS_SHAPE_SPHERE;      break;
        case PHYSICS_BLOB_TAG_CAPSULE:     physics->shapeType = PHYSICS_SHAPE_CAPSULE;     break;
        case PHYSICS_BLOB_TAG_CYLINDER:    physics->shapeType = PHYSICS_SHAPE_CYLINDER;    break;
        case PHYSICS_BLOB_TAG_CONVEX_HULL: physics->shapeType = PHYSICS_SHAPE_CONVEX_HULL; break;
        case PHYSICS_BLOB_TAG_MESH:        physics->shapeType = PHYSICS_SHAPE_MESH;        break;
        default:                           physics->shapeType = PHYSICS_SHAPE_MESH;        break;
        }
    }

    info("physics: created %s body from pre-baked blob for entity %u (%u bytes, scale=%.4f)",
         physics->isDynamic ? "dynamic" : "static",
         entity,
         blobSize,
         worldScale);
}

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
                            float* rot) {
    Physics* physics   = createComponent(scene, Physics, entity);
    physics->isDynamic = isDynamic;

    int motionType    = isDynamic ? JOLT_MOTION_DYNAMIC : JOLT_MOTION_STATIC;
    uint64_t userData = (uint64_t)entity;

    // Track dynamic bodies for transform sync
    if (isDynamic) {
        mapPut(dynamicBodyScenes, entity, scene);
    }

    // Compute half-extents from AABB
    float hx = (aabb[3] - aabb[0]) * 0.5f;
    float hy = (aabb[4] - aabb[1]) * 0.5f;
    float hz = (aabb[5] - aabb[2]) * 0.5f;

    if (strequals(shapeType, "SPHERE")) {
        physics->shapeType = PHYSICS_SHAPE_SPHERE;
        float radius       = hx;
        if (hy > radius) radius = hy;
        if (hz > radius) radius = hz;
        physics->joltBody = joltCreateSphereShape(radius,
                                                  pos,
                                                  rot,
                                                  motionType,
                                                  mass,
                                                  friction,
                                                  restitution,
                                                  userData);
        // info("physics: created %s sphere for entity %u (r=%.2f)",
        //      isDynamic ? "dynamic" : "static",
        //      entity,
        //      radius);

    } else if (strequals(shapeType, "BOX")) {
        physics->shapeType   = PHYSICS_SHAPE_BOX;
        float halfExtents[3] = {hx, hy, hz};
        physics->joltBody    = joltCreateBoxShape(halfExtents,
                                                  pos,
                                                  rot,
                                                  motionType,
                                                  mass,
                                                  friction,
                                                  restitution,
                                                  userData);
        // info("physics: created %s box for entity %u (half=%.2f,%.2f,%.2f)",
        //      isDynamic ? "dynamic" : "static",
        //      entity,
        //      hx,
        //      hy,
        //      hz);

    } else if (strequals(shapeType, "CAPSULE")) {
        physics->shapeType = PHYSICS_SHAPE_CAPSULE;
        float radius       = hx > hz ? hx : hz;
        float halfHeight   = hy > radius ? hy - radius : 0.0f;
        physics->joltBody  = joltCreateCapsuleShape(halfHeight,
                                                    radius,
                                                    pos,
                                                    rot,
                                                    motionType,
                                                    mass,
                                                    friction,
                                                    restitution,
                                                    userData);
        // info("physics: created %s capsule for entity %u (halfH=%.2f, r=%.2f)",
        //      isDynamic ? "dynamic" : "static",
        //      entity,
        //      halfHeight,
        //      radius);

    } else if (strequals(shapeType, "CYLINDER")) {
        physics->shapeType = PHYSICS_SHAPE_CYLINDER;
        float radius       = hx > hz ? hx : hz;
        physics->joltBody  = joltCreateCylinderShape(hy,
                                                     radius,
                                                     pos,
                                                     rot,
                                                     motionType,
                                                     mass,
                                                     friction,
                                                     restitution,
                                                     userData);
        // info("physics: created %s cylinder for entity %u (halfH=%.2f, r=%.2f)",
        //      isDynamic ? "dynamic" : "static",
        //      entity,
        //      hy,
        //      radius);

    } else if (strequals(shapeType, "CONVEX_HULL")) {
        physics->shapeType = PHYSICS_SHAPE_CONVEX_HULL;
        assert(positions && positionCount > 0 && "CONVEX_HULL requires mesh vertex data");
        physics->joltBody = joltCreateConvexHullShape(positions,
                                                      positionCount / 3,
                                                      pos,
                                                      rot,
                                                      motionType,
                                                      mass,
                                                      friction,
                                                      restitution,
                                                      userData);
        info("physics: created %s convex hull for entity %u (verts=%u)",
             isDynamic ? "dynamic" : "static",
             entity,
             positionCount / 3);

    } else if (strequals(shapeType, "MESH")) {
        physics->shapeType = PHYSICS_SHAPE_MESH;
        assert(positions && positionCount > 0 && indices && indexCount > 0 &&
               "MESH requires vertex and index data");
        // Note: Jolt MeshShape is always static — cannot be dynamic.
        physics->joltMesh =
            joltCreateMeshShapeNoCache(positions, positionCount, indices, indexCount, pos, rot, 1, userData);
        // info("physics: created static mesh for entity %u (verts=%u, indices=%u)",
        //      entity,
        //      positionCount / 3,
        //      indexCount);

    } else if (strequals(shapeType, "CONE")) {
        assert(false && "CONE rigid body shape is not supported — change it in Blender");

    } else {
        warn("physics: unknown rigid body shape '%s' for entity %u", shapeType, entity);
    }
}
