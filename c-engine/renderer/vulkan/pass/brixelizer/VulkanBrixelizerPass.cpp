#include "VulkanBrixelizerPass.h"
#include "ecs/Ecs.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/window/WindowSystem.h"
#include "events/Events.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/barrier/VulkanBarrier.h"
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
#include <string.h>
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
    static char createTestInstance(Camera* camera);
    static FfxBrixelizerTraceDebugModes getSdfDebugMode(void);

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
    /* Step 2: SDF debug visualization target (render-res R16F RGBA, written
     * by the FFX debug pass as a UAV, dumped via brixelSdf). */
    static VulkanImage sdfDebug;

    /* Step 2 smoke-test instance: a generated cube proves the voxelizer bakes
     * geometry through our resources (replaced by real scene meshes in Step
     * 3). Positions-only 12 B vertices + u16 indices; placed 10 m in front of
     * the current camera (the parked player) so the parked view sees it. */
    static const float CUBE_HALF_EXTENT = 2.0f;
    static const float CUBE_DISTANCE    = 10.0f;
    static const float cubeVertexData[8 * 3] = {
        /* 8 corners (±CUBE_HALF_EXTENT), CCW-from-outside faces below */
        -1.0f * CUBE_HALF_EXTENT, -1.0f * CUBE_HALF_EXTENT, -1.0f * CUBE_HALF_EXTENT,
         1.0f * CUBE_HALF_EXTENT, -1.0f * CUBE_HALF_EXTENT, -1.0f * CUBE_HALF_EXTENT,
         1.0f * CUBE_HALF_EXTENT,  1.0f * CUBE_HALF_EXTENT, -1.0f * CUBE_HALF_EXTENT,
        -1.0f * CUBE_HALF_EXTENT,  1.0f * CUBE_HALF_EXTENT, -1.0f * CUBE_HALF_EXTENT,
        -1.0f * CUBE_HALF_EXTENT, -1.0f * CUBE_HALF_EXTENT,  1.0f * CUBE_HALF_EXTENT,
         1.0f * CUBE_HALF_EXTENT, -1.0f * CUBE_HALF_EXTENT,  1.0f * CUBE_HALF_EXTENT,
         1.0f * CUBE_HALF_EXTENT,  1.0f * CUBE_HALF_EXTENT,  1.0f * CUBE_HALF_EXTENT,
        -1.0f * CUBE_HALF_EXTENT,  1.0f * CUBE_HALF_EXTENT,  1.0f * CUBE_HALF_EXTENT,
    };
    static const u16 cubeIndexData[36] = {
        0, 1, 4, 4, 1, 5,  /* -Y */
        2, 3, 7, 2, 7, 6,  /* +Y */
        0, 4, 7, 0, 7, 3,  /* -X */
        1, 6, 5, 1, 2, 6,  /* +X */
        0, 3, 2, 0, 2, 1,  /* -Z */
        4, 6, 7, 4, 5, 6,  /* +Z */
    };
    static VulkanBuffer cubeVertBuf;
    static VulkanBuffer cubeIdxBuf;
    static u32 cubeVertBufIdx;
    static u32 cubeIdxBufIdx;
    static FfxBrixelizerInstanceID cubeInstanceID;
    static char testInstanceReady;
    /* ENGINE_BRIX_NO_CUBE=1: skip the test instance (ghost-artifact
     * diagnostics). */
    static char noCube;
    static char noCubeSet;
    /* The test cube is camera-relative, but the camera drifts for a few frames
     * at startup before settling at the parked position — creating the
     * instance during that drift would bake it at the pre-settle offset.
     * Wait until renderLocation is stable for CUBE_SETTLE_FRAMES, then create
     * the instance at the settled position (matches how Step 3's real meshes
     * sit at fixed world positions). */
    static vec3 cubeSettleLastLoc;
    static char cubeSettleHaveLast;
    static u32 cubeSettleStableFrames;
    static const u32 CUBE_SETTLE_FRAMES = 30; /* ~0.5 s of a stable camera */
    static const float CUBE_SETTLE_EPS2 = 1e-8f; /* (1e-4 m)^2 */
    /* ENGINE_BRIX_DUMP_BRICKMAP=1: one-shot readback of the cascade-0 brick
     * map (which voxel slots hold bricks) to /tmp, for ghost-artifact
     * analysis. */
    static VulkanBuffer brickMapReadbacks[FFX_BRIXELIZER_MAX_CASCADES];
    static char brickMapCopyRecorded;
    static u32 brickMapCopyFrame;
    static char brickMapDumped;
    static char dumpBrickMap;
    static char dumpBrickMapSet;
    /* ENGINE_BRIX_DUMP_SCRATCH=1: one-shot readback of the FFX scratch state
     * right after the cube's cascade-0 bake — the 10 scratch counters
     * (triangles swapped, references, brick allocations) plus the head of
     * the triangle-swap buffer. Written to /tmp/brixel_scratch_*.bin. */
    static VulkanBuffer scratchDumpBuf;
    static char scratchCopyRecorded;
    static u32 scratchCopyFrame;
    static char scratchDumped;
    static char dumpScratch;
    static char dumpScratchSet;
    /* The pass-frame (pre-increment) on which the cube instance was created;
     * the cascade-0 bake lands on the next odd FFX frame. */
    static u32 cubeCreatedFrame;
    /* Multi-frame cascade-0 brick-map tracker (the 27->2 brick loss): one
     * 1 MiB readback per dump, sampled every other cascade-0 update after
     * the bake (static frames cubeCreatedFrame + 2k). */
    static VulkanBuffer c0Readbacks[6];
    static char c0CopyRecorded[6];
    static char c0Dumped[6];

    /* Debug visualization mode (ENGINE_BRIX_SDF_DEBUG=distance|grad|brick|
     * cascade|uvw|iter; default distance). Step 9 moves this to the GUI. */
    static FfxBrixelizerTraceDebugModes sdfDebugMode;
    static char sdfDebugModeSet;
    static char sdfDebugEnabled = 1;
    static char statsTrisLogged;
    /* Debug ray-march range (ENGINE_BRIX_SDF_TMAX, default the sample's
     * 10000). The distance view normalizes hit distance by tMax, so near
     * objects need a smaller value to show a visible band. */
    static float sdfDebugTMax;
    static char sdfDebugTMaxSet;
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
        statsTrisLogged = 0;
        /* The registered-buffer table + instance table live inside the FFX
         * context — both are recreated with it. */
        testInstanceReady = 0;
        cubeInstanceID    = 0;
        /* The instance is recreated after a swapchain resize — restart the
         * settle clock so it is created at the (re)settled camera position. */
        cubeSettleHaveLast     = 0;
        cubeSettleStableFrames = 0;
    }

    static void destroyResources(void) {
        if (sdfAtlas.img) {
            vulkanDestroyImage(&sdfAtlas, NULL);
            sdfAtlas = VulkanImage{};
        }
        if (sdfDebug.img) {
            vulkanDestroyImage(&sdfDebug, NULL);
            sdfDebug = VulkanImage{};
        }
        if (cubeVertBuf.buf) {
            vulkanDestroyBuffer(&cubeVertBuf, NULL);
            cubeVertBuf = VulkanBuffer{};
        }
        if (cubeIdxBuf.buf) {
            vulkanDestroyBuffer(&cubeIdxBuf, NULL);
            cubeIdxBuf = VulkanBuffer{};
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
        if (scratchDumpBuf.buf) {
            vulkanDestroyBuffer(&scratchDumpBuf, NULL);
            scratchDumpBuf = VulkanBuffer{};
        }
        for (u32 i = 0; i < 6; i++) {
            if (c0Readbacks[i].buf) {
                vulkanDestroyBuffer(&c0Readbacks[i], NULL);
                c0Readbacks[i] = VulkanBuffer{};
            }
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
            /* TRANSFER_SRC: lets a debug readback (ENGINE_BRIX_DUMP_BRICKMAP)
             * copy the brick map to a CPU buffer. No VRAM cost. */
            cascadeBrickMaps[i] = vulkanCreateGpuBuffer("BrixelCascadeBrickMaps",
                                                        FFX_BRIXELIZER_CASCADE_BRICK_MAP_SIZE,
                                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
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

        /* Step 2 test cube: positions-only 12 B vertices + u16 indices.
         * STORAGE usage: the FFX backend binds vertex/index buffers as shader
         * storage (plan pitfall #13). TRANSFER_DST: the one-time staging
         * upload below (vkCmdCopyBuffer requires it). */
        cubeVertBuf = vulkanCreateGpuBuffer("BrixelCubeVerts",
                                           sizeof(cubeVertexData),
                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        cubeIdxBuf = vulkanCreateGpuBuffer("BrixelCubeIdx",
                                           sizeof(cubeIndexData),
                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

        /* SDF debug visualization target (Step 2.2): STORAGE (the FFX debug
         * pass writes it as a UAV) + SAMPLED (dumps / later sampling) +
         * TRANSFER_SRC (vulkanSaveImage dumps it). */
        u32 renderW = window.renderWidth > 0 ? (u32)window.renderWidth : (u32)window.width;
        u32 renderH = window.renderHeight > 0 ? (u32)window.renderHeight : (u32)window.height;
        sdfDebug    = vulkanCreateImage(.name   = "BrixelSdfDebug",
                                        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                        .usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                        .width  = (int)renderW,
                                        .height = (int)renderH);
        if (!cubeVertBuf.buf || !cubeIdxBuf.buf || !sdfDebug.img) {
            utils::error("vulkanBrixelizerPass: brixelizer test-cube/debug resource creation failed");
            destroyResources();
            return 0;
        }

        /* One-time: cube upload (staging pattern) + SDF atlas clear so pre-bake
         * dumps are predictable (0 = no brick allocated yet); the FFX
         * clear-bricks pass only rewrites bricks that were previously
         * allocated. */
        VulkanCommand* cmd = vulkanTransientBegin();
        vulkanCopy(.cmd         = cmd,
                   .source.data = (void*)cubeVertexData,
                   .target.buf  = &cubeVertBuf,
                   .size        = (u32)sizeof(cubeVertexData));
        vulkanCopy(.cmd         = cmd,
                   .source.data = (void*)cubeIndexData,
                   .target.buf  = &cubeIdxBuf,
                   .size        = (u32)sizeof(cubeIndexData));
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

    /* Step 2.1: register the cube buffers and create one static instance.
     * Runs once (retried while it fails); the instance is baked by the next
     * ffxBrixelizerUpdate (CPU-side job table, flushed inside the update). */
    static char createTestInstance(Camera* camera) {
        if (testInstanceReady) {
            return 1;
        }
        if (!noCubeSet) {
            noCubeSet = 1;
            const char* env = getenv("ENGINE_BRIX_NO_CUBE");
            noCube = (env && !strcmp(env, "1")) ? 1 : 0;
        }
        if (noCube) {
            testInstanceReady = 1; /* mark done so we don't re-check each frame */
            return 1;
        }
        /* Wait for the camera to settle before creating the instance (see the
         * cubeSettle* state above). A camera-relative instance created during
         * the startup drift leaves a ghost SDF copy at the earlier offset. */
        vec3 curLoc;
        glm_vec3_copy(camera->cameraUbo.renderLocation, curLoc);
        if (!cubeSettleHaveLast) {
            glm_vec3_copy(curLoc, cubeSettleLastLoc);
            cubeSettleHaveLast     = 1;
            cubeSettleStableFrames = 0;
        } else {
            float dx = curLoc[0] - cubeSettleLastLoc[0];
            float dy = curLoc[1] - cubeSettleLastLoc[1];
            float dz = curLoc[2] - cubeSettleLastLoc[2];
            if (dx * dx + dy * dy + dz * dz < CUBE_SETTLE_EPS2) {
                cubeSettleStableFrames++;
            } else {
                glm_vec3_copy(curLoc, cubeSettleLastLoc);
                cubeSettleStableFrames = 0;
            }
        }
        if (cubeSettleStableFrames < CUBE_SETTLE_FRAMES) {
            return 0; /* not settled yet — retry next frame */
        }
        /* PIXEL_COMPUTE_READ like the sample's GetBufferIndex — the FFX
         * backend binds every SRV buffer as a shader storage buffer. */
        FfxResource vertRes =
            vulkanFfxWrapBufferResource(&cubeVertBuf,
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ,
                                        L"BrixelCubeVerts");
        FfxResource idxRes =
            vulkanFfxWrapBufferResource(&cubeIdxBuf,
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ,
                                        L"BrixelCubeIdx");
        FfxBrixelizerBufferDescription bufDescs[2] = {};
        bufDescs[0].buffer   = vertRes;
        bufDescs[0].outIndex = &cubeVertBufIdx;
        bufDescs[1].buffer   = idxRes;
        bufDescs[1].outIndex = &cubeIdxBufIdx;
        FfxErrorCode regResult = ffxBrixelizerRegisterBuffers(&brixelizerContext, bufDescs, 2);
        if (regResult != FFX_OK) {
            utils::error("vulkanBrixelizerPass: ffxBrixelizerRegisterBuffers failed: %d", regResult);
            return 0;
        }

        /* One static instance: the cube 10 m in front of the camera (parked
         * player) so the parked view sees it. The transform is a ROW-major 3x4
         * (plan pitfall #2 — the GLSL LoadInstanceTransform loads 3 rows):
         * identity rotation + translation in column 3 of each row. */
        vec3 dir;
        glm_vec3_copy(camera->cameraUbo.renderDirection, dir);
        float dirLen = glm_vec3_norm(dir);
        if (dirLen < 1e-6f) {
            dir[0] = 1.0f;
            dir[1] = 0.0f;
            dir[2] = 0.0f;
        } else {
            glm_vec3_scale(dir, 1.0f / dirLen, dir);
        }
        const float cx = camera->cameraUbo.renderLocation[0] + dir[0] * CUBE_DISTANCE;
        const float cy = camera->cameraUbo.renderLocation[1] + dir[1] * CUBE_DISTANCE;
        const float cz = camera->cameraUbo.renderLocation[2] + dir[2] * CUBE_DISTANCE;

        FfxBrixelizerInstanceDescription inst = {};
        inst.maxCascade                        = 0; /* near cascade only (detail) */
        const float center[3] = {cx, cy, cz};
        for (u32 i = 0; i < 3; i++) {
            inst.aabb.min[i] = center[i] - CUBE_HALF_EXTENT;
            inst.aabb.max[i] = center[i] + CUBE_HALF_EXTENT;
        }
        /* ROW-major 3x4 (plan pitfall #2 — the GLSL LoadInstanceTransform
         * loads 3 rows and applies them row-vector style): the identity
         * diagonal sits at [0], [5], [10]. ([0]/[4]/[8] — column-major-style
         * indices — projected the cube onto the main diagonal: every output
         * component read p.x, the triangles collapsed onto a line, and
         * CompressBrick freed all but the two endpoint voxels — the "two
         * regions / ghost cube" artifact.) */
        inst.transform[0]  = 1.0f;
        inst.transform[5]  = 1.0f;
        inst.transform[10] = 1.0f;
        inst.transform[3]  = center[0]; /* row 0, col 3 */
        inst.transform[7]  = center[1]; /* row 1, col 3 */
        inst.transform[11] = center[2]; /* row 2, col 3 */
        inst.indexFormat   = FFX_INDEX_TYPE_UINT16;
        inst.indexBuffer   = cubeIdxBufIdx;
        inst.indexBufferOffset = 0;
        inst.triangleCount = 12;
        inst.vertexBuffer  = cubeVertBufIdx;
        inst.vertexStride  = 12;
        inst.vertexBufferOffset = 0;
        inst.vertexCount   = 8;
        inst.vertexFormat  = FFX_SURFACE_FORMAT_R32G32B32_FLOAT;
        inst.flags         = FFX_BRIXELIZER_INSTANCE_FLAG_NONE;
        inst.outInstanceID = &cubeInstanceID;
        FfxErrorCode instResult = ffxBrixelizerCreateInstances(&brixelizerContext, &inst, 1);
        if (instResult != FFX_OK) {
            utils::error("vulkanBrixelizerPass: ffxBrixelizerCreateInstances failed: %d", instResult);
            return 0;
        }

        testInstanceReady = 1;
        cubeCreatedFrame  = frameIndex;
        /* Diagnostics: the cube's cascade-0 local voxel coord (grid is 64^3,
         * 2 m/voxel, centered on sdfCenter) is (center - (sdfCenter - 64)) / 2
         * = 32 + dir * 5, so it should sit near voxel 32 (grid center), never
         * near an edge. If the debug dump shows the cube at a wrapped / second
         * position, this is the reference to diff against. */
        utils::info(
            "vulkanBrixelizerPass: test cube instance created at frame %u at (%.1f, %.1f, %.1f) "
            "(cam=(%.1f, %.1f, %.1f) dir=(%.2f, %.2f, %.2f) id=%u buf %u/%u)",
            frameIndex,
            cx,
            cy,
            cz,
            camera->cameraUbo.renderLocation[0],
            camera->cameraUbo.renderLocation[1],
            camera->cameraUbo.renderLocation[2],
            dir[0],
            dir[1],
            dir[2],
            cubeInstanceID,
            cubeVertBufIdx,
            cubeIdxBufIdx);
        return 1;
    }

    static FfxBrixelizerTraceDebugModes getSdfDebugMode(void) {
        if (!sdfDebugModeSet) {
            sdfDebugModeSet = 1;
            const char* env = getenv("ENGINE_BRIX_SDF_DEBUG");
            if (env && !strcmp(env, "grad")) {
                sdfDebugMode = FFX_BRIXELIZER_TRACE_DEBUG_MODE_GRAD;
            } else if (env && !strcmp(env, "brick")) {
                sdfDebugMode = FFX_BRIXELIZER_TRACE_DEBUG_MODE_BRICK_ID;
            } else if (env && !strcmp(env, "cascade")) {
                sdfDebugMode = FFX_BRIXELIZER_TRACE_DEBUG_MODE_CASCADE_ID;
            } else if (env && !strcmp(env, "uvw")) {
                sdfDebugMode = FFX_BRIXELIZER_TRACE_DEBUG_MODE_UVW;
            } else if (env && !strcmp(env, "iter")) {
                sdfDebugMode = FFX_BRIXELIZER_TRACE_DEBUG_MODE_ITERATIONS;
            } else if (env && !strcmp(env, "off")) {
                sdfDebugEnabled = 0;
            } else {
                sdfDebugMode = FFX_BRIXELIZER_TRACE_DEBUG_MODE_DISTANCE;
            }
        }
        if (!sdfDebugTMaxSet) {
            sdfDebugTMaxSet = 1;
            const char* env = getenv("ENGINE_BRIX_SDF_TMAX");
            sdfDebugTMax    = (env && *env) ? (float)atof(env) : 10000.0f;
        }
        return sdfDebugMode;
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

        /* Step 2: register the test cube's buffers + create its one static
         * instance (runs once; the instance persists for the context's
         * lifetime). Must run before the bake so the instance's job is baked
         * in this frame's update. */
        createTestInstance(camera);

        /* Step 2.2: read the debug-visualization mode once (ENGINE_BRIX_SDF_DEBUG;
         * "off" disables the extra dispatch). */
        getSdfDebugMode();

        /* The SDF atlas (and the debug image, when active) are the images this
         * update touches; the FFX dispatch does not manage engine layouts
         * (plan pitfall #11) — stage them for UAV writes. */
        vulkanTransition(cmd, &sdfAtlas, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        if (sdfDebugEnabled && sdfDebug.img) {
            vulkanTransition(cmd, &sdfDebug, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        }

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

        /* Step 2.2: SDF debug visualization — ray-march the baked SDF into a
         * render-res R16F image (mode via ENGINE_BRIX_SDF_DEBUG, default
         * distance; off disables the extra dispatch). The inverse matrices
         * are the engine's cglm column-major mat4s memcpy'd verbatim (plan
         * pitfall #1 — the GLSL unprojection expects them column-major). The
         * static-only cascade layout puts the detail cascade at index 0. */
        FfxBrixelizerDebugVisualizationDescription debugVisDesc = {};
        if (sdfDebugEnabled && sdfDebug.img) {
            memcpy(debugVisDesc.inverseViewMatrix,
                   camera->cameraUbo.invView,
                   sizeof(debugVisDesc.inverseViewMatrix));
            memcpy(debugVisDesc.inverseProjectionMatrix,
                   camera->cameraUbo.invProjection,
                   sizeof(debugVisDesc.inverseProjectionMatrix));
            debugVisDesc.debugState        = getSdfDebugMode();
            debugVisDesc.startCascadeIndex = 0;
            debugVisDesc.endCascadeIndex   = BRIX_NUM_CASCADES - 1;
            debugVisDesc.sdfSolveEps       = 0.5f;
            debugVisDesc.tMin              = 0.0f;
            debugVisDesc.tMax              = sdfDebugTMax;
            debugVisDesc.renderWidth       = (u32)sdfDebug.extent.width;
            debugVisDesc.renderHeight      = (u32)sdfDebug.extent.height;
            debugVisDesc.output            = vulkanFfxWrapImageResource(&sdfDebug,
                                                                        FFX_RESOURCE_USAGE_UAV,
                                                                        FFX_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                        L"BrixelSdfDebug");
            updateDesc.debugVisualizationDesc = &debugVisDesc;
        }

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

        /* FFX_BRIX_DIAG: poll for the one-shot FFX pipeline-end dump (lands
         * a few cascade-0 updates after the cube's first bake). */
        {
            static bool diagDumped = false;
            uint32_t diagSize = 0;
            void* diagDump = ffxBrixelizerRawGetDiagDump(&diagSize);
            if (diagDump && diagSize > 0 && !diagDumped) {
                FILE* f = fopen("/tmp/brixel_diag.bin", "wb");
                if (f) {
                    fwrite(diagDump, 1, diagSize, f);
                    fclose(f);
                }
                utils::info("vulkanBrixelizerPass: brix diag dump written to /tmp/brixel_diag.bin (%u bytes)",
                            diagSize);
                diagDumped = true;
            }
        }

        /* ENGINE_BRIX_DUMP_SCRATCH: capture FFX scratch state right after the
         * cube's cascade-0 bake (copy recorded in this same command list,
         * after the update's dispatches; the next cascade update reuses the
         * shared scratch, so it must land on the bake frame itself — cascade
         * 0 runs on odd FFX frames = odd static frameIndex, first one >=
         * cubeCreatedFrame). Targeted regions at the ground-truth scratch
         * offsets (dumped from FFX via FFX_BRIX_LOG_LAYOUT):
         *   [0, 64 KiB)        counters + padding
         *   [64, 128 KiB)     bricks_storage (first 32 bricks x 2048 B)
         *   [128, 132 KiB)    bricks_storage_offsets (first 1024)
         *   [132, 132.25 KiB) bricks_compression_list (first 64)
         *   [132.25, 134.25)  cr1_references (first 128 raw refs)
         *   [134.25, 134.75)  cr1_compacted_references (first 128)
         *   [134.75, +1M)     cr1_ref_counters
         *   [+1M, +1M)        cr1_ref_counter_scan
         *   [+1M, +1M)        cr1_stamp_scan
         *   [+1M, +4K)        cr1_stamp_global_scan */
        if (dumpScratch && !scratchCopyRecorded && testInstanceReady &&
            cubeInstanceID != FFX_BRIXELIZER_INVALID_ID &&
            (frameIndex & 1u) == 1u && frameIndex >= cubeCreatedFrame) {
            static const struct {
                u32 srcOff;
                u32 dstOff;
                u32 size;
            } regions[] = { {0u,            0u,        65536u},
                            {315621632u,    65536u,    65536u},
                            {349176064u,   131072u,     4096u},
                            {350224640u,   135168u,      256u},
                            {352322560u,   135424u,     2048u},
                            {754975744u,   137472u,      512u},
                            {889193472u,   137984u,   1048576u},
                            {890242048u,  1186560u,  1048576u},
                            {891294720u,  2235136u,  1048576u},
                            {892343296u,  3283712u,     4096u} };
            const u32 total = 3287808;
            if (!scratchDumpBuf.buf) {
                scratchDumpBuf = vulkanCreateReadbackBuffer("BrixelScratchDump", total, 0);
            }
            if (scratchDumpBuf.buf) {
                for (u32 r = 0; r < 10; r++) {
                    vulkanCopy(.cmd             = cmd,
                               .source.buf      = &gpuScratch,
                               .source.offset   = regions[r].srcOff,
                               .target.buf      = &scratchDumpBuf,
                               .target.bufferOffset = regions[r].dstOff,
                               .size            = regions[r].size);
                }
                vulkanBarrier(cmd, DEVICE_WRITE_TO_HOST_READ);
                scratchCopyRecorded = 1;
                scratchCopyFrame    = frameIndex;
                utils::info("vulkanBrixelizerPass: scratch dump copies recorded at frame %u (post-bake)",
                            scratchCopyFrame);
            }
        } else if (scratchCopyRecorded && frameIndex >= scratchCopyFrame + 4) {
            const uint32_t* data = (const uint32_t*)scratchDumpBuf.vmaInfo.pMappedData;
            FILE* f = fopen("/tmp/brixel_scratch_dump.bin", "wb");
            if (f) {
                fwrite(data, sizeof(uint32_t), 3287808 / 4, f);
                fclose(f);
            }
            utils::info(
                "vulkanBrixelizerPass: scratch counters (post-bake): triangles=%u maxTriangles=%u "
                "refs=%u maxRefs=%u groupIndex=%u compressionBricks=%u storageOffset=%u "
                "storageSize=%u bricksAllocated=%u clearBricks=%u",
                data[0],
                data[1],
                data[2],
                data[3],
                data[4],
                data[5],
                data[6],
                data[7],
                data[8],
                data[9]);
            scratchDumped = 1;
        }

        /* The GI ray-march (Step 7) samples the atlas — leave it readable.
         * The debug image is a dump target — leave it readable too. */
        vulkanTransition(cmd, &sdfAtlas, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        if (sdfDebugEnabled && sdfDebug.img) {
            vulkanTransition(cmd, &sdfDebug, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        }

        /* Ghost-artifact diagnostic (ENGINE_BRIX_DUMP_BRICKMAP=1): read back
         * the cascade-0 brick map (which voxel slots hold bricks) to /tmp.
         * Three-phase: this frame records the copy (in the current flight's
         * command buffer); a few frames later (FRAMES_IN_FLIGHT=2, so the
         * flight's fence has signalled and the GPU write is complete) the
         * mapped buffer is read and the file written. */
        if (!dumpBrickMapSet) {
            dumpBrickMapSet = 1;
            const char* env = getenv("ENGINE_BRIX_DUMP_BRICKMAP");
            dumpBrickMap = (env && !strcmp(env, "1")) ? 1 : 0;
        }
        if (!dumpScratchSet) {
            dumpScratchSet = 1;
            const char* env = getenv("ENGINE_BRIX_DUMP_SCRATCH");
            dumpScratch = (env && !strcmp(env, "1")) ? 1 : 0;
        }
        if (dumpBrickMap && !brickMapDumped) {
                /* Record the copy after the cube's bake (frame ~31 in the
                 * settle-wait flow; 45 = safely post-bake). The FFX does not
                 * bake a freshly-created instance on the same frame, so an
                 * earlier capture reads a pre-bake brick map. */
                if (!brickMapCopyRecorded && frameIndex >= 45) {
                    int allCreated = 1;
                    for (u32 i = 0; i < BRIX_NUM_CASCADES; i++) {
                        if (!brickMapReadbacks[i].buf) {
                            char rbName[64];
                            snprintf(rbName, 64, "BrixelBrickMapDump%u", i);
                            brickMapReadbacks[i] = vulkanCreateReadbackBuffer(rbName, FFX_BRIXELIZER_CASCADE_BRICK_MAP_SIZE, 0);
                        }
                        if (!brickMapReadbacks[i].buf) allCreated = 0;
                    }
                    if (allCreated) {
                        for (u32 i = 0; i < BRIX_NUM_CASCADES; i++) {
                            vulkanCopy(.cmd         = cmd,
                                       .source.buf  = &cascadeBrickMaps[i],
                                       .target.buf  = &brickMapReadbacks[i],
                                       .size        = FFX_BRIXELIZER_CASCADE_BRICK_MAP_SIZE);
                            vulkanBarrier(cmd, DEVICE_WRITE_TO_HOST_READ);
                        }
                        brickMapCopyRecorded = 1;
                        brickMapCopyFrame = frameIndex;
                        utils::info("vulkanBrixelizerPass: brick-map copies (all cascades) recorded at frame %u",
                                    brickMapCopyFrame);
                    }
                } else if (brickMapCopyRecorded && frameIndex >= brickMapCopyFrame + 4) {
                    for (u32 c = 0; c < BRIX_NUM_CASCADES; c++) {
                        const uint32_t* data = (const uint32_t*)brickMapReadbacks[c].vmaInfo.pMappedData;
                        const uint32_t   total = FFX_BRIXELIZER_CASCADE_BRICK_MAP_SIZE / sizeof(uint32_t);
                        char             path[64];
                        snprintf(path, 64, "/tmp/brixel_brickmap_cascade%u.bin", c);
                        FILE* f = fopen(path, "wb");
                        if (f) {
                            fwrite(data, sizeof(uint32_t), total, f);
                            fclose(f);
                        }
                        uint32_t occupied = 0;
                        char     line[256] = "";
                        for (u32 z = 0; z < 64; z++) {
                            for (u32 y = 0; y < 64; y++) {
                                for (u32 x = 0; x < 64; x++) {
                                    uint32_t id = data[(z * 64 + y) * 64 + x];
                                    if (id != FFX_BRIXELIZER_UNINITIALIZED_ID && id != FFX_BRIXELIZER_INVALID_ID) {
                                        occupied++;
                                        if (occupied <= 6) {
                                            char tmp[40];
                                            int  tn =
                                                snprintf(tmp, sizeof(tmp), " (%u,%u,%u)->%u", x, y, z, id);
                                            if (strlen(line) + (size_t)tn < sizeof(line) - 1) {
                                                strcat(line, tmp);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        utils::info("vulkanBrixelizerPass: cascade %u brick map: %u occupied of %u slots%s",
                                    c,
                                    occupied,
                                    total,
                                    occupied ? line : "");
                    }
                    brickMapDumped = 1;
                }
            }

        /* Multi-frame cascade-0 brick-map tracker: watch the occupied-slot
         * set across the cascade-0 updates after the bake (the 27 -> 2 loss).
         * A copy recorded at static frame f lands in the flight after FFX
         * frame f-1 (the just-finished update), so f = cubeCreatedFrame + 2k
         * samples right after FFX frames ..., 31, 33, 35, 37, 39, 41. */
        if (dumpBrickMap && cubeCreatedFrame) {
            for (u32 k = 1; k <= 6; k++) {
                const u32 f = cubeCreatedFrame + 2u * k;
                if (frameIndex == f) {
                    if (!c0Readbacks[k - 1].buf) {
                        char name[64];
                        snprintf(name, 64, "BrixelBrickMapC0Dump%u", k - 1);
                        c0Readbacks[k - 1] = vulkanCreateReadbackBuffer(name, FFX_BRIXELIZER_CASCADE_BRICK_MAP_SIZE, 0);
                    }
                    if (c0Readbacks[k - 1].buf) {
                        vulkanCopy(.cmd         = cmd,
                                   .source.buf  = &cascadeBrickMaps[0],
                                   .target.buf  = &c0Readbacks[k - 1],
                                   .size        = FFX_BRIXELIZER_CASCADE_BRICK_MAP_SIZE);
                        vulkanBarrier(cmd, DEVICE_WRITE_TO_HOST_READ);
                        c0CopyRecorded[k - 1] = 1;
                    }
                } else if (c0CopyRecorded[k - 1] && !c0Dumped[k - 1] && frameIndex >= f + 4) {
                    const uint32_t* data = (const uint32_t*)c0Readbacks[k - 1].vmaInfo.pMappedData;
                    const uint32_t  total = FFX_BRIXELIZER_CASCADE_BRICK_MAP_SIZE / sizeof(uint32_t);
                    char            path[64];
                    snprintf(path, 64, "/tmp/brixel_brickmap_c0_f%u.bin", f - 1);
                    FILE* fp = fopen(path, "wb");
                    if (fp) {
                        fwrite(data, sizeof(uint32_t), total, fp);
                        fclose(fp);
                    }
                    uint32_t occupied = 0;
                    char     line[256] = "";
                    for (u32 z = 0; z < 64; z++) {
                        for (u32 y = 0; y < 64; y++) {
                            for (u32 x = 0; x < 64; x++) {
                                uint32_t id = data[(z * 64 + y) * 64 + x];
                                if (id != FFX_BRIXELIZER_UNINITIALIZED_ID && id != FFX_BRIXELIZER_INVALID_ID) {
                                    occupied++;
                                    if (occupied <= 8) {
                                        char tmp[40];
                                        int  tn =
                                            snprintf(tmp, sizeof(tmp), " (%u,%u,%u)->%u", x, y, z, id);
                                        if (strlen(line) + (size_t)tn < sizeof(line) - 1) {
                                            strcat(line, tmp);
                                        }
                                    }
                                }
                            }
                        }
                    }
                    utils::info(
                        "vulkanBrixelizerPass: cascade-0 brick map after FFX frame %u: %u occupied%s",
                        f - 1,
                        occupied,
                        line);
                    c0Dumped[k - 1] = 1;
                }
            }
        }

        /* outStats is a lagged GPU readback (filled a few updates later). */
        /* Track the free-brick pool across updates (lagged readback): the
         * 262144-27 -> 262144-2 transition pins down which update frees the
         * baked bricks. */
        static u32 lastFreeBricks = 0;
        if (stats.contextStats.freeBricks && stats.contextStats.freeBricks != lastFreeBricks) {
            lastFreeBricks = stats.contextStats.freeBricks;
            utils::info(
                "vulkanBrixelizerPass: free-brick pool (lagged stats, cascade %u): free=%u allocAttempted=%u allocSucceeded=%u cleared=%u",
                stats.cascadeIndex,
                stats.contextStats.freeBricks,
                stats.contextStats.brickAllocationsAttempted,
                stats.contextStats.brickAllocationsSucceeded,
                stats.contextStats.bricksCleared);
        }
        /* Gate 2: the test cube actually baked (triangles/bricks allocated in
         * a static cascade) — fires once, lagged. */
        if ((stats.staticCascadeStats.trianglesAllocated || stats.staticCascadeStats.bricksAllocated) &&
            !statsTrisLogged) {
            statsTrisLogged = 1;
            utils::info(
                "vulkanBrixelizerPass: test instance baked (lagged): cascade=%u staticTris=%u "
                "staticRefs=%u staticBricks=%u freeBricks=%u",
                stats.cascadeIndex,
                stats.staticCascadeStats.trianglesAllocated,
                stats.staticCascadeStats.referencesAllocated,
                stats.staticCascadeStats.bricksAllocated,
                stats.contextStats.freeBricks);
        }
        if (frameIndex % 120 == 0) {
            utils::info(
                "vulkanBrixelizerPass: stats cascade=%u freeBricks=%u bricksCleared=%u "
                "staticTris=%u staticRefs=%u staticBricks=%u gpu=%.3f ms cam=(%.1f, %.1f, %.1f)",
                stats.cascadeIndex,
                stats.contextStats.freeBricks,
                stats.contextStats.bricksCleared,
                stats.staticCascadeStats.trianglesAllocated,
                stats.staticCascadeStats.referencesAllocated,
                stats.staticCascadeStats.bricksAllocated,
                profile.elapsed / MILLION, /* ns → ms */
                camera->cameraUbo.renderLocation[0],
                camera->cameraUbo.renderLocation[1],
                camera->cameraUbo.renderLocation[2]);
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

    struct VulkanImage* vulkanBrixelizerPassGetSdfDebug(void) {
        return sdfDebug.img ? &sdfDebug : NULL;
    }
}  // namespace engine