#pragma once

#include "ecs/system/System.h"

namespace engine {
struct VulkanImage;

/* Full-resolution, temporally-filtered sun-shadow mask built from the raster
 * (CSM) half of AMD FFX HybridShadows.
 *
 * Pipeline (raster-only — the RT work-queue is allocated but never dispatched):
 *   1. FFX Classifier (shadow mode, classify-by-cascades): per 8x4 tile it
 *      classifies the CSM depth test into {definitely lit, definitely
 *      shadowed, indeterminate}, writes the per-tile ray-hit result to a
 *      tile-resolution R32 UAV, and queues indeterminate tiles in a work
 *      queue (dead code in the raster path — the queue is never consumed).
 *   2. FFX Denoiser (shadow mode): consumes the tile ray-hit mask + depth +
 *      world normal + motion vectors and produces a full-resolution,
 *      temporally accumulated shadow mask (RGBA8, .r = lit fraction, 1 = lit).
 *
 * The classifier needs the per-cascade light-view / light-projection
 * matrices from the CSM pass (see ShadowCascadeData) to map world positions
 * into shadow-map UVs.
 *
 * Dormant by default: enable with ENGINE_HYBRID_SHADOWS=1.  The mask is
 * consumed by the lit fragment shaders (Phase 3).  Debug knobs:
 *   ENGINE_HYBRID_SHADOWS=1            enable the pass
 *   ENGINE_HS_SUN_ANGLE=<deg>          sun size in light space (softness)
 *   ENGINE_HS_BLOCKER_OFFSET=<f>       blocker (bias) offset
 *   ENGINE_HS_DEPTH_SIGMA=<f>          denoiser depth similarity sigma
 *   ENGINE_HS_DUMP=<path>              dump the shadow mask to disk (NDEBUG)
 */
class VulkanShadowDenoisePass : public System {
public:
    VulkanShadowDenoisePass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
};

extern VulkanShadowDenoisePass vulkanShadowDenoisePass;

void vulkanShadowDenoisePassSetEnabled(char enabled);
char vulkanShadowDenoisePassIsEnabled(void);
/* Sun angular diameter in degrees (FFX light-space sun size). */
float vulkanShadowDenoisePassGetSunAngle(void);
void vulkanShadowDenoisePassSetSunAngle(float degrees);
/* Current-frame shadow mask (RGBA8, .r = lit fraction); NULL while the pass
 * is disabled or before the first swapchain exists. */
struct VulkanImage* vulkanShadowDenoisePassGetMask(void);
}  // namespace engine