#pragma once

#include "ecs/system/System.h"

namespace engine {
struct VulkanImage;

/* Ambient occlusion via AMD FidelityFX CACAO (depth-based; normals are
 * reconstructed from depth — the engine's normal buffer is oct-encoded and
 * incompatible with CACAO's affine unpack) followed by a dedicated temporal
 * accumulation pass (ao_temporal.comp): CACAO v1.4 has no internal temporal
 * filter, and the color TAA pass cannot average its spatially correlated
 * (half-res block) noise, so the AO gets its own reprojection history.
 *
 * Debug knobs (env vars): ENGINE_AO_DISABLED=1, ENGINE_AO_TEMPORAL=0,
 * ENGINE_AO_TWEIGHT / _TDEPTH / _TCLAMP / _TFLOOR (temporal filter tuning),
 * ENGINE_AO_ANGLE_OFF=1 (disable kernel rotation), ENGINE_AO_DETAIL=<f>,
 * ENGINE_AO_QUALITY=<0-4>, ENGINE_AO_RADIUS / _STRENGTH / _POWER / _CLAMP
 * (CACAO shape overrides). */
class VulkanAOPass : public System {
public:
    VulkanAOPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern VulkanAOPass vulkanAOPass;

void  vulkanAOPassSetDisabled(char disabled);
char  vulkanAOPassIsDisabled(void);
/// Runtime override of CACAO's strength (settings.shadowMultiplier).
/// Takes precedence over the per-frame ENGINE_AO_STRENGTH env read in
/// cacaoUpdate() — a one-shot assignment there would be clobbered within
/// one frame, so the override lives in a member the env read defers to.
/// Pass a negative value to clear the override (env/default wins again).
/// The GI pass no longer installs an override (its D5 attenuation was
/// measured a no-op — plans/ssgi-halo.md); the API remains for manual A/B
/// runs.
void  vulkanAOPassSetStrength(float strength);
/// Current-frame temporally-accumulated AO (R16G16B16A16_SFLOAT, .r =
/// occlusion, 1 = unoccluded).  Sampled by the composite pass; NULL before
/// the first swapchain exists or while disabled.
struct VulkanImage* vulkanAOPassGetOutput(void);
}  // namespace engine