#include "VulkanLpmPass.h"

#include "ecs/Ecs.h"
#include "events/Events.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanProfile.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/swapchain/VulkanSwapchain.h"
#include "renderer/vulkan/pass/lens/VulkanLensPass.h"
#include "ecs/system/window/WindowSystem.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <FidelityFX/host/ffx_lpm.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#pragma GCC diagnostic pop

#include <cstdlib>

namespace engine {

/* AMD FidelityFX LPM (Luma Preserving Mapper) — tone + gamut mapping in
 * the display-referred domain. Replaces the engine's custom tonemapping
 * curves (AgX/ACES/...) that used to run in the Final pass fragment
 * shader:
 *
 *   Final (raster): scene HDR + bloom + exposure -> LpmInput (R16F, linear)
 *   LPM (compute):  LpmInput -> LpmOutput (R8G8B8A8_UNORM) — LPM's LDR
 *                   display mode applies the display gamma itself, so the
 *                   stored bytes are sRGB-ready
 *   blit:           LpmOutput -> lens input (when the lens pass is active)
 *                   or the swapchain. The blit is format-compatible
 *                   (same texel size, channel swizzle only — no color-space
 *                   conversion, the bytes are final), the same pattern the
 *                   lens pass uses for its UNORM -> sRGB swapchain copy.
 *
 * LPM parameters follow the FFX SDK sample defaults (git/samples/lpm):
 * shoulder=1, softGap=0, contrast=0.3, shoulderContrast=1, saturation=0,
 * crosstalk=(1, 1/2, 1/32), REC709 working space, LDR display mode.
 * hdrMax defines the tone curve's input range and is tuned to the scene's
 * HDR scale (sun radiance ~2.6, sun disc ~80 pre-bloom). All five
 * tuning knobs are runtime parameters (vulkanLpmPassGetParams/SetParams),
 * live-tunable from the debug GUI.
 */

static void swapchainCreated(void* _);
static void createImages(void);
static void destroyImages(void);
static void destroyContext(void);
static VkImageCreateInfo makeImageCreateInfo(VulkanImage* image);
static FfxResource wrapImageResource(VulkanImage* image, FfxResourceUsage usage,
                                     FfxResourceStates state, const wchar_t* name);

static VulkanLpmParams lpmParams = {
    .contrast         = 0.3f,  /* FFX sample default */
    .hdrMax           = 4.0f,  /* scene HDR scale (sun radiance ~2.6) */
    .shoulderContrast = 1.0f,
    .saturation       = 0.0f,
    .lpmExposure      = 1.0f,
};

static double elapsedCPU;
static double elapsedGPU;

static VulkanImage lpmInput;   /* R16F, Final's render target    */
static VulkanImage lpmOutput;  /* UNORM, LPM storage output      */
static u32        outputWidth;
static u32        outputHeight;

static void*        scratchBuffer;
static size_t       scratchBufferSize;
static FfxInterface backendInterface;
static char         backendReady;
static FfxLpmContext lpmContext;
static char         contextReady;

static VulkanProfile profile;
static char         profileReady;

static u64 frameSeq    = 0;  /* bumped every frame (preUpdate)    */
static u64 renderedSeq = 0;  /* frameSeq when Final last rendered */

VulkanLpmPass vulkanLpmPass;

VulkanLpmPass::VulkanLpmPass() : System("lpm") {}

void VulkanLpmPass::added() {
    utils::signalSubscribe("swapchainCreated", swapchainCreated);

    /* The swapchain (and window size) already exist when passes are added —
     * the swapchainCreated signal only fires on later recreations. */
    createImages();

    profile      = vulkanCreateProfile("lpm");
    profileReady = 1;
}

void VulkanLpmPass::preUpdate() {
    frameSeq++;
    if (profileReady) {
        vulkanResetProfile(vulkan.currentCmd, &profile, 0);
    }
}

static void createImages(void) {
    destroyImages();
    if (window.width <= 0 || window.height <= 0) {
        return;
    }
    lpmInput =
        vulkanCreateImage(.name   = "LpmInput",
                          .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                          .usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT,
                          .width  = window.width,
                          .height = window.height);
    lpmOutput =
        vulkanCreateImage(.name   = "LpmOutput",
                          .format = VK_FORMAT_R8G8B8A8_UNORM,
                          .usage  = VK_IMAGE_USAGE_STORAGE_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT,
                          .width  = window.width,
                          .height = window.height);
    outputWidth  = (u32)window.width;
    outputHeight = (u32)window.height;

    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanTransition(cmd, &lpmInput, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    vulkanTransition(cmd, &lpmOutput, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    vulkanTransientEnd(cmd, 1);
    utils::info("vulkanLpmPass: created intermediates %ux%u", outputWidth, outputHeight);
}

static void swapchainCreated(void* _) {
    (void)_;
    destroyContext();
    createImages();
}

static void destroyImages(void) {
    if (lpmInput.img) {
        vulkanDestroyImage(&lpmInput, NULL);
        lpmInput = VulkanImage{};
    }
    if (lpmOutput.img) {
        vulkanDestroyImage(&lpmOutput, NULL);
        lpmOutput = VulkanImage{};
    }
    outputWidth  = 0;
    outputHeight = 0;
}

static void destroyContext(void) {
    if (contextReady) {
        ffxLpmContextDestroy(&lpmContext);
        lpmContext = FfxLpmContext{};
        contextReady = 0;
    }
}

static VkImageCreateInfo makeImageCreateInfo(VulkanImage* image) {
    VkImageCreateInfo info   = {};
    info.sType               = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType           = VK_IMAGE_TYPE_2D;
    info.format              = image->format;
    info.extent              = image->extent;
    info.mipLevels           = (u32)image->mipLevels;
    info.arrayLayers         = (u32)image->layers;
    info.samples             = image->samples;
    info.tiling              = VK_IMAGE_TILING_OPTIMAL;
    info.usage               = image->usage;
    info.sharingMode         = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout       = VK_IMAGE_LAYOUT_UNDEFINED;
    return info;
}

static FfxResource wrapImageResource(VulkanImage* image, FfxResourceUsage usage,
                                     FfxResourceStates state, const wchar_t* name) {
    VkImageCreateInfo createInfo = makeImageCreateInfo(image);
    FfxResourceDescription desc = ffxGetImageResourceDescriptionVK(image->img, createInfo, usage);
    return ffxGetResourceVK(image->img, desc, name, state);
}

struct VulkanImage* vulkanLpmPassGetInput(void) {
    return lpmInput.img ? &lpmInput : NULL;
}

const VulkanLpmParams* vulkanLpmPassGetParams(void) {
    return &lpmParams;
}

void vulkanLpmPassSetParams(const VulkanLpmParams* params) {
    if (params) {
        lpmParams = *params;
    }
}

void vulkanLpmPassMarkRendered(void) {
    renderedSeq = frameSeq;
}

void VulkanLpmPass::update() {
    if (vulkan.skipFrame || renderedSeq != frameSeq) {
        return;
    }

    VulkanImage* swapImage = vulkanSwapchain.currentSwapchainImage;
    /* Lens active: LPM's display-referred output feeds the lens input
     * (SRGB attachment, auto-decoded by the lens shader); the lens pass
     * then blits its result into the swapchain. Lens inactive: blit
     * straight into the swapchain. */
    char        lensActive = vulkanLensPassIsActive();
    VulkanImage* target    = lensActive ? vulkanLensPassGetInput() : swapImage;
    if (!target || !lpmInput.img || outputWidth != (u32)window.width ||
        outputHeight != (u32)window.height) {
        return;
    }

    /* Lazy backend + context creation. LPM is the display path — without
     * it nothing reaches the swapchain — so a creation failure is
     * fatal (same policy as the FSR upscaler's OOM terminate). */
    if (!contextReady) {
        if (!backendReady) {
            scratchBufferSize = ffxGetScratchMemorySizeVK(vulkan.physicalDevice, FFX_LPM_CONTEXT_COUNT);
            scratchBuffer     = calloc(1, scratchBufferSize);
            if (!scratchBuffer) {
                utils::terminate("vulkanLpmPass: failed to allocate %zu bytes of backend scratch memory",
                                 scratchBufferSize);
            }
            VkDeviceContext deviceContext = {
                .vkDevice         = vulkan.device,
                .vkPhysicalDevice = vulkan.physicalDevice,
                .vkDeviceProcAddr = vkGetDeviceProcAddr,
            };
            FfxDevice device        = ffxGetDeviceVK(&deviceContext);
            FfxErrorCode backendResult = ffxGetInterfaceVK(
                &backendInterface, device, scratchBuffer, scratchBufferSize, FFX_LPM_CONTEXT_COUNT);
            if (backendResult != FFX_OK) {
                utils::terminate("vulkanLpmPass: ffxGetInterfaceVK failed: %d", backendResult);
            }
            backendReady = 1;
        }

        FfxLpmContextDescription desc = {};
        desc.backendInterface = backendInterface;

        FfxErrorCode result = ffxLpmContextCreate(&lpmContext, &desc);
        if (result != FFX_OK) {
            utils::terminate("vulkanLpmPass: ffxLpmContextCreate failed: %d", result);
        }
        contextReady = 1;
        utils::info("vulkanLpmPass: created LPM context (%ux%u)", outputWidth, outputHeight);
    }

    VulkanCommand* cmd = vulkan.currentCmd;

    /* Final rendered into lpmInput (COLOR_ATTACHMENT_OPTIMAL); stage it
     * for the compute read, and the UAV target to GENERAL. */
    vulkanTransition(cmd, &lpmInput, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, &lpmOutput, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

    FfxLpmDispatchDescription dispatch = {};
    dispatch.commandList  = ffxGetCommandListVK(cmd->cmd);
    dispatch.inputColor   = wrapImageResource(&lpmInput, FFX_RESOURCE_USAGE_READ_ONLY,
                                              FFX_RESOURCE_STATE_COMPUTE_READ, L"lpm_input");
    dispatch.outputColor  = wrapImageResource(&lpmOutput, FFX_RESOURCE_USAGE_UAV,
                                              FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"lpm_output");
    dispatch.shoulder         = 1;
    dispatch.softGap          = 0.0f;
    dispatch.hdrMax           = lpmParams.hdrMax;
    /* Exposure is already applied to the HDR composite in the Final pass
     * (sceneBuffer.cameras[0].exposure). */
    dispatch.lpmExposure      = lpmParams.lpmExposure;
    dispatch.contrast         = lpmParams.contrast;
    dispatch.shoulderContrast = lpmParams.shoulderContrast;
    dispatch.saturation[0]    = lpmParams.saturation;
    dispatch.saturation[1]    = lpmParams.saturation;
    dispatch.saturation[2]    = lpmParams.saturation;
    dispatch.crosstalk[0]     = 1.0f;
    dispatch.crosstalk[1]     = 1.0f / 2.0f;
    dispatch.crosstalk[2]     = 1.0f / 32.0f;
    dispatch.colorSpace       = FfxLpmColorSpace::FFX_LPM_ColorSpace_REC709;
    dispatch.displayMode      = FfxLpmDisplayMode::FFX_LPM_DISPLAYMODE_LDR;
    /* Display primaries / luminance range are only consumed by the HDR
     * (PQ/scRGB) display modes; the LDR mode ignores them. */
    dispatch.displayRedPrimary[0]   = 0.64f;
    dispatch.displayRedPrimary[1]   = 0.33f;
    dispatch.displayGreenPrimary[0] = 0.30f;
    dispatch.displayGreenPrimary[1] = 0.60f;
    dispatch.displayBluePrimary[0]  = 0.15f;
    dispatch.displayBluePrimary[1]  = 0.06f;
    dispatch.displayWhitePoint[0]   = 0.3127f;
    dispatch.displayWhitePoint[1]   = 0.3290f;
    dispatch.displayMinLuminance    = 0.0f;
    dispatch.displayMaxLuminance    = 1.0f;

    vulkanBeginProfile(cmd, &profile, 0);
    FfxErrorCode result = ffxLpmContextDispatch(&lpmContext, &dispatch);
    vulkanEndProfile(cmd, &profile, 0);

    if (result != FFX_OK) {
        utils::error("vulkanLpmPass: ffxLpmContextDispatch failed: %d", result);
        destroyContext();
        /* Restore the layouts the frame staged (the blit below is skipped):
         * lpmInput must be back in the attachment layout for next frame's
         * Final render pass, lpmOutput in the UAV layout for next frame's
         * dispatch. */
        vulkanTransition(cmd, &lpmInput, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
        vulkanTransition(cmd, &lpmOutput, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        return;
    }

    /* Blit (not copy) into the target: the blit is format-aware — it
     * converts the UNORM (sRGB-encoded bytes) to the SRGB target with a
     * channel swizzle, byte-identical to the lens pass's swapchain copy. */
    vulkanTransition(cmd, &lpmOutput, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1);
    vulkanTransition(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);
    vulkanBlit(cmd, &lpmOutput, target);
    if (!lensActive) {
        /* The swapchain is rendered into by the UI pass next; put it back
         * in the attachment layout (same as the lens pass does). */
        vulkanTransition(cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    } else {
        /* The lens pass dispatches on frames where the LPM pass fed its
         * input. */
        vulkanLensPassMarkRendered();
    }

    /* Put lpmInput back into the attachment-tracked layout: the Final
     * render pass does not update the engine's per-image layout tracking,
     * so a SHADER_READ-only tracked state would no-op next frame's
     * transition while the attachment use changed the actual layout
     * (validation catches exactly that mismatch). */
    vulkanTransition(cmd, &lpmInput, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);

    elapsedGPU = profile.elapsed;
}

void VulkanLpmPass::postUpdate() {
    vulkanLpmPass.cpuElapsed = elapsedCPU;
    vulkanLpmPass.gpuElapsed = elapsedGPU;
    elapsedCPU               = utils::nanos();
    elapsedCPU               = utils::nanos() - elapsedCPU;
}

void VulkanLpmPass::removed() {
    destroyContext();
    destroyImages();
    if (scratchBuffer) {
        free(scratchBuffer);
        scratchBuffer     = nullptr;
        scratchBufferSize = 0;
    }
    backendInterface = FfxInterface{};
    backendReady     = 0;
    if (profileReady) {
        vulkanDestroyProfile(&profile);
        profile      = VulkanProfile{};
        profileReady = 0;
    }
}

}  // namespace engine