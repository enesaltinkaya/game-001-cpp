#pragma once

typedef struct System {
    const char* name;
    FnVoid added;
    FnVoid removed;
    FnVoid preUpdate;
    FnVoid update;
    FnVoid postUpdate;

    double cpuElapsedLastFrame;
    double cpuElapsed;
    double gpuElapsed;

    i32 priority;
} System;

void systemPreUpdate(struct System* system);
void systemUpdate(struct System* system);
void systemPostUpdate(struct System* system);
