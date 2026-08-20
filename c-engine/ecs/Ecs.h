#pragma once

typedef struct System System;
typedef struct Scene Scene;
typedef struct Entity Entity;

typedef struct Ecs {
    Array(System*) systems;

    Array(Scene*) scenes;
    Scene* defaultScene;
    Array(SparseSet*) components;

    char showStats;
    double totalCpuElapsed;
    double totalCpuElapsedTemp;
} Ecs;

extern struct Ecs ecs;

void ecsInit(System* gameSystem);
void ecsDestroy(void);
void ecsPreUpdate(void);
void ecsUpdate(void);
void ecsPostUpdate(void);

/*
SYSTEM
*/
void systemAdd(int order, System* system);     // adds system next frame
void systemAddNow(int order, System* system);  // adds system immediately
void systemRemove(System* system);             // removes system next frame
