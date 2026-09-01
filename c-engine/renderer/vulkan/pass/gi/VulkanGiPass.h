#pragma once

#include "ecs/system/System.h"

namespace engine {
struct VulkanImage;

/* Screen-space diffuse global illumination (SSGI / SSGI++ — see
 * plans/ssgi.md).  Per-texel hemisphere ray march against the depth buffer
 * (gi_estimate.comp, half internal resolution) writing a cosine-weighted
 * irradiance estimate (rgb = irradiance of the hit/miss radiance, a =
 * confidence); a dedicated temporal filter (gi_temporal.comp, full internal
 * resolution, ping-pong history) stabilises it before it is mixed into the
 * ambient term of scene.frag / azgaar_props.frag (AO-pass structural
 * pattern).
 *
 * Consumer wiring: the previous frame's temporal output is published into
 * the scene buffer every frame (sceneBuffer.gi — one frame of latency, the
 * scene/props passes run before this pass in the pass list) with the
 * 0xFFFFFFFFu absent-sentinel for startup/resize/disable.  While GI runs,
 * the CACAO strength is attenuated through vulkanAOPassSetStrength() to
 * avoid double darkening.  Ships behind the "giDisabled" settings key
 * (on by default; Phase 4 validated); ENGINE_GI_ENABLED=1 forces it on
 * over the setting for A/B runs.
 *
 * Debug knobs (env vars): ENGINE_GI_DISABLED=1 (skips the dispatch entirely
 * — images are not even created), ENGINE_GI_TEMPORAL=0 (raw estimate),
 * ENGINE_GI_RAYS=<4-8>, ENGINE_GI_DIST_SCALE, ENGINE_GI_TWEIGHT/_TDEPTH/
 * _TCLAMP/_TFLOOR/_TDEV0/_TDEV1/_TLUMA (temporal filter tuning),
 * ENGINE_GI_INTENSITY, ENGINE_GI_AO_SCALE. */
class VulkanGiPass : public System {
public:
    VulkanGiPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern VulkanGiPass vulkanGiPass;

void  vulkanGiPassSetDisabled(char disabled);
char  vulkanGiPassIsDisabled(void);
/// Temporally-accumulated GI history (R16G16B16A16_SFLOAT, rgb = filtered
/// irradiance, a = confidence; full internal resolution, ping-pong).  Raw
/// half-res estimate when the temporal filter is off (ENGINE_GI_TEMPORAL=0).
/// NULL before the first swapchain exists or while disabled (consumers
/// convert NULL to the 0xFFFFFFFFu absent-sentinel index).
struct VulkanImage* vulkanGiPassGetOutput(void);
/// Raw per-frame half-res estimate (R16G16B16A16_SFLOAT, rgb = irradiance,
/// a = confidence) — the ray-debugging view, independent of the temporal
/// filter.  NULL while disabled / before the first dispatch.
struct VulkanImage* vulkanGiPassGetEstimate(void);
}  // namespace engine
