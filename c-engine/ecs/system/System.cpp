#include "ecs/system/System.h"
#include "ecs/Ecs.h"

namespace engine {
void systemPreUpdate(System* system) {
    if (ecs.showStats) {
        system->cpuElapsedLastFrame = system->cpuElapsed;
        system->cpuElapsed          = 0;
        double start                = utils::nanos();
        system->preUpdate();
        system->cpuElapsed += utils::nanos() - start;
    } else {
        system->preUpdate();
    }
}

void systemUpdate(System* system) {
    if (ecs.showStats) {
        double start = utils::nanos();
        system->update();
        system->cpuElapsed += utils::nanos() - start;
    } else {
        system->update();
    }
}

void systemPostUpdate(System* system) {
    if (ecs.showStats) {
        double start = utils::nanos();
        system->postUpdate();
        system->cpuElapsed += utils::nanos() - start;
    } else {
        system->postUpdate();
    }
}
}  // namespace engine