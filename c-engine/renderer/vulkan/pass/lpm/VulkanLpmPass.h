#pragma once

#include "ecs/system/System.h"

namespace engine {
class VulkanLpmPass;
struct VulkanImage;

class VulkanLpmPass : public System {
public:
    VulkanLpmPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern VulkanLpmPass vulkanLpmPass;

/* The R16F HDR render target the Final pass composites into (scene HDR +
 * bloom + exposure, linear). The LPM pass tone/gamut-maps it into an 8-bit
 * display-referred image and blits the result into the lens input (when the
 * lens pass is active) or the swapchain. Only valid once the pass has
 * created its images (display resolution, on swapchain recreation). */
struct VulkanImage* vulkanLpmPassGetInput(void);

/* The tone/gamut-mapping parameters of the FFX LPM dispatch (all
 * per-frame — the context is created once and the constants are
 * re-derived CPU-side on every dispatch). The debug GUI (Ctrl+B) drives
 * them live; the LPM pass reads them in update(). Defaults match the
 * FFX SDK sample, tuned to the scene's HDR scale. */
struct VulkanLpmParams {
    float contrast;         /* 0 (neutral) .. 1 (max) — curve exponent is 1+contrast */
    float hdrMax;           /* input luma that maps to display 1.0 */
    float shoulderContrast; /* highlight shoulder exponent (1.0 = off) */
    float saturation;       /* uniform per-channel saturation power offset */
    float lpmExposure;      /* stops between hdrMax and the 18% mid-level */
};

const VulkanLpmParams* vulkanLpmPassGetParams(void);
void vulkanLpmPassSetParams(const VulkanLpmParams* params);

/* Called by the Final pass right after it rendered into the LPM input.
 * The LPM pass only dispatches on frames where this was called — e.g.
 * frames where the Final pass bails out early. */
void vulkanLpmPassMarkRendered(void);
}  // namespace engine