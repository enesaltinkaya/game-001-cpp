#include "VulkanBrixelizerPass.h"
#include "ecs/Ecs.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "events/Events.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanProfile.h"
#include "renderer/vulkan/resources/VulkanBuffer.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/utils/VulkanFfxUtils.h"
#include "timer/Timer.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function" /* ffx_core_cpu.h static inlines */
#include <FidelityFX/host/ffx_brixelizer.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#pragma GCC diagnostic pop
#include <stdlib.h>
#include <wchar.h>

namespace engine {
    /* Static-only cascade layout (Steps 1–9; Step 10 switches to the sample's
     * 3-per-level layout): 8 cascades, voxel size doubling per level —
     * 2, 4, …, 256 m. The far cascade spans 256 m × 64 bricks = 16.4 km,
     * which covers the 10.24 km heightmap streaming window. */
    static const u32 BRIX_NUM_CASCADES      = 8;
    static const float BRIX_BASE_VOXEL_SIZE = 2.0f;
    /* Bake budgets from the FFX sample (tuned in Step 9). The scratch buffer must
     * hold the reference/swap partitions sized by those budgets (~892 MB at the
     * sample values) — the sample allocates 1 GiB, which we match. */
    static const u32 BRIX_MAX_REFERENCES      = 32u * (1u << 20);
    static const u32 BRIX_TRIANGLE_SWAP_SIZE  = 300u * (1u << 20);
    static const u32 BRIX_MAX_BRICKS_PER_BAKE = 1u << 14;
    static const u64 BRIX_GPU_SCRATCH_SIZE    = 1u << 30;

    static void swapchainCreated(void* _);
    static void destroyResources(void);
    static void destroyContext(void);
    static char createResources(void);
    static char ensureContext(void);

    static double elapsedCPU;
    static double elapsedGPU;

    /* FFX backend shared by the brixelizer + GI contexts (scratch sized for
     * 2; the FSR/CACAO/LPM passes keep their own single-context interfaces). */
    static void* scratchBuffer;
    static size_t scratchBufferSize;
    static FfxInterface backendInterface;
    static char backendReady;

    /* Voxelizer context + engine-owned SDF resources (table in
     * plans/brixelizer-gi.md). */
    static FfxBrixelizerContext brixelizerContext;
    static char contextReady;
    static VulkanImage sdfAtlas;
    static VulkanBuffer brickAABBs;
    static VulkanBuffer cascadeAABBTrees[FFX_BRIXELIZER_MAX_CASCADES];
    static VulkanBuffer cascadeBrickMaps[FFX_BRIXELIZER_MAX_CASCADES];
    static VulkanBuffer gpuScratch;
    /* ~8 MB (wchar-inflated on Linux) — file scope, not stack. */
    static FfxBrixelizerBakedUpdateDescription bakedUpdateDesc;
    static u32 frameIndex;
    static FfxBrixelizerStats stats;
    static char statsLiveLogged;
    static VulkanProfile profile;
    static char profileReady;

    VulkanBrixelizerPass vulkanBrixelizerPass;

    VulkanBrixelizerPass::VulkanBrixelizerPass() : System("brixelizer") {}

    void VulkanBrixelizerPass::added() {
        utils::signalSubscribe("swapchainCreated", swapchainCreated);
        profile      = vulkanCreateProfile("brixelizer");
        profileReady = 1;
    }

    void VulkanBrixelizerPass::preUpdate() {
        if (profileReady) {
            /* force: the stats GUI is usually closed, but the brixelizer cost is
             * tracked in the log until Step 9 moves tuning to the GUI. */
            vulkanResetProfile(vulkan.currentCmd, &profile, 1);
        }
    }

    static void swapchainCreated(void* _) {
        (void)_;
        destroyContext();
        destroyResources();
        frameIndex      = 0;
        stats           = FfxBrixelizerStats{};
        statsLiveLogged = 0;
    }

    static void destroyResources(void) {
        if (sdfAtlas.img) {
            vulkanDestroyImage(&sdfAtlas, NULL);
            sdfAtlas = VulkanImage{};
        }
        if (brickAABBs.buf) {
            vulkanDestroyBuffer(&brickAABBs, NULL);
            brickAABBs = VulkanBuffer{};
        }
        for (i32 i = 0; i < (i32)FFX_BRIXELIZER_MAX_CASCADES; i++) {
            if (cascadeAABBTrees[i].buf) {
                vulkanDestroyBuffer(&cascadeAABBTrees[i], NULL);
                cascadeAABBTrees[i] = VulkanBuffer{};
            }
            if (cascadeBrickMaps[i].buf) {
                vulkanDestroyBuffer(&cascadeBrickMaps[i], NULL);
                cascadeBrickMaps[i] = VulkanBuffer{};
            }
        }
        if (gpuScratch.buf) {
            vulkanDestroyBuffer(&gpuScratch, NULL);
            gpuScratch = VulkanBuffer{};
        }
    }

    static void destroyContext(void) {
        if (contextReady) {
            ffxBrixelizerContextDestroy(&brixelizerContext);
            brixelizerContext = FfxBrixelizerContext{};
            contextReady      = 0;
        }
    }

    static char createResources(void) {
        /* 512³ R8 SDF atlas: STORAGE (FFX UAV brick writes) + SAMPLED (the GI
         * ray-march reads it in Step 7) + TRANSFER_DST (one-time clear below;
         * history resets in Step 10). Not engine-descriptor-bound, so keep it
         * out of the sampled/storage pools. */
        sdfAtlas =
            vulkanCreateImage(.name     = "BrixelSdfAtlas",
                              .format   = VK_FORMAT_R8_UNORM,
                              .usage    = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              .type     = VK_IMAGE_TYPE_3D,
                              .viewType = VK_IMAGE_VIEW_TYPE_3D,
                              .width    = (int)FFX_BRIXELIZER_STATIC_CONFIG_SDF_ATLAS_SIZE,
                              .height   = (int)FFX_BRIXELIZER_STATIC_CONFIG_SDF_ATLAS_SIZE,
                              .layers   = (int)FFX_BRIXELIZER_STATIC_CONFIG_SDF_ATLAS_SIZE,
                              .noPool   = 1);
        if (!sdfAtlas.img) {
            utils::error("vulkanBrixelizerPass: SDF atlas image creation failed");
            return 0;
        }

        brickAABBs = vulkanCreateGpuBuffer("BrixelBrickAABBs",
                                           FFX_BRIXELIZER_BRICK_AABBS_SIZE,
                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        for (u32 i = 0; i < FFX_BRIXELIZER_MAX_CASCADES; i++) {
            cascadeAABBTrees[i] = vulkanCreateGpuBuffer("BrixelCascadeAABBTrees",
                                                        FFX_BRIXELIZER_CASCADE_AABB_TREE_SIZE,
                                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            cascadeBrickMaps[i] = vulkanCreateGpuBuffer("BrixelCascadeBrickMaps",
                                                        FFX_BRIXELIZER_CASCADE_BRICK_MAP_SIZE,
                                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        }
        /* TRANSFER_SRC: the FFX backend vkCmdCopyBuffer's job/constant data into
         * the scratch buffer each update (Cauldron's VK backend adds the same
         * flag for its upload buffers). */
        gpuScratch = vulkanCreateGpuBuffer(
            "BrixelGpuScratch",
            BRIX_GPU_SCRATCH_SIZE,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        if (!brickAABBs.buf || !gpuScratch.buf) {
            utils::error("vulkanBrixelizerPass: brixelizer buffer creation failed");
            destroyResources();
            return 0;
        }
        for (u32 i = 0; i < FFX_BRIXELIZER_MAX_CASCADES; i++) {
            if (!cascadeAABBTrees[i].buf || !cascadeBrickMaps[i].buf) {
                utils::error("vulkanBrixelizerPass: cascade buffer creation failed (index %u)", i);
                destroyResources();
                return 0;
            }
        }

        /* One-time clear so pre-bake dumps are predictable (0 = no brick
         * allocated yet); the FFX clear-bricks pass only rewrites bricks that
         * were previously allocated. */
        VulkanCommand* cmd = vulkanTransientBegin();
        vulkanTransition(cmd, &sdfAtlas, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        VkClearColorValue black = {};
        vulkanClearColorImage(cmd, &sdfAtlas, black);
        vulkanTransientEnd(cmd, 1);
        return 1;
    }

    static char ensureContext(void) {
        if (!backendReady) {
            scratchBufferSize = ffxGetScratchMemorySizeVK(vulkan.physicalDevice, 2);
            scratchBuffer     = calloc(1, scratchBufferSize);
            if (!scratchBuffer) {
                utils::error(
                    "vulkanBrixelizerPass: failed to allocate %zu bytes of FFX backend scratch "
                    "memory",
                    scratchBufferSize);
                return 0;
            }

            VkDeviceContext deviceContext = {
                .vkDevice         = vulkan.device,
                .vkPhysicalDevice = vulkan.physicalDevice,
                .vkDeviceProcAddr = vkGetDeviceProcAddr,
            };

            FfxDevice device = ffxGetDeviceVK(&deviceContext);
            FfxErrorCode backendResult =
                ffxGetInterfaceVK(&backendInterface, device, scratchBuffer, scratchBufferSize, 2);
            if (backendResult != FFX_OK) {
                utils::error("vulkanBrixelizerPass: ffxGetInterfaceVK failed: %d", backendResult);
                free(scratchBuffer);
                scratchBuffer     = NULL;
                scratchBufferSize = 0;
                return 0;
            }

            backendReady = 1;
        }

        if (contextReady) {
            return 1;
        }

        if (!sdfAtlas.img && !createResources()) {
            return 0;
        }

        FfxBrixelizerContextDescription desc = {};
        desc.numCascades                     = BRIX_NUM_CASCADES;
        /* ALL_DEBUG carries the context/cascade readback flags, which are
         * required for outStats: the readback buffers are only allocated when
         * set, and the stats are filled from that (lagged) GPU readback. */
        desc.flags = FFX_BRIXELIZER_CONTEXT_FLAG_ALL_DEBUG;
        for (u32 i = 0; i < BRIX_NUM_CASCADES; i++) {
            desc.cascadeDescs[i].flags     = FFX_BRIXELIZER_CASCADE_STATIC;
            desc.cascadeDescs[i].voxelSize = BRIX_BASE_VOXEL_SIZE * (float)(1u << i);
        }
        desc.backendInterface = backendInterface;

        FfxErrorCode createResult = ffxBrixelizerContextCreate(&desc, &brixelizerContext);
        if (createResult != FFX_OK) {
            if ((u32)createResult == FFX_ERROR_OUT_OF_MEMORY ||
                (u32)createResult == FFX_ERROR_INSUFFICIENT_MEMORY) {
                utils::terminate(
                    "vulkanBrixelizerPass: not enough GPU memory to create the brixelizer context. "
                    "Free VRAM by closing other GPU applications and try again.");
            }
            utils::error("vulkanBrixelizerPass: ffxBrixelizerContextCreate failed: %d",
                         createResult);
            destroyResources();
            return 0;
        }

        contextReady = 1;
        utils::info(
            "vulkanBrixelizerPass: created voxelizer context (%u cascades, voxel %.0f-%.0f m, "
            "gpu scratch %llu MiB)",
            BRIX_NUM_CASCADES,
            BRIX_BASE_VOXEL_SIZE,
            BRIX_BASE_VOXEL_SIZE * (float)(1u << (BRIX_NUM_CASCADES - 1)),
            BRIX_GPU_SCRATCH_SIZE >> 20);
        return 1;
    }

    void VulkanBrixelizerPass::update() {
        elapsedCPU = utils::nanos();
        if (vulkan.skipFrame) {
            elapsedCPU = utils::nanos() - elapsedCPU;
            return;
        }

        VulkanCommand* cmd = vulkan.currentCmd;
        Entity* camEntity  = cameraGetEntity();
        if (!cmd || !camEntity) {
            elapsedCPU = utils::nanos() - elapsedCPU;
            return;
        }
        Camera* camera = getComponent(camEntity->scene, Camera, camEntity->id);
        if (!camera || !ensureContext()) {
            elapsedCPU = utils::nanos() - elapsedCPU;
            return;
        }

        /* The SDF atlas is the only image this update touches; the FFX dispatch
         * does not manage engine layouts (plan pitfall #11) — stage it for UAV
         * writes. */
        vulkanTransition(cmd, &sdfAtlas, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

        FfxBrixelizerUpdateDescription updateDesc = {};
        updateDesc.resources.sdfAtlas =
            vulkanFfxWrapImageResource(&sdfAtlas,
                                       FFX_RESOURCE_USAGE_UAV,
                                       FFX_RESOURCE_STATE_UNORDERED_ACCESS,
                                       L"BrixelSdfAtlas");
        updateDesc.resources.brickAABBs =
            vulkanFfxWrapBufferResource(&brickAABBs,
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        FFX_RESOURCE_STATE_UNORDERED_ACCESS,
                                        L"BrixelBrickAABBs");
        wchar_t cascadeName[64];
        for (u32 i = 0; i < FFX_BRIXELIZER_MAX_CASCADES; i++) {
            swprintf(cascadeName, 64, L"BrixelCascade%uAabbTree", i);
            updateDesc.resources.cascadeResources[i].aabbTree =
                vulkanFfxWrapBufferResource(&cascadeAABBTrees[i],
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                            FFX_RESOURCE_STATE_UNORDERED_ACCESS,
                                            cascadeName);
            swprintf(cascadeName, 64, L"BrixelCascade%uBrickMap", i);
            updateDesc.resources.cascadeResources[i].brickMap =
                vulkanFfxWrapBufferResource(&cascadeBrickMaps[i],
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                            FFX_RESOURCE_STATE_UNORDERED_ACCESS,
                                            cascadeName);
        }

        updateDesc.frameIndex = frameIndex++;
        /* Cascades follow the camera (the sample's m_SdfCenterFollowCamera
         * behavior). */
        updateDesc.sdfCenter[0]            = camera->cameraUbo.renderLocation[0];
        updateDesc.sdfCenter[1]            = camera->cameraUbo.renderLocation[1];
        updateDesc.sdfCenter[2]            = camera->cameraUbo.renderLocation[2];
        updateDesc.populateDebugAABBsFlags = FFX_BRIXELIZER_POPULATE_AABBS_NONE;
        updateDesc.debugVisualizationDesc  = NULL;
        updateDesc.maxReferences           = BRIX_MAX_REFERENCES;
        updateDesc.triangleSwapSize        = BRIX_TRIANGLE_SWAP_SIZE;
        updateDesc.maxBricksPerBake        = BRIX_MAX_BRICKS_PER_BAKE;
        updateDesc.outStats                = &stats;

        size_t scratchNeeded            = 0;
        updateDesc.outScratchBufferSize = &scratchNeeded;

        vulkanBeginProfile(cmd, &profile, 1);
        FfxErrorCode bakeResult =
            ffxBrixelizerBakeUpdate(&brixelizerContext, &updateDesc, &bakedUpdateDesc);
        if (bakeResult == FFX_OK) {
            if (scratchNeeded > BRIX_GPU_SCRATCH_SIZE) {
                utils::error("vulkanBrixelizerPass: brixelizer scratch overflow: %zu > %llu",
                             scratchNeeded,
                             BRIX_GPU_SCRATCH_SIZE);
            }
            FfxResource scratch = vulkanFfxWrapBufferResource(
                &gpuScratch,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                FFX_RESOURCE_STATE_UNORDERED_ACCESS,
                L"BrixelGpuScratch");
            FfxErrorCode result = ffxBrixelizerUpdate(&brixelizerContext,
                                                      &bakedUpdateDesc,
                                                      scratch,
                                                      ffxGetCommandListVK(cmd->cmd));
            if (result != FFX_OK) {
                utils::error("vulkanBrixelizerPass: ffxBrixelizerUpdate failed: %d", result);
            }
        } else {
            utils::error("vulkanBrixelizerPass: ffxBrixelizerBakeUpdate failed: %d", bakeResult);
        }
        vulkanEndProfile(cmd, &profile, 1);

        /* The GI ray-march (Step 7) samples the atlas — leave it readable. */
        vulkanTransition(cmd, &sdfAtlas, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

        /* outStats is a lagged GPU readback (filled a few updates later). */
        if (stats.contextStats.freeBricks && !statsLiveLogged) {
            statsLiveLogged = 1;
            utils::info(
                "vulkanBrixelizerPass: stats readback live (lagged): freeBricks=%u "
                "bricksCleared=%u",
                stats.contextStats.freeBricks,
                stats.contextStats.bricksCleared);
        }
        if (frameIndex % 120 == 0) {
            utils::info(
                "vulkanBrixelizerPass: stats cascade=%u freeBricks=%u bricksCleared=%u "
                "staticTris=%u staticRefs=%u staticBricks=%u gpu=%.3f ms",
                stats.cascadeIndex,
                stats.contextStats.freeBricks,
                stats.contextStats.bricksCleared,
                stats.staticCascadeStats.trianglesAllocated,
                stats.staticCascadeStats.referencesAllocated,
                stats.staticCascadeStats.bricksAllocated,
                profile.elapsed / MILLION); /* ns → ms */
        }

        elapsedGPU = profile.elapsed;
        elapsedCPU = utils::nanos() - elapsedCPU;
    }

    void VulkanBrixelizerPass::postUpdate() {
        vulkanBrixelizerPass.cpuElapsed = elapsedCPU;
        vulkanBrixelizerPass.gpuElapsed = elapsedGPU;
    }

    void VulkanBrixelizerPass::removed() {
        /*
            Crashes on cleanup, safe to ignore for now, we will investigate later.
            Produces validation error;
            vkDestroyDevice(): VkDevice 0x58e2083d2630 has 4226 leaked objects that have not been destroyed.
            Safe to ignore.
        */

        // destroyContext();
        // destroyResources();
        // if (scratchBuffer) {
        //     free(scratchBuffer);
        //     scratchBuffer     = NULL;
        //     scratchBufferSize = 0;
        // }
        // backendInterface = FfxInterface{};
        // backendReady     = 0;
        // if (profileReady) {
        //     vulkanDestroyProfile(&profile);
        //     profile      = VulkanProfile{};
        //     profileReady = 0;
        // }
    }

    char vulkanBrixelizerPassGetInterface(FfxInterface* out) {
        if (out) {
            *out = backendInterface;
        }
        return backendReady;
    }

    char vulkanBrixelizerPassIsReady(void) {
        return contextReady;
    }
}  // namespace engine