#pragma once
#include "ecs/system/System.h"

/* FidelityFX FfxInterface (ffx_interface.h) — kept out of this header so
 * FFX includes stay in the .cpp, matching the FSR/AO/LPM passes. */
struct FfxInterface;

namespace engine {
/* FidelityFX Brixelizer voxelizer (SDF brick pipeline). Step 1 of
 * plans/brixelizer-gi.md: owns the shared FFX backend interface, the
 * engine-side SDF resources, and the voxelizer context; runs the per-frame
 * bake/update (no instances yet — the cascade pass set is a no-op-but-
 * executed baseline until Steps 2–5 register geometry). The GI context
 * lands in Step 7 and reuses this pass's backend interface. */
class VulkanBrixelizerPass : public System {
public:
    VulkanBrixelizerPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern VulkanBrixelizerPass vulkanBrixelizerPass;

/* Copies the shared FFX backend interface (scratch sized for 2 contexts:
 * brixelizer + GI) into *out. Zero-initialized until the first update after
 * swapchain creation. */
char vulkanBrixelizerPassGetInterface(FfxInterface* out);
/* Voxelizer context created (resources + ffxBrixelizerContextCreate done). */
char vulkanBrixelizerPassIsReady(void);
}  // namespace engine