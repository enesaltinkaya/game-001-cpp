#pragma once

#include "ecs/system/System.h"

namespace engine {
struct VulkanImage;

/* Cheap screen-space GI via edge-aware colour diffusion (Petersen,
 * "Simplified Diffusion for Real-Time GI").  Reads the directly-lit scene
 * colour and blurs it N iterations with a depth/normal edge mask so light
 * bleeds along surfaces but stops at occluders and across surface
 * orientation changes.  The composite pass adds (diffused - direct),
 * clamped non-negative per channel, scaled by the strength — a pure bounce
 * term on top of untouched direct lighting.
 *
 * Debug knobs (env vars): ENGINE_GI_DISABLED=1, ENGINE_GI_ITER=<n>
 * (1..8 H+V separable iterations, default 2), ENGINE_GI_STRENGTH=<f> (blend
 * toward the diffused image, 0..1, default 0.4), ENGINE_GI_RES=<f> (GI
 * buffer scale vs render resolution, default 0.25 — the diffusion is
 * low-frequency and the composite upsamples linearly), ENGINE_GI_RADIUS=<f>
 * (gaussian sigma in render-resolution px, default 20), ENGINE_GI_DEPTH_EDGE
 * =<f> (relative inv-depth edge), ENGINE_GI_NDOT_MIN / ENGINE_GI_NDOT_MAX
 * (normal-dot edge window; NDOT_MIN <= -1 disables the gate, which is the
 * default — the grass-colour climbing a wall is the intended look). */
class VulkanDiffuseGIPass : public System {
public:
    VulkanDiffuseGIPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern VulkanDiffuseGIPass vulkanDiffuseGIPass;

void  vulkanDiffuseGIPassSetDisabled(char disabled);
char  vulkanDiffuseGIPassIsDisabled(void);
float vulkanDiffuseGIPassGetStrength(void);
/// Diffused scene colour (R16G16B16A16_SFLOAT); NULL before the first
/// swapchain exists or while disabled.
struct VulkanImage* vulkanDiffuseGIPassGetOutput(void);
}  // namespace engine
