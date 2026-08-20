#pragma once

typedef struct Transform Transform;
typedef struct Entity Entity;
typedef struct System System;
typedef struct Scene Scene;

extern System transformSystem;


// entities that are activated, will have their transforms
// calculated (parent/interpolation) and uploaded to gpu
// considered active for 2 seconds
// transformSaveLast activates entity
void transformActivate(Scene* scene, u32 entity);
void transformSaveLast(Scene* scene, u32 entity);

// Optimized: activate + save last transforms for entire subtree in one pass
// Avoids redundant parent/child walks that transformSaveLast does per-node
void transformActivateAndSaveLastSubtree(Scene* scene, u32 entity);

struct WorldTransform* transformGetWorld(Scene* scene, u32 entity);
void transformGetDirection(Scene* scene, u32 entity, float* out);

void transformQuatToPitchYaw(versor q, float* pitch, float* yaw);
