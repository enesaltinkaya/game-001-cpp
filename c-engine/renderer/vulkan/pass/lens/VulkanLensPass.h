#pragma once

#include "ecs/system/System.h"

namespace engine {

class VulkanLensPass : public System {
public:
    VulkanLensPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern VulkanLensPass vulkanLensPass;

/* True when the lens pass will dispatch this frame: the LPM pass blits its
 * display-referred output into the lens input image (SRGB, auto-decoded by
 * the lens shader) instead of the swapchain. False when lens effects are
 * off or not ready. */
char vulkanLensPassIsActive(void);

/* Called by the LPM pass right after it blitted its output into the lens
 * input. The lens pass only dispatches on frames where this was called —
 * e.g. the main menu frame renders no LPM output. */
void vulkanLensPassMarkRendered(void);

/* The SRGB render target the LPM pass blits into when the lens pass is
 * active. Only valid when vulkanLensPassIsActive() — the image is created
 * lazily at display resolution in the lens pass's own update, which runs
 * before the LPM pass's in the pass order... see Vulkan.cpp for ordering;
 * the LPM pass falls back to the swapchain while this returns NULL. */
struct VulkanImage* vulkanLensPassGetInput(void);

void vulkanLensPassSetDisabled(char disabled);
char vulkanLensPassIsDisabled(void);

/* Effect parameters, 0..1 (0 disables that effect). Vignette > 0 only
 * darkens corners; chromAb is a dispersion coefficient; grain is scaled
 * film grain animated by a per-frame seed. */
void vulkanLensPassSetGrain(float amount);
void vulkanLensPassSetChromAb(float amount);
void vulkanLensPassSetVignette(float amount);
float vulkanLensPassGetGrain(void);
float vulkanLensPassGetChromAb(void);
float vulkanLensPassGetVignette(void);

}  // namespace engine
