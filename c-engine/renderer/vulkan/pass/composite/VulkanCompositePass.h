#pragma once

#include "ecs/system/System.h"

namespace engine {
class VulkanCompositePass : public System {
public:
    VulkanCompositePass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern VulkanCompositePass vulkanCompositePass;

/* Step 9: Brixelizer GI composite strength (plans/brixelizer-gi.md).
 * Persisted settings (giDiffuseFactor / giSpecularFactor, read in added() with
 * the ENGINE_BRIXGI_DIFFUSE / ENGINE_BRIXGI_SPECULAR env overrides); live-
 * settable from the settings GUI. The FFX sample defaults (1.5 / 3.0) wash
 * out this open-sky outdoor world, so the defaults are lower. */
float vulkanCompositePassGetGiDiffuseFactor(void);
float vulkanCompositePassGetGiSpecularFactor(void);
void vulkanCompositePassSetGiFactors(float diffuse, float specular);
}  // namespace engine
