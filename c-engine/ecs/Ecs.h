#pragma once

namespace engine {
class System;
struct Scene;
struct Entity;

struct Ecs {
    std::vector<System*> systems;

    std::vector<Scene*> scenes;
    Scene* defaultScene;
    std::vector<utils::SparseSet*> components;

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
}  // namespace engine
