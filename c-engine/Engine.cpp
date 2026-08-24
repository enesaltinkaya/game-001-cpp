#include <assert.h>
#include <stdlib.h>
#include "ecs/Ecs.h"
#include "ecs/system/lua/LuaSystem.h"
#include "ecs/system/window/WindowSystem.h"
#include "futuretask/FutureTask.h"
#include "renderer/Renderer.h"
#include "timer/Timer.h"

namespace engine {
volatile char engineRunning = 1;

static double engineStartNanos = 0;
static char logTimeoutEnabled = 0;

static System* gameSystem;

void engineSetGameSystem(System* system) {
    gameSystem = system;
}

void engineStart(void) {
    if (!gameSystem) {
        utils::terminate("call engineSetGameSystem(...) before engineStart()");
    }

    char* logTimeoutEnv = getenv("ENGINE_LOG_TIMEOUT");
    if (logTimeoutEnv) {
        engineStartNanos = utils::nanos() + atof(logTimeoutEnv) * MILLION;
        logTimeoutEnabled = 1;
    }

    ecsInit(gameSystem);

    do {
        utils::timerBegin();
        utils::futureTaskRun();
        ecsPreUpdate();
        ecsUpdate();
        ecsPostUpdate();
        utils::timerEnd();

        {
            static int hitchOn = -1;
            if (hitchOn < 0) hitchOn = getenv("ENGINE_HITCH_DEBUG") != NULL;
            if (hitchOn && utils::timer.elapsed > 20.0 * MILLION)
                utils::info("HITCH: frame cpu %.1f ms (fps %.1f)", utils::timer.elapsed / MILLION, utils::timer.fps);
        }

        if (logTimeoutEnabled && utils::nanos() > engineStartNanos) {
            utils::info("ENGINE_LOG_TIMEOUT reached (%.0f ms) — shutting down", (utils::nanos() - engineStartNanos) / MILLION);
            engineRunning = 0;
            break;
        }

    } while (engineRunning);

    windowSystemHide();
    utils::gotoSleepMS(200);
    ecsDestroy();
    utils::futureTaskFinish();
    luaDestroy();
}

void engineStop(void) {
    engineRunning = 0;
}
}  // namespace engine
