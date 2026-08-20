#pragma once

#include "azgaar/AzgaarWorld.h"
#include <stdbool.h>

// ── Azgaar weather: CPU side (plans/azgaar-weather-gpu-particles.md D8) ──
// Climate→condition state machine driving the GPU particle weather pass.
// Every 500 ms the climate at the camera is sampled (or ENGINE_AZGAAR_
// WEATHER forces a condition) and a target VulkanWeatherData is built;
// every field is then cross-faded toward it at ~0.25 / s (≈ 4 s
// transitions), so weather changes never pop: old-type particles finish
// falling as their spawn type while new spawns arrive with the new type.
//
// Wind coherence: a slow CPU gust (winds[0] + noise) is computed here and
// shared by the weather particles, the props sway (AzgaarPropsData.wind)
// and the water ripples (WaterData.windAngle) — see azgaarWeatherGetWind.
//
// Lifecycle:
//   azgaarWeatherInit(world)              — once, after the world is loaded
//   azgaarWeatherUpdate(camX, camY, camZ) — every frame with the camera pos
//   azgaarWeatherDestroy()                — on teardown

namespace game {
enum AzgaarWeatherCondition {
    AZGAAR_WEATHER_NONE   = 0,
    AZGAAR_WEATHER_SNOW   = 1,
    AZGAAR_WEATHER_RAIN   = 2,
    AZGAAR_WEATHER_DUST   = 3,
    AZGAAR_WEATHER_LEAVES = 4,
};

void azgaarWeatherInit(const AzgaarWorld* world);
void azgaarWeatherUpdate(float camX, float camY, float camZ);
void azgaarWeatherDestroy(void);

// The shared gusty wind (unit direction + speed m/s), driven by winds[0]
// plus slow noise.  Returns false when the module is inactive (callers
// fall back to their own defaults).  One source so flakes, grass and
// waves stay coherent.
bool azgaarWeatherGetWind(float* outDirX, float* outDirZ, float* outSpeed);

// Debug (ENGINE_AZGAAR_WEATHER_DEBUG): condition + cross-fade state.
AzgaarWeatherCondition azgaarWeatherGetCondition(void);
}  // namespace game
