#include "VulkanDofPass.h"

#include "ecs/Ecs.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/window/WindowSystem.h"
#include "events/Events.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/pipeline/VulkanProfile.h"
#include "renderer/vulkan/pass/fsr/VulkanFsrPass.h"
#include "renderer/vulkan/pass/taa/VulkanTaaPass.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "timer/Timer.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <FidelityFX/host/ffx_dof.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#pragma GCC diagnostic pop

#include <cstdlib>

namespace engine {

    /* Thin-lens CoC model. The FFX DOF shader works in half-res pixels: the
     * downsample pass computes CoC from full-res depth and stores it in the
     * half-res bilateral color's alpha, and the blur pass uses it as a radius
     * in half-res pixels. So the "conversion" factor fed to
     * ffxDofCalculateCocScale (pixels per view-space unit) must be in
     * HALF-RES pixels: (renderWidth / 2) / sensorWidth. The sensor width is
     * fixed at full-frame (36 mm) — the f-number + focal-length sliders are
     * what shape the look, the sensor only sets the absolute scale. */
    static const float DOF_SENSOR_WIDTH_M = 0.036f;
    /* CoC clamp, as a factor of the (full-res) input height: the shader
     * clamps to 0.5 * factor * height half-res pixels. 0.1 caps the bokeh
     * radius at ~1/10 of the screen height (full-res) at 1080p. */
    static const float DOF_COC_LIMIT_FACTOR = 0.1f;
    /* Fixed lens constants (not user-facing): a 50 mm lens at f/2.8 gives a
     * natural, moderate bokeh at the camera distances the game uses. */
    static const float DOF_F_NUMBER        = 2.8f;
    static const float DOF_FOCAL_LENGTH_MM = 50.0f;
    /* Close/mid-range DoF attenuation (see update()): at short-to-mid focus
     * distances the depth of field is physically shallow, so the subject and the
     * ground right around it blur easily (like a phone camera in macro mode).
     * Stop down the effective aperture for the whole close + normal-play range to
     * keep a large sharp zone around the character, ramping back to the base
     * f-number only when zoomed out far enough that bokeh is desirable. */
    static const float DOF_CLOSE_FULL_M =
        5.0f;  // at/below this, fully stopped down (covers close + normal play)
    static const float DOF_CLOSE_NONE_M = 12.0f;  // at/above this, base f-number (bokeh)
    static const float DOF_STOPPED_F    = 20.0f;  // effective f-number when fully stopped

    static void swapchainCreated(void* _);
    static void createOutput(void);
    static void destroyOutput(void);
    static void destroyContext(void);
    static char ensureContext(u32 width, u32 height);
    static VkImageCreateInfo makeImageCreateInfo(VulkanImage* image);
    static FfxResource wrapImageResource(VulkanImage* image,
                                         FfxResourceUsage usage,
                                         FfxResourceStates state,
                                         const wchar_t* name);

    static double elapsedCPU;
    static double elapsedGPU;

    static VulkanImage dofOutput;
    static u32 outputWidth;
    static u32 outputHeight;

    static void* scratchBuffer;
    static size_t scratchBufferSize;
    static FfxInterface backendInterface;
    static char backendReady;
    static FfxDofContext context;
    static char contextReady;
    static char contextBroken;
    static u32 contextQuality;

    static VulkanProfile profile;
    static char profileReady;
    static VulkanPipe cocMaskPipe;
    static char cocMaskPipeReady;

    static char dofDisabled;
    static float focusDistance = 10.0f; /* pushed by the game each frame */
    static int qualityRings    = 4;

    /* Last frame's CoC constants — consumed by the reactive-mask dispatch,
     * which runs later in the same frame (FSR pass, after this one). */
    static float lastCocScale;
    static float lastCocBias;
    static float lastCocLimit;
    static char dispatchedThisFrame;

    VulkanDofPass vulkanDofPass;

    VulkanDofPass::VulkanDofPass() : System("dof") {}

    void VulkanDofPass::added() {
        utils::signalSubscribe("swapchainCreated", swapchainCreated);

        dofDisabled  = utils::settingsGetBool("dofEnabled") ? 0 : 1;
        qualityRings = (int)utils::settingsGetDouble("dofQuality");
        if (qualityRings < 1) qualityRings = 1;
        if (qualityRings > 8) qualityRings = 8;

        /* Env overrides for headless screenshot validation (same pattern as
         * the TAA tuning vars) — no settings-file edits needed. Focus distance
         * is game-driven (vulkanDofPassSetFocusDistance), so it has no override. */
        const char* env;
        if ((env = getenv("ENGINE_DOF_ENABLED")) && *env) {
            dofDisabled = atoi(env) ? 0 : 1;
        }
        if ((env = getenv("ENGINE_DOF_QUALITY")) && *env) {
            qualityRings = atoi(env);
            if (qualityRings < 1) qualityRings = 1;
            if (qualityRings > 8) qualityRings = 8;
        }

        cocMaskPipe      = vulkanCreatePipe(.name = "dof_coc_mask",
                                            .comp = "shaders/pass/dof/spv/coc_mask.comp.spv");
        cocMaskPipeReady = 1;

        profile      = vulkanCreateProfile("dof");
        profileReady = 1;
    }

    void VulkanDofPass::preUpdate() {
        dispatchedThisFrame = 0;
        if (profileReady) {
            vulkanResetProfile(vulkan.currentCmd, &profile, 0);
        }
    }

    static void createOutput(void) {
        destroyOutput();
        if (window.renderWidth <= 0 || window.renderHeight <= 0) {
            return;
        }
        dofOutput =
            vulkanCreateImage(.name   = "DofOutput",
                              .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                              .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              .width  = window.renderWidth,
                              .height = window.renderHeight);
        outputWidth  = (u32)window.renderWidth;
        outputHeight = (u32)window.renderHeight;

        VulkanCommand* cmd = vulkanTransientBegin();
        vulkanTransition(cmd, &dofOutput, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        vulkanTransientEnd(cmd, 1);
        utils::info("vulkanDofPass: created output %ux%u", outputWidth, outputHeight);
    }

    static void destroyOutput(void) {
        if (dofOutput.img) {
            vulkanDestroyImage(&dofOutput, NULL);
            dofOutput = VulkanImage{};
        }
        outputWidth  = 0;
        outputHeight = 0;
    }

    static void destroyContext(void) {
        if (contextReady) {
            ffxDofContextDestroy(&context);
            context      = FfxDofContext{};
            contextReady = 0;
        }
    }

    static void swapchainCreated(void* _) {
        (void)_;
        destroyContext();
        createOutput();
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
        if (contextReady && outputWidth == width && outputHeight == height &&
            contextQuality == (u32)qualityRings) {
            return 1;
        }

        if (!backendReady) {
            scratchBufferSize =
                ffxGetScratchMemorySizeVK(vulkan.physicalDevice, FFX_DOF_CONTEXT_COUNT);
            scratchBuffer = calloc(1, scratchBufferSize);
            if (!scratchBuffer) {
                utils::error(
                    "vulkanDofPass: failed to allocate %zu bytes of backend scratch memory",
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
                                                           FFX_DOF_CONTEXT_COUNT);
            if (backendResult != FFX_OK) {
                utils::error("vulkanDofPass: ffxGetInterfaceVK failed: %d", backendResult);
                free(scratchBuffer);
                scratchBuffer     = NULL;
                scratchBufferSize = 0;
                return 0;
            }
            backendReady = 1;
        }

        if (contextReady) {
            /* The previous frame's command buffer may still be in flight and
             * referencing the context's pipelines (the DoF dispatch is recorded
             * into the main per-frame cmd buffer). Destroying the pipelines
             * while that cmd buffer is executing trips VUID-vkDestroyPipeline-
             * pipeline-00765. Quality changes are rare and user-initiated, so a
             * full wait is acceptable (same pattern as swapchain recreate). */
            vulkanWaitIdle("dof context recreate");
        }
        destroyContext();

        FfxDofContextDescription desc = {};
        /* Reverse-Z depth (1 = near, 0 = far) — the engine's D32 depth buffer.
         * No OUTPUT_PRE_INIT: the output is a separate engine image. */
        desc.flags             = FFX_DOF_REVERSE_DEPTH;
        desc.quality           = (u32)qualityRings;
        desc.resolution.width  = width;
        desc.resolution.height = height;
        desc.cocLimitFactor    = DOF_COC_LIMIT_FACTOR;
        desc.backendInterface  = backendInterface;

        FfxErrorCode result = ffxDofContextCreate(&context, &desc);
        if (result != FFX_OK) {
            utils::error("vulkanDofPass: ffxDofContextCreate failed: %d", result);
            context       = FfxDofContext{};
            contextBroken = 1;
            return 0;
        }
        contextReady   = 1;
        contextQuality = (u32)qualityRings;
        utils::info("vulkanDofPass: created context %ux%u quality %d", width, height, qualityRings);
        return 1;
    }

    void VulkanDofPass::update() {
        if (vulkan.skipFrame || dofDisabled) {
            return;
        }
        if (contextBroken) {
            return;
        }

        /* Input color: the temporally resolved TAA output when TAA is on,
         * otherwise the post-composite HDR color (the same image FSR would
         * consume). Depth: the engine's reverse-Z D32 buffer. */
        VulkanImage* taaColor       = vulkanTaaPassGetOutput();
        VulkanImage* compositeColor = vulkanFrameResourcesGetCompositeColor();
        VulkanImage* color =
            taaColor ? taaColor
                     : (compositeColor ? compositeColor : vulkanFrameResourcesGetSceneColor());
        VulkanImage* depth = vulkanFrameResourcesGetDepth();
        if (!color || !depth || window.renderWidth <= 0 || window.renderHeight <= 0) {
            return;
        }

        if (!dofOutput.img || outputWidth != (u32)window.renderWidth ||
            outputHeight != (u32)window.renderHeight) {
            createOutput();
            if (!dofOutput.img) {
                return;
            }
        }
        if (!ensureContext((u32)window.renderWidth, (u32)window.renderHeight)) {
            return;
        }

        Entity* camEntity = cameraGetEntity();
        Camera* camera    = getComponent(camEntity->scene, Camera, camEntity->id);
        if (!camera) {
            return;
        }

        /* Thin-lens CoC from the camera's projection (cglm RH_ZO, reverse-Z:
         * the engine builds it with near/far swapped, so proj34 > 0 and
         * proj43 = -1). The focus distance is passed as a negative view-space
         * z (the camera looks down -Z), matching the SDK's signed convention.
         * The jittered projection's z-row is jitter-free, so the UBO matrix is
         * fine to read. mat4 is a C array (vec4[4]) — index via pointer. */
        const mat4* proj = &camera->cameraUbo.projection;
        float proj33     = (*proj)[2][2];
        float proj34     = (*proj)[3][2];
        float proj43     = (*proj)[2][3];

        float focalLengthM = DOF_FOCAL_LENGTH_MM * 0.001f;
        float focusM       = focusDistance > 0.1f ? focusDistance : 0.1f;
        float focusSigned  = -focusM;
        float conversion   = ((float)window.renderWidth * 0.5f) / DOF_SENSOR_WIDTH_M;

        /* Close-range DoF attenuation: ramp the effective f-number from the base
         * (full bokeh) at DOF_CLOSE_NONE_M to DOF_STOPPED_F (deep DoF, subject
         * sharp) at DOF_CLOSE_FULL_M. */
        float fNumber = DOF_F_NUMBER;
        if (focusM < DOF_CLOSE_NONE_M) {
            float t = (DOF_CLOSE_NONE_M - focusM) / (DOF_CLOSE_NONE_M - DOF_CLOSE_FULL_M);
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            fNumber = DOF_F_NUMBER + (DOF_STOPPED_F - DOF_F_NUMBER) * t;
        }
        float apertureM = focalLengthM / fNumber;

        lastCocScale = ffxDofCalculateCocScale(apertureM,
                                               focusSigned,
                                               focalLengthM,
                                               conversion,
                                               proj33,
                                               proj34,
                                               proj43);
        lastCocBias  = ffxDofCalculateCocBias(apertureM,
                                              focusSigned,
                                              focalLengthM,
                                              conversion,
                                              proj33,
                                              proj34,
                                              proj43);
        lastCocLimit = 0.5f * DOF_COC_LIMIT_FACTOR * (float)window.renderHeight;

        VulkanCommand* cmd = vulkan.currentCmd;
        vulkanTransition(cmd, color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, &dofOutput, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

        FfxDofDispatchDescription dispatch = {};
        dispatch.commandList               = ffxGetCommandListVK(cmd->cmd);
        dispatch.color                     = wrapImageResource(color,
                                                               FFX_RESOURCE_USAGE_READ_ONLY,
                                                               FFX_RESOURCE_STATE_COMPUTE_READ,
                                                               L"dof_color");
        dispatch.depth                     = wrapImageResource(depth,
                                                               FFX_RESOURCE_USAGE_READ_ONLY,
                                                               FFX_RESOURCE_STATE_COMPUTE_READ,
                                                               L"dof_depth");
        dispatch.output                    = wrapImageResource(&dofOutput,
                                                               FFX_RESOURCE_USAGE_UAV,
                                                               FFX_RESOURCE_STATE_UNORDERED_ACCESS,
                                                               L"dof_output");
        dispatch.cocScale                  = lastCocScale;
        dispatch.cocBias                   = lastCocBias;

        vulkanBeginProfile(cmd, &profile, 0);
        FfxErrorCode result = ffxDofContextDispatch(&context, &dispatch);
        vulkanEndProfile(cmd, &profile, 0);

        if (result != FFX_OK) {
            utils::error("vulkanDofPass: ffxDofContextDispatch failed: %d", result);
            destroyContext();
            contextBroken = 1;
            return;
        }

        vulkanTransition(cmd, &dofOutput, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        dispatchedThisFrame = 1;
        elapsedGPU          = profile.elapsed;
    }

    void VulkanDofPass::postUpdate() {
        vulkanDofPass.cpuElapsed = elapsedCPU;
        vulkanDofPass.gpuElapsed = elapsedGPU;
        elapsedCPU               = utils::nanos();
        elapsedCPU               = utils::nanos() - elapsedCPU;
    }

    void VulkanDofPass::removed() {
        destroyContext();
        destroyOutput();
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
        if (cocMaskPipeReady) {
            vulkanDestroyPipe(&cocMaskPipe);
            cocMaskPipe      = VulkanPipe{};
            cocMaskPipeReady = 0;
        }
    }

    VulkanImage* vulkanDofPassGetOutput(void) {
        return dispatchedThisFrame ? &dofOutput : NULL;
    }

    void vulkanDofPassSetDisabled(char disabled) {
        dofDisabled = disabled;
    }

    char vulkanDofPassIsDisabled(void) {
        return dofDisabled;
    }

    void vulkanDofPassSetFocusDistance(float meters) {
        focusDistance = meters > 0.0f ? meters : 0.0f;
    }

    float vulkanDofPassGetFocusDistance(void) {
        return focusDistance;
    }

    void vulkanDofPassSetQuality(int rings) {
        if (rings < 1) rings = 1;
        if (rings > 8) rings = 8;
        if (rings != qualityRings) {
            qualityRings = rings;
        }
    }

    int vulkanDofPassGetQuality(void) {
        return qualityRings;
    }

    void vulkanDofPassApplyReactiveMask(VulkanCommand* cmd, VulkanImage* depth) {
        if (vulkan.skipFrame || dofDisabled || !cocMaskPipeReady) {
            return;
        }
        VulkanImage* mask = vulkanFsrPassGetReactiveMaskImage();
        if (!mask || !depth || !mask->img) {
            return;
        }

        /* The FSR pass leaves the mask in GENERAL layout after its own
         * reactive dispatch; the CoC mask max-blends into it before the FSR
         * pass transitions it to SHADER_READ_ONLY for the upscaler. */
        vulkanBindPipe(cmd, &cocMaskPipe);

        struct CocMaskPushConstants {
            u32 depthIndex;
            u32 maskIndex;
            float cocScale;
            float cocBias;
            float cocLimit;
            u32 width;
            u32 height;
        } pc = {
            .depthIndex = (u32)depth->sampledPoolIndex,
            .maskIndex  = (u32)mask->storagePoolIndex,
            .cocScale   = lastCocScale,
            .cocBias    = lastCocBias,
            .cocLimit   = lastCocLimit,
            .width      = mask->extent.width,
            .height     = mask->extent.height,
        };

        vulkanPush(cmd, &cocMaskPipe, sizeof(pc), &pc);

        u32 groupsX = (mask->extent.width + 7) / 8;
        u32 groupsY = (mask->extent.height + 7) / 8;
        vulkanDispatch(cmd, &cocMaskPipe, groupsX, groupsY, 1);
    }

}  // namespace engine