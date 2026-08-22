#include "VulkanSsrPass.h"

#include "ecs/Ecs.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/window/WindowSystem.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/pipeline/VulkanProfile.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "renderer/vulkan/resources/VulkanIbl.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/utils/VulkanUtils.h"
#include "timer/Timer.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <FidelityFX/host/ffx_sssr.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#pragma GCC diagnostic pop

#include <cstdlib>
#include <cstring>

namespace engine {

    /* ── SSSR tuning ─────────────────────────────────────────────────────
     * Defaults sit close to the old custom SSR's behavior (roughness cutoff
     * 0.25, moderate temporal accumulation). The env overrides mirror the DOF
     * pass' headless-validation pattern — no settings-file edits needed. */
    static float  ssrRoughnessThreshold       = 0.25f;
    static float  ssrTemporalStability        = 0.5f;
    static float  ssrDepthBufferThickness     = 0.5f;
    static float  ssrVarianceThreshold        = 0.01f;
    static uint32_t ssrSamplesPerQuad        = 1;
    static uint32_t ssrMaxTraversalIntersections = 20;
    static uint32_t ssrMinTraversalOccupancy = 32;
    static uint32_t ssrMostDetailedMip       = 1;
    static char   ssrVarianceGuidedTracing   = 1;

    static void swapchainCreated(void* _);
    static void createNormalsImage(void);
    static void destroyNormalsImage(void);
    static void destroyContext(void);
    static char ensureContext(u32 width, u32 height);
    static VkImageCreateInfo makeImageCreateInfo(VulkanImage* image);
    static FfxResource wrapImageResource(VulkanImage* image,
                                         FfxResourceUsage usage,
                                         FfxResourceStates state,
                                         const wchar_t* name);
    static void clearReflectionColor(VulkanCommand* cmd, VulkanImage* reflColor);

    static double elapsedCPU;
    static double elapsedGPU;

    /* Decoded linear world normals (the G-buffer stores oct-encoded RG, which
     * the SDK's normal unpack can't invert). Filled by the normal-decode
     * pre-pass, then fed to SSSR + the denoiser's normal history. */
    static VulkanImage sssrNormals;
    static u32 sssrNormalsWidth;
    static u32 sssrNormalsHeight;

    static VulkanPipe normalDecodePipe;
    static char normalDecodePipeReady;

    static void* scratchBuffer;
    static size_t scratchBufferSize;
    static FfxInterface backendInterface;
    static char backendReady;
    static FfxSssrContext context;
    static char contextReady;
    static char contextBroken;
    static u32 contextWidth;
    static u32 contextHeight;

    static VulkanProfile profile;
    static char profileReady;

    static char ssrDisabled;

    VulkanSsrPass vulkanSsrPass;

    VulkanSsrPass::VulkanSsrPass() : System("ssr") {}

    void VulkanSsrPass::added() {
        utils::signalSubscribe("swapchainCreated", swapchainCreated);

        ssrDisabled = utils::settingsGetBool("ssrDisabled") ? 1 : 0;

        const char* env;
        if ((env = getenv("ENGINE_SSR_DISABLED")) && *env) {
            ssrDisabled = atoi(env) ? 1 : 0;
        }
        if ((env = getenv("ENGINE_SSSR_ROUGHNESS")) && *env) {
            ssrRoughnessThreshold = atof(env);
        }
        if ((env = getenv("ENGINE_SSSR_STABILITY")) && *env) {
            ssrTemporalStability = atof(env);
        }
        if ((env = getenv("ENGINE_SSSR_THICKNESS")) && *env) {
            ssrDepthBufferThickness = atof(env);
        }
        if ((env = getenv("ENGINE_SSSR_SAMPLES")) && *env) {
            ssrSamplesPerQuad = (uint32_t)atoi(env);
            if (ssrSamplesPerQuad < 1) ssrSamplesPerQuad = 1;
            if (ssrSamplesPerQuad > 4) ssrSamplesPerQuad = 4;
        }

        normalDecodePipe      = vulkanCreatePipe(.name = "ssr_normal_decode",
                                                 .comp = "shaders/pass/ssr/spv/normal_decode.comp.spv");
        normalDecodePipeReady = 1;

        profile      = vulkanCreateProfile("ssr");
        profileReady = 1;
    }

    void VulkanSsrPass::preUpdate() {
        if (profileReady) {
            vulkanResetProfile(vulkan.currentCmd, &profile, 0);
        }
    }

    static void createNormalsImage(void) {
        destroyNormalsImage();
        if (window.renderWidth <= 0 || window.renderHeight <= 0) {
            return;
        }
        sssrNormals =
            vulkanCreateImage(.name   = "SssrNormals",
                              .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                              .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              .width  = window.renderWidth,
                              .height = window.renderHeight);
        sssrNormalsWidth  = (u32)window.renderWidth;
        sssrNormalsHeight = (u32)window.renderHeight;

        VulkanCommand* cmd = vulkanTransientBegin();
        vulkanTransition(cmd, &sssrNormals, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        vulkanTransientEnd(cmd, 1);
        utils::info("vulkanSsrPass: created normals image %ux%u", sssrNormalsWidth, sssrNormalsHeight);
    }

    static void destroyNormalsImage(void) {
        if (sssrNormals.img) {
            vulkanDestroyImage(&sssrNormals, NULL);
            sssrNormals = VulkanImage{};
        }
        sssrNormalsWidth  = 0;
        sssrNormalsHeight = 0;
    }

    static void destroyContext(void) {
        if (contextReady) {
            ffxSssrContextDestroy(&context);
            context      = FfxSssrContext{};
            contextReady = 0;
        }
    }

    static void swapchainCreated(void* _) {
        (void)_;
        destroyContext();
        createNormalsImage();
    }

    static VkImageCreateInfo makeImageCreateInfo(VulkanImage* image) {
        VkImageCreateInfo info = {};
        info.sType             = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType         = VK_IMAGE_TYPE_2D;
        info.format            = image->format;
        info.extent            = image->extent;
        info.mipLevels         = (u32)image->mipLevels;
        info.arrayLayers       = (u32)image->layers;
        info.samples           = image->samples;
        info.tiling            = VK_IMAGE_TILING_OPTIMAL;
        info.usage             = image->usage;
        info.sharingMode       = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
        /* The IBL prefilter is a cubemap; the FFX backend keys the cube
         * resource type off this flag when building the view. */
        if (image->viewType == VK_IMAGE_VIEW_TYPE_CUBE) {
            info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        }
        return info;
    }

    static FfxResource wrapImageResource(VulkanImage* image,
                                         FfxResourceUsage usage,
                                         FfxResourceStates state,
                                         const wchar_t* name) {
        VkImageCreateInfo createInfo = makeImageCreateInfo(image);
        FfxResourceDescription desc =
            ffxGetImageResourceDescriptionVK(image->img, createInfo, usage);
        return ffxGetResourceVK(image->img, desc, name, state);
    }

    static char ensureContext(u32 width, u32 height) {
        if (contextReady && contextWidth == width && contextHeight == height) {
            return 1;
        }

        if (!backendReady) {
            /* SSSR owns two effect contexts (itself + the bundled denoiser). */
            scratchBufferSize =
                ffxGetScratchMemorySizeVK(vulkan.physicalDevice, FFX_SSSR_CONTEXT_COUNT);
            scratchBuffer = calloc(1, scratchBufferSize);
            if (!scratchBuffer) {
                utils::error(
                    "vulkanSsrPass: failed to allocate %zu bytes of backend scratch memory",
                    scratchBufferSize);
                return 0;
            }
            VkDeviceContext deviceContext = {
                .vkDevice         = vulkan.device,
                .vkPhysicalDevice = vulkan.physicalDevice,
                .vkDeviceProcAddr = vkGetDeviceProcAddr,
            };
            FfxDevice device           = ffxGetDeviceVK(&deviceContext);
            FfxErrorCode backendResult = ffxGetInterfaceVK(&backendInterface,
                                                           device,
                                                           scratchBuffer,
                                                           scratchBufferSize,
                                                           FFX_SSSR_CONTEXT_COUNT);
            if (backendResult != FFX_OK) {
                utils::error("vulkanSsrPass: ffxGetInterfaceVK failed: %d", backendResult);
                free(scratchBuffer);
                scratchBuffer     = NULL;
                scratchBufferSize = 0;
                return 0;
            }
            backendReady = 1;
        }

        if (contextReady) {
            /* The previous frame's command buffer may still be in flight and
             * referencing the context's pipelines. A full wait is acceptable
             * (resize is rare, same pattern as the DOF pass). */
            vulkanWaitIdle("ssr context recreate");
        }
        destroyContext();

        FfxSssrContextDescription desc = {};
        /* Reverse-Z depth (1 = near, 0 = far) — the engine's D32 buffer. The
         * normal history is a linear R16F world normal (the denoiser blit-
         * copies our decoded-normal image into it each frame). */
        desc.flags                    = FFX_SSSR_ENABLE_DEPTH_INVERTED;
        desc.renderSize.width         = width;
        desc.renderSize.height        = height;
        desc.normalsHistoryBufferFormat = FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT;
        desc.backendInterface         = backendInterface;

        FfxErrorCode result = ffxSssrContextCreate(&context, &desc);
        if (result != FFX_OK) {
            utils::error("vulkanSsrPass: ffxSssrContextCreate failed: %d", result);
            context       = FfxSssrContext{};
            contextBroken = 1;
            return 0;
        }
        contextReady   = 1;
        contextWidth   = width;
        contextHeight  = height;
        utils::info("vulkanSsrPass: created SSSR context %ux%u", width, height);
        return 1;
    }

    static void clearReflectionColor(VulkanCommand* cmd, VulkanImage* reflColor) {
        vulkanTransition(cmd, reflColor, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);

        VkClearColorValue clearColor = {
            .float32 = {0.0f, 0.0f, 0.0f, 0.0f},
        };
        VkImageSubresourceRange range = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        };
        vkCmdClearColorImage(cmd->cmd,
                             reflColor->img,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &clearColor,
                             1,
                             &range);

        vulkanTransition(cmd, reflColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    }

    void VulkanSsrPass::update() {
        elapsedCPU = utils::nanos();

        if (vulkan.skipFrame) {
            elapsedCPU = utils::nanos() - elapsedCPU;
            return;
        }

        VulkanCommand* cmd        = vulkan.currentCmd;
        VulkanImage*   sceneColor = vulkanFrameResourcesGetSceneColor();
        VulkanImage*   depth      = vulkanFrameResourcesGetDepth();
        VulkanImage*   normals    = vulkanFrameResourcesGetNormals();
        VulkanImage*   material   = vulkanFrameResourcesGetMaterial();
        VulkanImage*   velocity   = vulkanFrameResourcesGetVelocity();
        VulkanImage*   reflColor  = vulkanFrameResourcesGetReflectionColor();

        if (!reflColor) {
            elapsedCPU = utils::nanos() - elapsedCPU;
            return;
        }

        if (ssrDisabled || contextBroken || !sceneColor || !depth || !normals ||
            !material || !velocity || window.renderWidth <= 0 || window.renderHeight <= 0) {
            clearReflectionColor(cmd, reflColor);
            elapsedCPU = utils::nanos() - elapsedCPU;
            return;
        }

        Entity* camEntity = cameraGetEntity();
        Camera* camera    = getComponent(camEntity->scene, Camera, camEntity->id);
        if (!camera) {
            clearReflectionColor(cmd, reflColor);
            elapsedCPU = utils::nanos() - elapsedCPU;
            return;
        }

        /* IBL fallback inputs (environment cubemap + BRDF LUT). The BRDF LUT
         * is declared by the SSSR shader but unused; a valid resource is still
         * required. Without IBL the environment falls back to black. */
        VulkanImage* prefilter = vulkanIblGetPrefilterImage();
        VulkanImage* brdfLut   = vulkanIblGetBrdfLutImage();
        if (!prefilter || !brdfLut) {
            clearReflectionColor(cmd, reflColor);
            elapsedCPU = utils::nanos() - elapsedCPU;
            return;
        }

        if (!sssrNormals.img || sssrNormalsWidth != (u32)window.renderWidth ||
            sssrNormalsHeight != (u32)window.renderHeight) {
            createNormalsImage();
            if (!sssrNormals.img) {
                clearReflectionColor(cmd, reflColor);
                elapsedCPU = utils::nanos() - elapsedCPU;
                return;
            }
        }
        if (!ensureContext((u32)window.renderWidth, (u32)window.renderHeight)) {
            clearReflectionColor(cmd, reflColor);
            elapsedCPU = utils::nanos() - elapsedCPU;
            return;
        }

        /* ── Normal decode pre-pass ───────────────────────────────────────
         * Expand the G-buffer's oct-encoded world normals to a linear image
         * the SDK can consume. */
        vulkanTransition(cmd, normals, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, &sssrNormals, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

        if (normalDecodePipeReady) {
            vulkanBindPipe(cmd, &normalDecodePipe);
            struct NormalDecodePushConstants {
                u32 normalsIndex;
                u32 outputIndex;
                u32 width;
                u32 height;
            } ndpc = {
                .normalsIndex = (u32)normals->sampledPoolIndex,
                .outputIndex  = (u32)sssrNormals.storagePoolIndex,
                .width        = sssrNormalsWidth,
                .height       = sssrNormalsHeight,
            };
            vulkanPush(cmd, &normalDecodePipe, sizeof(ndpc), &ndpc);
            u32 groupsX = (sssrNormalsWidth + 7) / 8;
            u32 groupsY = (sssrNormalsHeight + 7) / 8;
            vulkanDispatch(cmd, &normalDecodePipe, groupsX, groupsY, 1);
        }

        /* The FFX backend trusts the state we hand it in ffxGetResourceVK and
         * will not emit a barrier from the image's actual layout, so leave
         * the decoded normals in SHADER_READ_ONLY (the state we wrap them with)
         * before the dispatch. The denoiser's normal-history blit-copy then
         * transitions it to TRANSFER_SRC on its own. */
        vulkanTransition(cmd, &sssrNormals, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

        /* ── SSSR dispatch ────────────────────────────────────────────────
         * All matrices are the jittered set (the G-buffer was rasterised with
         * the jittered projection); the denoiser's reprojection is anchored
         * by the motion vectors + prevViewProjection. */
        vulkanTransition(cmd, sceneColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, velocity, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, material, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, prefilter, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, brdfLut, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, reflColor, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

        const CameraUbo* ubo = &camera->cameraUbo;

        FfxSssrDispatchDescription dispatch = {};
        dispatch.commandList = ffxGetCommandListVK(cmd->cmd);
        dispatch.color       = wrapImageResource(sceneColor,
                                                 FFX_RESOURCE_USAGE_READ_ONLY,
                                                 FFX_RESOURCE_STATE_COMPUTE_READ,
                                                 L"sssr_color");
        dispatch.depth       = wrapImageResource(depth,
                                                 FFX_RESOURCE_USAGE_READ_ONLY,
                                                 FFX_RESOURCE_STATE_COMPUTE_READ,
                                                 L"sssr_depth");
        dispatch.motionVectors = wrapImageResource(velocity,
                                                   FFX_RESOURCE_USAGE_READ_ONLY,
                                                   FFX_RESOURCE_STATE_COMPUTE_READ,
                                                   L"sssr_motion_vectors");
        dispatch.normal      = wrapImageResource(&sssrNormals,
                                                 FFX_RESOURCE_USAGE_READ_ONLY,
                                                 FFX_RESOURCE_STATE_COMPUTE_READ,
                                                 L"sssr_normal");
        dispatch.materialParameters = wrapImageResource(material,
                                                        FFX_RESOURCE_USAGE_READ_ONLY,
                                                        FFX_RESOURCE_STATE_COMPUTE_READ,
                                                        L"sssr_material");
        dispatch.environmentMap = wrapImageResource(prefilter,
                                                    FFX_RESOURCE_USAGE_READ_ONLY,
                                                    FFX_RESOURCE_STATE_COMPUTE_READ,
                                                    L"sssr_environment_map");
        dispatch.brdfTexture   = wrapImageResource(brdfLut,
                                                   FFX_RESOURCE_USAGE_READ_ONLY,
                                                   FFX_RESOURCE_STATE_COMPUTE_READ,
                                                   L"sssr_brdf_texture");
        dispatch.output        = wrapImageResource(reflColor,
                                                   FFX_RESOURCE_USAGE_UAV,
                                                   FFX_RESOURCE_STATE_UNORDERED_ACCESS,
                                                   L"sssr_output");

        memcpy(dispatch.invViewProjection, &ubo->invViewProjection, sizeof(float) * 16);
        memcpy(dispatch.projection, &ubo->projection, sizeof(float) * 16);
        memcpy(dispatch.invProjection, &ubo->invProjection, sizeof(float) * 16);
        memcpy(dispatch.view, &ubo->view, sizeof(float) * 16);
        memcpy(dispatch.invView, &ubo->invView, sizeof(float) * 16);
        memcpy(dispatch.prevViewProjection, &ubo->prevViewProjection, sizeof(float) * 16);

        dispatch.renderSize.width  = (u32)window.renderWidth;
        dispatch.renderSize.height = (u32)window.renderHeight;
        /* Velocity is in pixels (current - previous); the denoiser computes
         * history_uv = uv + scale * mv, so scale = -1/res recovers prevUv. */
        dispatch.motionVectorScale.x = -1.0f / (float)window.renderWidth;
        dispatch.motionVectorScale.y = -1.0f / (float)window.renderHeight;

        dispatch.iblFactor             = 1.0f;
        dispatch.normalUnPackMul       = 1.0f;
        dispatch.normalUnPackAdd       = 0.0f;
        dispatch.roughnessChannel      = 0;  /* material buffer: R = roughness */
        dispatch.isRoughnessPerceptual = false;
        dispatch.temporalStabilityFactor = ssrTemporalStability;
        dispatch.depthBufferThickness  = ssrDepthBufferThickness;
        dispatch.roughnessThreshold    = ssrRoughnessThreshold;
        dispatch.varianceThreshold     = ssrVarianceThreshold;
        dispatch.maxTraversalIntersections = ssrMaxTraversalIntersections;
        dispatch.minTraversalOccupancy = ssrMinTraversalOccupancy;
        dispatch.mostDetailedMip       = ssrMostDetailedMip;
        dispatch.samplesPerQuad        = ssrSamplesPerQuad;
        dispatch.temporalVarianceGuidedTracingEnabled = ssrVarianceGuidedTracing;

        vulkanBeginProfile(cmd, &profile, 0);
        FfxErrorCode result = ffxSssrContextDispatch(&context, &dispatch);
        vulkanEndProfile(cmd, &profile, 0);

        if (result != FFX_OK) {
            utils::error("vulkanSsrPass: ffxSssrContextDispatch failed: %d", result);
            destroyContext();
            contextBroken = 1;
            clearReflectionColor(cmd, reflColor);
            elapsedCPU = utils::nanos() - elapsedCPU;
            return;
        }

        vulkanTransition(cmd, reflColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

        /* Debug: dump the SSSR output (denoised reflection radiance) to disk
         * so the reflection content can be inspected directly. Waits a number
         * of frames (ENGINE_SSSR_DUMP_FRAME, default 60) so the denoiser's
         * temporal accumulation has built up. */
        if (getenv("ENGINE_SSSR_DUMP") && *getenv("ENGINE_SSSR_DUMP")) {
            static int dumpFrame = -1;
            if (dumpFrame < 0) {
                const char* f = getenv("ENGINE_SSSR_DUMP_FRAME");
                dumpFrame = f && *f ? atoi(f) : 60;
            }
            static int frameCount = 0;
            if (++frameCount == dumpFrame) {
                vulkanSaveImage(reflColor, "build/c-game/data/sssr_output.jpg");
                utils::info("vulkanSsrPass: dumped SSSR output at frame %d", dumpFrame);
            }
        }

        elapsedGPU = profile.elapsed;
        elapsedCPU = utils::nanos() - elapsedCPU;
    }

    void VulkanSsrPass::postUpdate() {
        vulkanSsrPass.cpuElapsed = elapsedCPU;
        vulkanSsrPass.gpuElapsed = elapsedGPU;
    }

    void VulkanSsrPass::removed() {
        destroyContext();
        destroyNormalsImage();
        if (scratchBuffer) {
            free(scratchBuffer);
            scratchBuffer     = NULL;
            scratchBufferSize = 0;
        }
        backendInterface = FfxInterface{};
        backendReady     = 0;
        contextBroken    = 0;
        if (profileReady) {
            vulkanDestroyProfile(&profile);
            profile      = VulkanProfile{};
            profileReady = 0;
        }
        if (normalDecodePipeReady) {
            vulkanDestroyPipe(&normalDecodePipe);
            normalDecodePipe      = VulkanPipe{};
            normalDecodePipeReady = 0;
        }
    }

    void vulkanSsrPassSetDisabled(char disabled) {
        ssrDisabled = disabled;
        utils::info("SSR (SSSR): %s", ssrDisabled ? "disabled" : "enabled");
    }

    char vulkanSsrPassIsDisabled(void) {
        return ssrDisabled;
    }
}  // namespace engine