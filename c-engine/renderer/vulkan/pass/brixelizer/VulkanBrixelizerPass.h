#pragma once
#include "ecs/system/System.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "renderer/vulkan/resources/VulkanBuffer.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#include <FidelityFX/host/ffx_brixelizer.h>
#include <FidelityFX/host/ffx_brixelizergi.h>
#pragma GCC diagnostic pop

namespace engine {
/*
 * VulkanBrixelizerPass
 * --------------------
 * FidelityFX Brixelizer SDF voxelizer (docs/fsr3.1.md, "Brixelizer GI"
 * sections).  Voxelizes the streamed props world into a per-camera SDF
 * clipmap that the brixelizer-GI pass (task 3) traces for diffuse/specular
 * GI.  The engine's terrain is a heightfield (no mesh) and stays invisible
 * to GI rays — the SDF holds props only (canopies, buildings, rocks).
 *
 * Voxelizer stage (this task):
 *  - FFX backend scratch + FfxInterface (same pattern as VulkanFsrPass).
 *  - Voxelizer context: 8 STATIC cascades, 2 m base voxel doubling per
 *    level (the layout the GI sample's BRIXGI_STATIC_ONLY A/B matched;
 *    raw cascade index == level for static-only cascades).
 *  - External FFX resources: 512^3 R8 SDF atlas, brick-AABB list, per-
 *    (24) cascade AABB tree + brick map, update scratch buffer.
 *  - Prop-tile lifecycle: each fully-built tile's PropInstance set is
 *    registered as FFX static instances (pos/yaw/scale row-major
 *    transform, diagonal [0]/[5]/[10] — the [0]/[4]/[8] layout was the
 *    old brick-collapse bug); the registration is re-created when a tile
 *    is rebuilt (new readyStamp) and deleted when the tile is evicted.
 *    Cull re-uploads (compact subsets of the same stamp) are ignored.
 *  - Per-frame ffxBrixelizerBakeUpdate + ffxBrixelizerUpdate with the
 *    camera-following, snapped clipmap center; FfxBrixelizerStats logged
 *    (lagged readback; freeBricks is the brick-collapse signal).
 *
 * Registered between the decal and composite passes so the (later) GI
 * dispatch can consume the SDF in the same frame.
 */
class VulkanBrixelizerPass : public System {
public:
    VulkanBrixelizerPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
};

extern VulkanBrixelizerPass vulkanBrixelizerPass;

// Live voxelizer context (NULL while not created) — the GI pass feeds it
// to ffxBrixelizerGIContextCreate and ffxBrixelizerGetContextInfo.
FfxBrixelizerContext* vulkanBrixelizerPassGetContext(void);
char vulkanBrixelizerPassIsEnabled(void);

// Current (snapped) clipmap center + cascade count the context was built with.
char vulkanBrixelizerPassGetSdfCenter(float out[3]);
u32  vulkanBrixelizerPassGetNumCascades(void);

// The FFX-owned GPU resources (the GI pass reads the SDF atlas / brick AABBs
// from them; the cascade buffers are passed through as-is).
struct VulkanImage* vulkanBrixelizerPassGetSdfAtlas(void);
struct VulkanBuffer* vulkanBrixelizerPassGetBrickAABBs(void);
struct VulkanBuffer* vulkanBrixelizerPassGetCascadeAabbTree(u32 cascade);
struct VulkanBuffer* vulkanBrixelizerPassGetCascadeBrickMap(u32 cascade);

// ── GI stage (task 3) ─────────────────────────────────────────────────────
// The Brixelizer GI output images (render resolution, R16G16B16A16_SFLOAT).
// NULL until the GI context is created; the composite pass (task 4) samples the
// diffuse GI from these.  debugVisualization is the FFX radiance/irradiance
// cache debug view, produced only when ENGINE_BRIX_GI_DEBUG is set.
struct VulkanImage* vulkanBrixelizerPassGetDiffuseGI(void);
struct VulkanImage* vulkanBrixelizerPassGetSpecularGI(void);
struct VulkanImage* vulkanBrixelizerPassGetDebugVisualization(void);
char vulkanBrixelizerPassGIReady(void);
// Runtime GI toggle (debug GUI): off skips the GI dispatch and makes the GI
// accessors above report no output, so the composite falls back to no GI.
void vulkanBrixelizerPassSetGIEnabled(char enabled);
char vulkanBrixelizerPassIsGIEnabled(void);
// The GI render resolution (matches the GBuffer; 0x0 while not created).
char vulkanBrixelizerPassGetGIResolution(u32* outWidth, u32* outHeight);
// 1 while the GI outputs exist and are exactly the given (GBuffer) size —
// the composite must only consume a matching pair (it fetches per-pixel,
// no rescale).
char vulkanBrixelizerPassGIResolutionMatches(const struct VulkanImage* gi);
}  // namespace engine
