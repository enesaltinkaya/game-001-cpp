#pragma once

struct System;
struct Scene;
struct Entity;

struct Ecs {
    Array(System*) systems;

    Array(Scene*) scenes;
    Scene* defaultScene;
    Array(SparseSet*) components;

    bool showStats;
    double totalCpuElapsed;
    double totalCpuElapsedTemp;
};

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
