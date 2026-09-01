#include <algorithm>
#include <assert.h>
#include <cmath>
#include <fstream>
#include <stdlib.h>
#include <vector>
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

    // Opt-in frame-time recorder: per-frame cost is exactly three field reads
    // and one push_back into a pre-reserved vector. No I/O, no logging inside
    // the loop; the single file write happens after the loop exits.
    struct FrameTimeSample {
        u64 timeSinceStartSeconds;
        double frameTime;  // wall time since previous frame (ns), clamped at 250 ms by Timer
        double elapsed;    // CPU+GPU work in this frame's timer window (ns)
    };
    char* frameTimesLogEnv = getenv("ENGINE_FRAME_TIMES_LOG");
    bool frameTimesLogging = frameTimesLogEnv != nullptr;
    std::vector<FrameTimeSample> frameTimeSamples;
    if (frameTimesLogging) frameTimeSamples.reserve(8192);

    do {
        utils::timerBegin();
        utils::futureTaskRun();
        ecsPreUpdate();
        ecsUpdate();
        ecsPostUpdate();
        utils::timerEnd();

        if (frameTimesLogging)
            frameTimeSamples.push_back({utils::timer.timeSinceStartSeconds, utils::timer.frameTime, utils::timer.elapsed});

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

    // Single blocking write of everything recorded (kept out of the loop on
    // purpose: no I/O may touch the frame timing being measured).
    if (frameTimesLogging) {
        std::ofstream out(frameTimesLogEnv);
        if (!out.is_open()) {
            utils::error("ENGINE_FRAME_TIMES_LOG: cannot open '%s' for writing", frameTimesLogEnv);
        } else {
            out << "# time_offset_s frame_time_ms cpu_work_ms\n";
            for (const FrameTimeSample& s : frameTimeSamples)
                out << s.timeSinceStartSeconds << ' ' << s.frameTime / MILLION << ' ' << s.elapsed / MILLION << '\n';
            out << "# count " << frameTimeSamples.size() << '\n';
            out.close();

            auto summarize = [](const char* name, const std::vector<FrameTimeSample>& samples, bool useFrameTime) {
                std::vector<double> v;
                v.reserve(samples.size());
                for (const FrameTimeSample& s : samples) v.push_back(useFrameTime ? s.frameTime : s.elapsed);
                if (v.empty()) return;
                std::sort(v.begin(), v.end());
                double sum = 0.;
                for (double x : v) sum += x;
                double mean = sum / v.size();
                double var = 0.; for (double x : v) var += (x - mean) * (x - mean);
                auto pct = [&](double p) { return v[(size_t)(p / 100.0 * (v.size() - 1))]; };
                utils::info("FRAME_TIMES %s: n=%zu mean=%.3f ms stdev=%.3f ms p50=%.3f ms p95=%.3f ms p99=%.3f ms max=%.3f ms",
                            name, v.size(), mean / MILLION, std::sqrt(var / v.size()) / MILLION,
                            pct(50.) / MILLION, pct(95.) / MILLION, pct(99.) / MILLION, v.back() / MILLION);
            };
            summarize("frameTime", frameTimeSamples, true);
            summarize("cpuWork", frameTimeSamples, false);
            utils::info("FRAME_TIMES: wrote %zu samples to %s", frameTimeSamples.size(), frameTimesLogEnv);
        }
    }

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
