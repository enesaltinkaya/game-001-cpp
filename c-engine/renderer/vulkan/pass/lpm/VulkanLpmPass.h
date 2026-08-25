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

/* Called by the Final pass right after it rendered into the LPM input.
 * The LPM pass only dispatches on frames where this was called — e.g.
 * frames where the Final pass bails out early. */
void vulkanLpmPassMarkRendered(void);
}  // namespace engine