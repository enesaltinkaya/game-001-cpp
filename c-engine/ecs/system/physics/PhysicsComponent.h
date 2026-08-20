#pragma once

#include "ecs/system/scene/SceneSystem.h"

typedef struct JoltMesh JoltMesh;
typedef struct JoltBody JoltBody;

REGISTER_COMPONENT(Physics);

typedef enum PhysicsShapeType {
    PHYSICS_SHAPE_MESH,
    PHYSICS_SHAPE_SPHERE,
    PHYSICS_SHAPE_BOX,
    PHYSICS_SHAPE_CAPSULE,
    PHYSICS_SHAPE_CYLINDER,
    PHYSICS_SHAPE_CONVEX_HULL,
} PhysicsShapeType;

typedef struct Physics {
    PhysicsShapeType shapeType;
    bool isDynamic;
    union {
        JoltMesh* joltMesh;
        JoltBody* joltBody;
    };
} Physics;
