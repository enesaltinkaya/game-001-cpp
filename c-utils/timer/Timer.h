#pragma once

typedef struct Timer {
    double fps;
    double ups;
    double fpsLimit;

    double timeSinceStart;
    u64 timeSinceStartSeconds;

    double start;
    double next;
    double elapsed;
    double elapsedFull;
    double frameTime;
    double desired;

    float dt, dtNanos;
    double alpha;
    double accumulator;

    char busyLoop;
    char fpsLimitChecked;
    u64 frameCounter;
} Timer;

extern Timer timer;
typedef void (*FnVoid)(void);

void timerInit(double fpsLimit, char fpsLimitChecked,char busyLoop);
void timerBegin(void);
void timerUpdate(FnVoid update);
void timerEnd(void);
