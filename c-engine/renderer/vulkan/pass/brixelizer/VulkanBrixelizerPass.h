#pragma once
#include "ecs/system/System.h"

/* FidelityFX FfxInterface (ffx_interface.h) — kept out of this header so
 * FFX includes stay in the .cpp, matching the FSR/AO/LPM passes. */
struct FfxInterface;

namespace engine {
struct Scene;
struct VulkanImage;

/* FidelityFX Brixelizer voxelizer (SDF brick pipeline). Steps 1–3 of
 * plans/brixelizer-gi.md: owns the shared FFX backend interface, the
 * engine-side SDF resources, and the voxelizer context; registers the
 * static scene meshes (Step 3) and runs the per-frame bake/update. The GI
 * context lands in Step 7 and reuses this pass's backend interface. */
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
/* SDF debug visualization image (R16F RGBA, render res, Step 2). Null until
 * the first update after swapchain creation. */
struct VulkanImage* vulkanBrixelizerPassGetSdfDebug(void);
/* Scene hooks (called from rendererSceneCreate/Destroy in Renderer.cpp).
 * Scene create runs on the scene-load worker thread, so the FFX registration
 * is deferred to a main-thread task; a scene created before the context
 * exists is picked up when the context (re)registers all ecs.scenes. */
void vulkanBrixelizerPassSceneCreate(struct Scene* scene);
void vulkanBrixelizerPassSceneDestroy(struct Scene* scene);
}  // namespace engine