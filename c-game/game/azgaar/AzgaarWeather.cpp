#include "azgaar/AzgaarWeather.h"
#include "azgaar/AzgaarWorld.h"
#include "renderer/vulkan/pass/azgaar_weather/VulkanAzgaarWeatherPass.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "Utils.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

// ── State ───────────────────────────────────────────────────────────────────

static const AzgaarWorld* g_world       = nullptr;
static bool g_initialized               = false;
static bool g_debug                     = false;
static AzgaarWeatherCondition g_forced  = AZGAAR_WEATHER_NONE;  // env override
static bool g_forcedActive              = false;

static VulkanWeatherData g_current;      // live, cross-faded state
static VulkanWeatherData g_target;       // per-condition target
static AzgaarWeatherCondition g_condition = AZGAAR_WEATHER_NONE;
static bool g_reseeded                   = false;

static double g_lastSampleTime = -1e9;   // climate sample clock (500 ms)
static double g_lastFrameTime  = 0.0;    // cross-fade clock
static float g_gustDirX = 1.0f, g_gustDirZ = 0.0f, g_gustSpeed = 3.5f;

// Cross-fade rate: every scalar field moves toward its target at 0.25 / s,
// i.e. a full 0→1 transition takes ~4 s (plan D8).
#define AZGAAR_WEATHER_LERP_RATE 0.25f

// ── Helpers ─────────────────────────────────────────────────────────────────

static float approach(float cur, float target, float dt) {
    float d = target - cur;
    float step = AZGAAR_WEATHER_LERP_RATE * dt;
    if (d > step) d = step;
    if (d < -step) d = -step;
    return cur + d;
}

static void setVec4(vec4 dst, float x, float y, float z, float w) {
    dst[0] = x;
    dst[1] = y;
    dst[2] = z;
    dst[3] = w;
}

// Slow gusty wind around the authored base direction (winds[0]): the same
// value feeds the weather particles, the props sway and the water ripples.
static void updateGust(double t) {
    float deg = (g_world && g_world->winds[0] != 0.0f) ? g_world->winds[0] : 45.0f;
    float ang = deg * static_cast<float>(M_PI) / 180.0f + 0.12f * sinf(static_cast<float>(t) * 0.05f + 1.3f);
    float g1  = 0.5f + 0.5f * sinf(static_cast<float>(t) * 0.13f);
    float g2  = 0.5f + 0.5f * sinf(static_cast<float>(t) * 0.071f + 2.1f);
    g_gustDirX  = cosf(ang);
    g_gustDirZ  = sinf(ang);
    g_gustSpeed = 3.5f * (0.75f + 0.25f * (0.6f * g1 + 0.4f * g2));
}

static const char* conditionName(AzgaarWeatherCondition c) {
    switch (c) {
        case AZGAAR_WEATHER_SNOW:   return "snow";
        case AZGAAR_WEATHER_RAIN:   return "rain";
        case AZGAAR_WEATHER_DUST:   return "dust";
        case AZGAAR_WEATHER_LEAVES: return "leaves";
        default:                    return "none";
    }
}

// ── Condition target (climate → WeatherData) ───────────────────────────────

// The world-population plan's F table:
//   snowfall   temp < −1 °C
//   drizzle    temp < 3 °C && prec > 60
//   dust storm biome 1 && temp > 25 °C
//   leaves     biome 6
//   none       otherwise
static AzgaarWeatherCondition sampleCondition(float camX, float camZ) {
    if (!g_world) return AZGAAR_WEATHER_NONE;

    AzgaarClimateSample s = {};
    float mpp = static_cast<float>(g_world->metersPerPixel);
    if (mpp <= 0.0f) mpp = 1.0f;
    float mapX = static_cast<float>(g_world->widthPx) * 0.5f - camX / mpp;
    float mapY = static_cast<float>(g_world->heightPx) * 0.5f - camZ / mpp;
    azgaarWorldSampleClimate(g_world, mapX, mapY, &s);

    if (s.temperature < -1.0f) return AZGAAR_WEATHER_SNOW;
    if (s.temperature < 3.0f && s.precipitation > 60.0f) return AZGAAR_WEATHER_RAIN;
    if (s.biome == 1u && s.temperature > 25.0f) return AZGAAR_WEATHER_DUST;
    if (s.biome == 6u) return AZGAAR_WEATHER_LEAVES;
    return AZGAAR_WEATHER_NONE;
}

static void buildTarget(AzgaarWeatherCondition c, float camX, float camZ) {
    VulkanWeatherData t = g_target;

    // Types / density / box / fade / turbulence per the plan's parameter
    // table.  NONE keeps the type weights (cross-fade-out: old-type
    // particles finish falling as their spawn type) but fades opacity and
    // density to zero.
    switch (c) {
        case AZGAAR_WEATHER_SNOW:
            setVec4(t.types, 1.0f, 0.0f, 0.0f, 0.0f);
            setVec4(t.params, 90.0f, 30.0f, 1.00f, 1.0f);
            t.look[0] = 1.0f; t.look[2] = 60.0f;
            t.wind[3] = 0.9f;
            setVec4(t.tint, 0.92f, 0.95f, 1.00f, 0.0f);
            break;
        case AZGAAR_WEATHER_RAIN:
            setVec4(t.types, 0.0f, 1.0f, 0.0f, 0.0f);
            setVec4(t.params, 90.0f, 30.0f, 0.50f, 1.0f);
            t.look[0] = 0.9f; t.look[2] = 55.0f;
            t.wind[3] = 0.2f;
            setVec4(t.tint, 0.65f, 0.72f, 0.85f, 0.0f);
            break;
        case AZGAAR_WEATHER_DUST: {
            setVec4(t.types, 0.0f, 0.0f, 1.0f, 0.0f);
            setVec4(t.params, 90.0f, 12.0f, 0.30f, 1.0f);  // flattened box
            t.look[0] = 1.0f; t.look[2] = 45.0f;
            t.wind[3] = 0.6f;
            // Dust is tinted by the local biome colour (desert sand).
            float biome[3] = {0.76f, 0.66f, 0.46f};
            if (g_world) {
                float mpp = static_cast<float>(g_world->metersPerPixel);
                if (mpp <= 0.0f) mpp = 1.0f;
                float mapX = static_cast<float>(g_world->widthPx) * 0.5f - camX / mpp;
                float mapY = static_cast<float>(g_world->heightPx) * 0.5f - camZ / mpp;
                azgaarWorldSampleBiomeColorSmooth(g_world, mapX, mapY, biome);
            }
            setVec4(t.tint, biome[0], biome[1], biome[2], 0.0f);
            break;
        }
        case AZGAAR_WEATHER_LEAVES:
            setVec4(t.types, 0.0f, 0.0f, 0.0f, 1.0f);
            setVec4(t.params, 90.0f, 25.0f, 0.08f, 1.0f);
            t.look[0] = 1.0f; t.look[2] = 55.0f;
            t.wind[3] = 0.7f;
            setVec4(t.tint, 0.72f, 0.48f, 0.22f, 0.0f);  // autumn base
            break;
        default:  // AZGAAR_WEATHER_NONE
            t.params[2] = 0.0f;   // density → 0 (respawns come back disabled)
            t.look[0]   = 0.0f;   // opacity → 0
            t.wind[3]   = 0.3f;
            break;
    }
    t.look[1] = 1.0f;  // fall speed scale (per-type speeds are absolute)

    g_target = t;
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

void azgaarWeatherInit(const AzgaarWorld* world) {
    if (!world) return;
    azgaarWeatherDestroy();

    g_world       = world;
    g_debug       = getenv("ENGINE_AZGAAR_WEATHER_DEBUG") != nullptr;
    g_forced      = AZGAAR_WEATHER_NONE;
    g_forcedActive = false;

    // ENGINE_AZGAAR_WEATHER=0..4 forces none/snow/rain/dust/leaves and
    // bypasses the climate logic entirely.
    const char* forcedEnv = getenv("ENGINE_AZGAAR_WEATHER");
    if (forcedEnv && *forcedEnv) {
        int v = atoi(forcedEnv);
        if (v >= 0 && v <= 4) {
            g_forced       = static_cast<AzgaarWeatherCondition>(v);
            g_forcedActive = true;
        }
    }

    // Start from the engine's disabled default; the first condition sample
    // (immediately on the first update) builds the target and the 4 s
    // cross-fade brings the field in.
    g_current = vulkanResourceGetWeatherData();
    g_current.look[0] = 0.0f;  // fade opacity in
    g_current.look[3] = 0.0f;
    g_current.params[2] = 0.0f;
    memset(g_current.types, 0, sizeof(g_current.types));

    g_condition     = AZGAAR_WEATHER_NONE;
    g_reseeded      = false;
    g_lastSampleTime = -1e9;
    g_lastFrameTime  = timer.timeSinceStart / BILLION;  // nanos -> seconds

    vulkanResourceSetWeather(&g_current);
    g_initialized = true;

    info("azgaarWeather: init (%s)%s",
         conditionName(g_forced),
         g_forcedActive ? " [forced]" : "");
}

void azgaarWeatherUpdate(float camX, float camY, float camZ) {
    static_cast<void>(camY);
    if (!g_initialized) return;

    // timer.timeSinceStart counts NANOseconds; everything below (gust
    // sines, dt clamp, 500 ms sampling) assumes seconds.
    double now = timer.timeSinceStart / BILLION;
    float dt = static_cast<float>(now - g_lastFrameTime);
    g_lastFrameTime = now;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.1f) dt = 0.1f;

    // Climate → condition every 500 ms (or forced).
    if (now - g_lastSampleTime >= 0.5) {
        g_lastSampleTime = now;
        AzgaarWeatherCondition c = g_forcedActive ? g_forced : sampleCondition(camX, camZ);
        if (c != g_condition || !g_reseeded) {
            if (c != g_condition) {
                info("azgaarWeather: condition %s -> %s",
                     conditionName(g_condition), conditionName(c));
            }
            g_condition = c;
            buildTarget(c, camX, camZ);

            // First real activation: re-seed the GPU pool with the target
            // type mix / density so the field is correct as it appears (the
            // boot-time seed is a placeholder mix).  Snap the faded-in fields
            // to their target too — nothing was on screen before, so there is
            // no pop to smooth; the 4 s cross-fade is for condition CHANGES.
            if (!g_reseeded && c != AZGAAR_WEATHER_NONE) {
                vulkanAzgaarWeatherReseed(&g_target);
                memcpy(g_current.types, g_target.types, sizeof(g_current.types));
                memcpy(g_current.params, g_target.params, sizeof(g_current.params));
                g_current.look[0] = g_target.look[0];
                g_current.look[2] = g_target.look[2];
                g_current.wind[3] = g_target.wind[3];
                memcpy(g_current.tint, g_target.tint, sizeof(g_current.tint));
                g_current.look[3] = 1.0f;
                g_reseeded = true;
            }
        } else if (c == AZGAAR_WEATHER_DUST) {
            // Dust tint tracks the local biome colour while active.
            buildTarget(c, camX, camZ);
        }
    }

    // Shared gust (weather particles / props sway / water ripples).
    updateGust(now);

    // Cross-fade every scalar toward its target at 0.25 / s.
    VulkanWeatherData cur = g_current;
    for (int i = 0; i < 4; i++) {
        cur.types[i]  = approach(cur.types[i],  g_target.types[i],  dt);
        cur.params[i] = approach(cur.params[i], g_target.params[i], dt);
    }
    cur.look[0] = approach(cur.look[0], g_target.look[0], dt);  // opacity
    cur.look[1] = approach(cur.look[1], g_target.look[1], dt);  // fall scale
    cur.look[2] = approach(cur.look[2], g_target.look[2], dt);  // far fade
    cur.wind[3] = approach(cur.wind[3], g_target.wind[3], dt);  // turbulence
    for (int i = 0; i < 3; i++) {
        cur.tint[i] = approach(cur.tint[i], g_target.tint[i], dt);
    }

    // Wind from the shared gust (smooth by construction, not lerped).
    cur.wind[0] = g_gustDirX;
    cur.wind[1] = g_gustDirZ;
    cur.wind[2] = g_gustSpeed;

    // Enabled: on while a condition is active; stays on during the fade-out
    // so old-type particles can finish falling, then the pass early-outs.
    if (g_condition != AZGAAR_WEATHER_NONE) {
        cur.look[3] = 1.0f;
    } else {
        cur.look[3] = (cur.look[0] > 0.02f) ? 1.0f : 0.0f;
    }

    g_current = cur;
    vulkanResourceSetWeather(&g_current);

    if (g_debug) {
        static double lastLog = -10.0;
        if (now - lastLog >= 2.0) {
            lastLog = now;
            info("azgaarWeather: cond=%s types=(%.2f %.2f %.2f %.2f) dens=%.2f "
                 "opac=%.2f wind=(%.2f %.2f @ %.1f m/s)",
                 conditionName(g_condition),
                 static_cast<double>(cur.types[0]), static_cast<double>(cur.types[1]),
                 static_cast<double>(cur.types[2]), static_cast<double>(cur.types[3]),
                 static_cast<double>(cur.params[2]), static_cast<double>(cur.look[0]),
                 static_cast<double>(cur.wind[0]), static_cast<double>(cur.wind[1]), static_cast<double>(cur.wind[2]));
        }
    }
}

void azgaarWeatherDestroy(void) {
    if (!g_initialized) return;
    VulkanWeatherData defaultWeather = {
        .wind   = {1.0f, 0.0f, 3.5f, 0.5f},
        .types  = {0.0f, 0.0f, 0.0f, 0.0f},
        .params = {90.0f, 30.0f, 0.0f, 1.0f},
        .look   = {0.0f, 1.0f, 60.0f, 0.0f},  // disabled
        .tint   = {1.0f, 1.0f, 1.0f, 0.0f},
    };
    vulkanResourceSetWeather(&defaultWeather);
    g_initialized = false;
    g_world = nullptr;
    g_condition = AZGAAR_WEATHER_NONE;
    g_reseeded = false;
}

bool azgaarWeatherGetWind(float* outDirX, float* outDirZ, float* outSpeed) {
    if (!g_initialized) return false;
    if (outDirX)  *outDirX = g_gustDirX;
    if (outDirZ)  *outDirZ = g_gustDirZ;
    if (outSpeed) *outSpeed = g_gustSpeed;
    return true;
}

AzgaarWeatherCondition azgaarWeatherGetCondition(void) {
    return g_condition;
}
