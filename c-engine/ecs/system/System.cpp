#include "ecs/system/System.h"
#include "ecs/Ecs.h"

void systemPreUpdate(struct System* system) {
    if (ecs.showStats) {
        system->cpuElapsedLastFrame = system->cpuElapsed;
        system->cpuElapsed          = 0;
        double start                = nanos();
        if (system->preUpdate) {
            system->preUpdate();
        }
        system->cpuElapsed += nanos() - start;
    } else {
        if (system->preUpdate) {
            system->preUpdate();
        }
    }
}

void systemUpdate(struct System* system) {
    if (ecs.showStats) {
        double start = nanos();
        if (system->update) {
            system->update();
        }
        system->cpuElapsed += nanos() - start;
    } else {
        if (system->update) {
            system->update();
        }
    }
}

void systemPostUpdate(struct System* system) {
    if (ecs.showStats) {
        double start = nanos();
        if (system->postUpdate) {
            system->postUpdate();
        }
        system->cpuElapsed += nanos() - start;
    } else {
        if (system->postUpdate) {
            system->postUpdate();
        }
    }
}
