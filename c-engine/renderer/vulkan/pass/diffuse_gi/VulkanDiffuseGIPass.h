#pragma once

#include "ecs/system/System.h"

namespace engine {
struct VulkanImage;

/* Cheap screen-space GI via edge-aware colour diffusion (Petersen,
 * "Simplified Diffusion for Real-Time GI").  Reads the directly-lit scene
 * colour and blurs it with a wide separable edge-aware gaussian (depth
 * edge mask) so light bleeds across surfaces but stops at occluders.
 * The composite pass adds (diffused - direct) * strength — sign-preserving,
 * so the bleed tints bright surfaces (grass green onto a wall) as well as
 * light-leaks into dark ones.  A temporal filter reprojects last frame's
 * field with the TAA motion vectors (see below) so camera motion does not
 * cause TAA shimmer.
 *
 * Debug knobs (env vars): ENGINE_GI_DISABLED=1, ENGINE_GI_ITER=<n>
 * (1..8 H+V separable iterations, default 2), ENGINE_GI_STRENGTH=<f> (blend
 * toward the diffused image, 0..1, default 0.4), ENGINE_GI_RES=<f> (GI
 * buffer scale vs render resolution, default 0.25 — the diffusion is
 * low-frequency and the composite upsamples linearly), ENGINE_GI_RADIUS=<f>
 * (gaussian sigma in render-resolution px, default 20), ENGINE_GI_DEPTH_EDGE
 * =<f> (relative inv-depth edge), ENGINE_GI_NDOT_MIN / ENGINE_GI_NDOT_MAX
 * (normal-dot edge window; NDOT_MIN <= -1 disables the gate, which is the
 * default — the grass-colour climbing a wall is the intended look),
 * ENGINE_GI_TEMPORAL=<f> (temporal history blend, 0..1, default 0.75 —
 * 0 disables the temporal filter), ENGINE_GI_DEPTH_THRESH=<f> (relative
 * inverse-depth temporal rejection threshold, default 0.05),
 * ENGINE_GI_GHOST=<f> (relative colour rejection threshold for history
 * lying outside the neighbourhood colour range, default 0.15). The temporal
 * filter reprojects last frame's GI field with the same motion vectors
 * TAA uses; without it, camera motion makes the screen-aligned spatial
 * blur wiggle frame-to-frame and TAA shimmers on grass. Its rejections
 * mirror taa.comp's cutout-edge-aware machinery (neighbourhood depth-span
 * and colour-AABB comparison plus AABB clamp) and its motion-confidence
 * falloff is a wide safety net (64->512 px) only for extreme camera whips —
 * while running, full-res MVs routinely exceed 30 px, so a tight curve would
 * zero the history exactly while the camera moves (the grass-shimmer case). */
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
