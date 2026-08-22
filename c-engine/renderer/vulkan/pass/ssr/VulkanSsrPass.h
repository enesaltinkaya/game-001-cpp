#pragma once

#include "ecs/system/System.h"

namespace engine {
/* Screen-space reflections, now backed by the FidelityFX SSSR component
 * (stochastic ray marching + the bundled Denoiser's temporal reflections
 * pass). Keeps the old pass' slot in the pipeline and its interface to the
 * composite pass (the full-res reflection-color buffer the composite samples
 * for the specular reflection term). */
class VulkanSsrPass : public System {
public:
    VulkanSsrPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern VulkanSsrPass vulkanSsrPass;

void  vulkanSsrPassSetDisabled(char disabled);
char  vulkanSsrPassIsDisabled(void);
}  // namespace engine