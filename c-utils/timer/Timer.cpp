#include "Timer.h"
#include "Utils.h"
#include "logger/Logger.h"

#define UPS 60.

namespace utils {
static void calculateFps(void);
static double engineStartTime;
Timer timer;

void timerInit(double fpsLimit, bool fpsLimitChecked, bool busyLoop) {
    timer.fpsLimit        = fpsLimit;
    timer.fpsLimitChecked = fpsLimitChecked;
    timer.busyLoop        = busyLoop;

    static bool first = false;
    if (!first) {
        first = true;
        info("timer: initializing");
    }
    debug("timer: fpsLimit %.0lf", timer.fpsLimitChecked ? timer.fpsLimit : 0);

    if (engineStartTime == 0) engineStartTime = nanos();

    timer.ups     = UPS;
    timer.dt      = 1.F / UPS;
    timer.dtNanos = timer.dt * BILLION;
    timer.start   = nanos();
    timer.desired = timer.fpsLimit > 0 ? BILLION / timer.fpsLimit : 0.;
}

void timerBegin(void) {
    double now                  = nanos();
    timer.elapsedFull           = now - timer.start;
    timer.timeSinceStart        = now - engineStartTime;
    timer.timeSinceStartSeconds = (now - engineStartTime) / BILLION;
    calculateFps();

    timer.frameTime = now - timer.start;
    timer.start     = now;
    timer.next      = now + timer.desired;
    if (timer.frameTime > .250 * BILLION) {
        timer.frameTime = .250 * BILLION;
    }
    timer.accumulator += timer.frameTime;
}

void timerUpdate(FnVoid update) {
    while (timer.accumulator >= timer.dtNanos) {
        update();
        timer.accumulator -= timer.dtNanos;
    }
    timer.alpha = timer.accumulator / timer.dtNanos;
}

void timerEnd(void) {
    double now    = nanos();
    timer.elapsed = now - timer.start;
    timer.frameCounter++;

    if (timer.fpsLimitChecked) {
        if (timer.busyLoop) {
            while (nanos() < timer.next) {
            }
        } else {
            static double frameOverhead;
            double frameStart = now;
            double frameEnd   = timer.start;
            double sleepTime  = timer.desired - (frameStart - frameEnd);
            if (sleepTime > frameOverhead) {
                double adjustedSleep = sleepTime - frameOverhead;
                gotoSleepNS(adjustedSleep);
                frameOverhead = (nanos() - frameStart) - adjustedSleep;
                if (frameOverhead > timer.desired / 2) {
                    frameOverhead = 0;
                }
            }
        }
    }
}

static double fpsTotal, fpsCounter, lastFpsTime;

void calculateFps(void) {
    fpsTotal += timer.elapsedFull;
    fpsCounter++;
    double now = nanos();
    if (now > lastFpsTime + BILLION / 2.) {  // twice per second
        lastFpsTime = now;
        timer.fps   = (fpsCounter / fpsTotal) * BILLION;
        fpsCounter  = 0;
        fpsTotal    = 0;
    }
}
}  // namespace utils
