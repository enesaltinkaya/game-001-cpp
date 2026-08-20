#pragma once

#include "ecs/system/System.h"
#include "renderer/vulkan/resources/VulkanBuffer.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"

/*
 * VulkanAzgaarWeatherPass
 * -----------------------
 * GPU-simulated weather particles (snow / rain / dust / leaves) —
 * plans/azgaar-weather-gpu-particles.md.  One pass, two stages in a single
 * command buffer:
 *
 *   1. weather_update.comp integrates the persistent device-local particle
 *      buffer in place (camera-following wrap volume, analytic fall + wind
 *      + turbulence, ground collision against this frame's scene depth,
 *      respawn with climate-rolled type + density roulette),
 *   2. a barrier makes the compute writes visible to the vertex shader,
 *   3. one instanced draw renders the buffer as procedural billboards /
 *      rain streaks into the scene colour target (water-pass transparent
 *      recipe: colour-only, blend, no depth write — occlusion comes from
 *      the soft-particle depth fade in the fragment shader).
 *
 * All tuning data (wind, spawn-type weights, box size, density, opacity,
 * tint) lives in WeatherData in the SceneBuffer (vulkanResourceSetWeather);
 * push constants only carry the mechanical bits (buffer address, depth
 * texture index, counts).  The whole pass early-outs while
 * WeatherData.look.w < 0.5, so non-Azgaar scenes pay nothing.
 */

namespace engine {
class VulkanAzgaarWeatherPass : public System {
public:
    VulkanAzgaarWeatherPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
};

extern VulkanAzgaarWeatherPass vulkanAzgaarWeatherPass;

// Re-seed the particle buffer from `weather` (game-side initial state):
// positions distributed through the wrap volume, spawn types rolled from
// the current weights, density roulette applied — so the first frame of a
// new condition has the right type mix / density instead of the boot-time
// placeholder.  Thread-safe (queued, consumed on the render thread).
// `weather` may be NULL to re-seed with the boot-time placeholder mix.
void vulkanAzgaarWeatherReseed(const VulkanWeatherData* weather);

// Per-frame particle coverage masks (R8, render resolution), written by
// this pass so the TAA pass can drop temporal history under weather
// particles.  Fast-moving particles (leaves / rain) otherwise leave a
// ~20-frame ghost chain behind them, because the TAA anti-ghost rejection
// is gated on camera motion and stays off for a static camera.
//
// `vulkanAzgaarWeatherPassGetMask` returns the mask written by the most
// recent weather update (this frame — TAA runs after this pass); `...Prev`
// the one before it (previous frame).  Both are NULL until the pass has a
// render target; callers must treat a NULL pair as "no mask".
VulkanImage* vulkanAzgaarWeatherPassGetMask(void);
VulkanImage* vulkanAzgaarWeatherPassGetPrevMask(void);
}  // namespace engine
