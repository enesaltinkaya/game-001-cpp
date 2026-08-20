#include <assert.h>
#include <stdlib.h>
#include "ecs/Ecs.h"
#include "ecs/system/lua/LuaSystem.h"
#include "ecs/system/window/WindowSystem.h"
#include "futuretask/FutureTask.h"
#include "renderer/Renderer.h"
#include "timer/Timer.h"

volatile char engineRunning = 1;

static double engineStartNanos = 0;
static char logTimeoutEnabled = 0;

static struct System* gameSystem;

void engineSetGameSystem(struct System* system) {
    gameSystem = system;
}

void engineStart(void) {
    if (!gameSystem) {
        terminate("call engineSetGameSystem(...) before engineStart()");
    }

    char* logTimeoutEnv = getenv("ENGINE_LOG_TIMEOUT");
    if (logTimeoutEnv) {
        engineStartNanos = nanos() + atof(logTimeoutEnv) * MILLION;
        logTimeoutEnabled = 1;
    }

    ecsInit(gameSystem);

    do {
        timerBegin();
        futureTaskRun();
        ecsPreUpdate();
        ecsUpdate();
        ecsPostUpdate();
        timerEnd();

        {
            static int hitchOn = -1;
            if (hitchOn < 0) hitchOn = getenv("ENGINE_HITCH_DEBUG") != NULL;
            if (hitchOn && timer.elapsed > 20.0 * MILLION)
                info("HITCH: frame cpu %.1f ms (fps %.1f)", timer.elapsed / MILLION, timer.fps);
        }

        if (logTimeoutEnabled && nanos() > engineStartNanos) {
            info("ENGINE_LOG_TIMEOUT reached (%.0f ms) — shutting down", (nanos() - engineStartNanos) / MILLION);
            engineRunning = 0;
            break;
        }

    } while (engineRunning);

    windowSystemHide();
    gotoSleepMS(200);
    ecsDestroy();
    futureTaskFinish();
    luaDestroy();
    arrayFree(ecs.systems);
    arrayFree(renderer.passes);
}

void engineStop(void) {
    engineRunning = 0;
}
