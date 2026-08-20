#pragma once

namespace utils {
struct Timer {
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

    bool busyLoop;
    bool fpsLimitChecked;
    u64 frameCounter;
};

extern Timer timer;
typedef void (*FnVoid)(void);

void timerInit(double fpsLimit, bool fpsLimitChecked, bool busyLoop);
void timerBegin(void);
void timerUpdate(FnVoid update);
void timerEnd(void);
}  // namespace utils
