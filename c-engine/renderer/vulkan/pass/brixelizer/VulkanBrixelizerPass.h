#pragma once
#include "ecs/system/System.h"

/* FidelityFX FfxInterface (ffx_interface.h) — kept out of this header so
 * FFX includes stay in the .cpp, matching the FSR/AO/LPM passes. */
struct FfxInterface;

namespace engine {
struct Scene;
struct VulkanImage;
struct PropInstance;
struct PropVariantRange;

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

/* Step 6: GI input resources. All null until the first update after
 * swapchain creation (created with the voxelizer resources). */
struct VulkanImage* vulkanBrixelizerPassGetBlueNoise(void);
/* Face N of the 128² environment cube (copied into a plain 2D image for
 * dumping). Null until the env cube has been baked. */
struct VulkanImage* vulkanBrixelizerPassGetEnvFace(u32 face);
struct VulkanImage* vulkanBrixelizerPassGetHistoryDepth(void);
struct VulkanImage* vulkanBrixelizerPassGetHistoryNormal(void);
struct VulkanImage* vulkanBrixelizerPassGetHistoryLit(void);

/* Step 7: GI context outputs (R16F RGBA, render res). All null until the
 * first update after swapchain creation (created with the GI context). */
struct VulkanImage* vulkanBrixelizerPassGetGiDiffuse(void);
struct VulkanImage* vulkanBrixelizerPassGetGiSpecular(void);
/* GI cache debug visualization (radiance / irradiance — ENGINE_BRIXGI_DEBUG),
 * render-res R16F. Null until the GI context exists. */
struct VulkanImage* vulkanBrixelizerPassGetGiDebug(void);
/* GI enabled (persisted `giEnabled` setting, env override ENGINE_BRIXGI,
 * default on) AND the GI context exists. When false the composite pass
 * skips the GI terms entirely (pixel-identical to pre-GI). */
char vulkanBrixelizerPassIsGiEnabled(void);
/* Step 9: settings / debug GUI (plans/brixelizer-gi.md). GI on/off and the
 * internal resolution are persisted settings (settings.json, written by the
 * settings GUI) with env overrides (ENGINE_BRIXGI / ENGINE_BRIXGI_RES, read
 * in added() — the ENGINE_AO_DISABLED pattern). The SDF debug mode and the GI
 * cache view are debug-only (env: ENGINE_BRIXGI_SDF_DEBUG, legacy
 * ENGINE_BRIX_SDF_DEBUG, and ENGINE_BRIXGI_DEBUG) — no persistence. */
char vulkanBrixelizerPassGetGiEnabled(void);
void vulkanBrixelizerPassSetGiEnabled(char enabled);
/* Internal GI resolution percent (50/75/100, clamped to those values).
 * A change recreates the GI context (internalResolution is a
 * context-creation parameter). */
int vulkanBrixelizerPassGetGiResolution(void);
void vulkanBrixelizerPassSetGiResolution(int percent);
/* SDF debug visualization mode: 0 = off, 1 = distance, 2 = gradient,
 * 3 = brick ID, 4 = cascade ID, 5 = UVW, 6 = iteration heatmap. */
int vulkanBrixelizerPassGetSdfDebugMode(void);
void vulkanBrixelizerPassSetSdfDebugMode(int mode);
/* GI cache debug view: 0 = off, 1 = radiance cache, 2 = irradiance cache.
 * Enables re-arm the one-shot cache visualization dispatch (a few frames out).
 * 0 disables it (the pending one-shot is cancelled). */
int vulkanBrixelizerPassGetGiCacheDebug(void);
void vulkanBrixelizerPassSetGiCacheDebug(int mode);
/* Latest (lagged) voxelizer stats + last-frame GPU costs in ms, for the
 * settings GUI readout. Any out pointer may be NULL. */
void vulkanBrixelizerPassGetStats(u32* outFreeBricks,
                                  u32* outStaticTris,
                                  u32* outStaticRefs,
                                  u32* outStaticBricks,
                                  u32* outDynamicTris,
                                  u32* outDynamicRefs,
                                  u32* outDynamicBricks,
                                  double* outVoxelizerMs,
                                  double* outGiMs);
/* Scene hooks (called from rendererSceneCreate/Destroy in Renderer.cpp).
 * Scene create runs on the scene-load worker thread, so the FFX registration
 * is deferred to a main-thread task; a scene created before the context
 * exists is picked up when the context (re)registers all ecs.scenes. */
void vulkanBrixelizerPassSceneCreate(struct Scene* scene);
void vulkanBrixelizerPassSceneDestroy(struct Scene* scene);

/* Step 5: props (vegetation / buildings) SDF. The merged species mesh is
 * pushed once per world load from azgaarPropsInit (main thread): the per-
 * (species, variant) position-only sub-buffers (12 B/vertex — the 72 B
 * PropsVertex does not fit the voxelizer's 6-bit stride field) + u16/u32
 * index ranges are extracted here and registered with the voxelizer when
 * its context exists. Per-tile / global / landmark instance lists arrive
 * from the azgaar_props pass (worker threads), are queued thread-safely,
 * and budgeted (ENGINE_BRIXGI_PROP_BUDGET, default 40 k; priority = species
 * class then distance to camera) on the render thread before the bake. */
void vulkanBrixelizerPassSetPropsMeshes(const void* verts, u32 vertCount,
                                        const void* idx, u32 idxCount,
                                        const struct PropVariantRange* variants,
                                        u32 variantCount);
void vulkanBrixelizerPassPropsTileSet(i32 tileX, i32 tileZ, u64 readyStamp,
                                      const struct PropInstance* instances,
                                      u32 instanceCount);
void vulkanBrixelizerPassPropsTileClear(i32 tileX, i32 tileZ);
void vulkanBrixelizerPassPropsGlobalSet(const struct PropInstance* instances,
                                        u32 instanceCount);
void vulkanBrixelizerPassPropsGlobalClear(void);
void vulkanBrixelizerPassPropsLandmarksSet(const struct PropInstance* instances,
                                           u32 instanceCount);
void vulkanBrixelizerPassPropsLandmarksClear(void);
}  // namespace engine