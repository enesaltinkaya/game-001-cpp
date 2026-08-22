#include "VulkanLensPass.h"

#include "ecs/Ecs.h"
#include "events/Events.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanProfile.h"
#include "renderer/vulkan/resources/VulkanBuffer.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/swapchain/VulkanSwapchain.h"
#include "ecs/system/window/WindowSystem.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <FidelityFX/host/ffx_lens.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#pragma GCC diagnostic pop

#include <cmath>
#include <cstdlib>

namespace engine {

/* AMD FidelityFX Lens — grain / vignette / chromatic aberration, applied in
 * the DISPLAY-REFERRED domain (sRGB-encoded LDR), after the Final pass and
 * before any UI:
 *
 *   Final (unchanged) -> LensInput (SRGB attachment, auto-encoded)
 *   Lens dispatch: LensInput (SRV, auto-decoded to linear) -> LensOutput
 *                  (UNORM storage, linear bytes)
 *   blit LensOutput -> swapchain (format-aware: swizzle + sRGB encode)
 *   RmlUI composites UI on the swapchain, unaffected by lens effects
 *
 * The copy exists because the lens shader's storage-image write cannot
 * encode sRGB (storage writes are raw), and the swapchain has no STORAGE
 * usage (nor is it guaranteed supported). Both intermediates are UNORM
 * holding sRGB-encoded bytes, so the copy is format-compatible (same texel
 * size) and byte-identical. See docs/fsr3.1.md for the fork's rgba8 UAV
 * patch that makes the 8-bit output legal. */

static void swapchainCreated(void* _);
static void createImages(void);
static void destroyImages(void);
static void destroyContext(void);
static VkImageCreateInfo makeImageCreateInfo(VulkanImage* image);
static FfxResource wrapImageResource(VulkanImage* image, FfxResourceUsage usage,
                                     FfxResourceStates state, const wchar_t* name);

static double elapsedCPU;
static double elapsedGPU;

static VulkanImage lensInput;   /* UNORM, Final's render target    */
static VulkanImage lensOutput;  /* UNORM, lens storage output      */
static u32        outputWidth;
static u32        outputHeight;

static void*        scratchBuffer;
static size_t       scratchBufferSize;
static FfxInterface backendInterface;
static char         backendReady;
static FfxLensContext lensContext;
static char         contextReady;
static char         contextBroken;

static VulkanProfile profile;
static char         profileReady;

static char  lensDisabled;
static float grainAmount  = 0.15f;
static float chromAb      = 0.10f;
static float vignette     = 0.25f;
static float grainScale   = 2.0f; /* fixed; finer/larger grain scale */
static u32   frameCounter = 0;    /* grain seed — animated per frame */
static u64   frameSeq     = 0;    /* bumped every frame (preUpdate)    */
static u64   renderedSeq  = 0;    /* frameSeq when Final last rendered */

VulkanLensPass vulkanLensPass;

VulkanLensPass::VulkanLensPass() : System("lens") {}

void VulkanLensPass::added() {
    utils::signalSubscribe("swapchainCreated", swapchainCreated);

    lensDisabled = utils::settingsGetBool("lensEnabled") ? 0 : 1;
    grainAmount  = (float)utils::settingsGetDouble("lensGrain");
    chromAb      = (float)utils::settingsGetDouble("lensChromAb");
    vignette     = (float)utils::settingsGetDouble("lensVignette");
    auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    grainAmount  = clamp01(grainAmount);
    chromAb      = clamp01(chromAb);
    vignette     = clamp01(vignette);

    /* The swapchain (and window size) already exist when passes are added —
     * the swapchainCreated signal only fires on later recreations. */
    createImages();

    profile      = vulkanCreateProfile("lens");
    profileReady = 1;
}

void VulkanLensPass::preUpdate() {
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
    /* SRGB so the Final pass's attachment store encodes — same contract as
     * the swapchain, so Final uses its regular (unchanged) pipeline. */
    lensInput =
        vulkanCreateImage(.name   = "LensInput",
                          .format = VK_FORMAT_B8G8R8A8_SRGB,
                          .usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT,
                          .width  = window.width,
                          .height = window.height);
    lensOutput =
        vulkanCreateImage(.name   = "LensOutput",
                          .format = VK_FORMAT_R8G8B8A8_UNORM,
                          .usage  = VK_IMAGE_USAGE_STORAGE_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT,
                          .width  = window.width,
                          .height = window.height);
    outputWidth  = (u32)window.width;
    outputHeight = (u32)window.height;

    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanTransition(cmd, &lensInput, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    vulkanTransition(cmd, &lensOutput, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransientEnd(cmd, 1);
    utils::info("vulkanLensPass: created intermediates %ux%u", outputWidth, outputHeight);
}

static void swapchainCreated(void* _) {
    (void)_;
    destroyContext();
    createImages();
}

static void destroyImages(void) {
    if (lensInput.img) {
        vulkanDestroyImage(&lensInput, NULL);
        lensInput = VulkanImage{};
    }
    if (lensOutput.img) {
        vulkanDestroyImage(&lensOutput, NULL);
        lensOutput = VulkanImage{};
    }
    outputWidth  = 0;
    outputHeight = 0;
}

static void destroyContext(void) {
    if (contextReady) {
        ffxLensContextDestroy(&lensContext);
        lensContext = FfxLensContext{};
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

char vulkanLensPassIsActive(void) {
    return !lensDisabled && lensInput.img != VK_NULL_HANDLE;
}

VulkanImage* vulkanLensPassGetInput(void) {
    return lensInput.img ? &lensInput : NULL;
}

void vulkanLensPassMarkRendered(void) {
    renderedSeq = frameSeq;
}

void VulkanLensPass::update() {
    if (vulkan.skipFrame || lensDisabled || renderedSeq != frameSeq) {
        return;
    }

    VulkanImage* swapImage = vulkanSwapchain.currentSwapchainImage;
    if (!swapImage || !lensInput.img || outputWidth != (u32)window.width ||
        outputHeight != (u32)window.height) {
        return;
    }

    /* Lazy backend + context creation. */
    if (!contextReady) {
        if (contextBroken) {
            return;
        }
        if (!backendReady) {
            scratchBufferSize = ffxGetScratchMemorySizeVK(vulkan.physicalDevice, FFX_LENS_CONTEXT_COUNT);
            scratchBuffer     = calloc(1, scratchBufferSize);
            if (!scratchBuffer) {
                utils::error("vulkanLensPass: failed to allocate %zu bytes of backend scratch memory",
                             scratchBufferSize);
                contextBroken = 1;
                return;
            }
            VkDeviceContext deviceContext = {
                .vkDevice         = vulkan.device,
                .vkPhysicalDevice = vulkan.physicalDevice,
                .vkDeviceProcAddr = vkGetDeviceProcAddr,
            };
            FfxDevice device        = ffxGetDeviceVK(&deviceContext);
            FfxErrorCode backendResult = ffxGetInterfaceVK(
                &backendInterface, device, scratchBuffer, scratchBufferSize, FFX_LENS_CONTEXT_COUNT);
            if (backendResult != FFX_OK) {
                utils::error("vulkanLensPass: ffxGetInterfaceVK failed: %d", backendResult);
                free(scratchBuffer);
                scratchBuffer     = nullptr;
                scratchBufferSize = 0;
                contextBroken     = 1;
                return;
            }
            backendReady = 1;
        }

        FfxLensContextDescription desc = {};
        desc.flags            = FFX_LENS_MATH_NONPACKED; /* f32 */
        desc.outputFormat     = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
        desc.floatPrecision   = FFX_LENS_FLOAT_PRECISION_32BIT;
        desc.backendInterface = backendInterface;

        FfxErrorCode result = ffxLensContextCreate(&lensContext, &desc);
        if (result != FFX_OK) {
            utils::error("vulkanLensPass: ffxLensContextCreate failed: %d", result);
            lensContext   = FfxLensContext{};
            contextBroken = 1;
            return;
        }
        contextReady = 1;
        utils::info("vulkanLensPass: created lens context (%ux%u)", outputWidth, outputHeight);
    }

    VulkanCommand* cmd = vulkan.currentCmd;

    /* Final rendered into lensInput (COLOR_ATTACHMENT_OPTIMAL); stage for
     * the compute read, and the UAV target to GENERAL. */
    vulkanTransition(cmd, &lensInput, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, &lensOutput, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

    FfxLensDispatchDescription dispatch = {};
    dispatch.commandList  = ffxGetCommandListVK(cmd->cmd);
    dispatch.resource     = wrapImageResource(&lensInput, FFX_RESOURCE_USAGE_READ_ONLY,
                                              FFX_RESOURCE_STATE_COMPUTE_READ, L"lens_input");
    dispatch.resourceOutput = wrapImageResource(&lensOutput, FFX_RESOURCE_USAGE_UAV,
                                                FFX_RESOURCE_STATE_UNORDERED_ACCESS, L"lens_output");
    dispatch.renderSize.width  = outputWidth;
    dispatch.renderSize.height = outputHeight;
    dispatch.grainScale        = grainScale;
    dispatch.grainAmount       = grainAmount;
    dispatch.grainSeed         = frameCounter++;
    dispatch.chromAb           = chromAb;
    dispatch.vignette          = vignette;

    vulkanBeginProfile(cmd, &profile, 0);
    FfxErrorCode result = ffxLensContextDispatch(&lensContext, &dispatch);
    vulkanEndProfile(cmd, &profile, 0);

    if (result != FFX_OK) {
        utils::error("vulkanLensPass: ffxLensContextDispatch failed: %d", result);
        destroyContext();
        return;
    }

    /* Blit (not copy) into the swapchain: the blit is format-aware — it
     * converts UNORM (linear bytes) to the sRGB swapchain, encoding on
     * write and swizzling channels correctly. A raw copy would neither
     * encode nor swizzle (R/B would swap between RGBA/BGRA byte orders). */
    vulkanTransition(cmd, &lensOutput, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1);
    vulkanTransition(cmd, swapImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);
    vulkanBlit(cmd, &lensOutput, swapImage);
    vulkanTransition(cmd, swapImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);

    /* Put lensInput back into the attachment-tracked layout: Final's render
     * pass does not update the engine's per-image layout tracking, so a
     * SHADER_READ-only tracked state would no-op next frame's transition
     * while the attachment use changed the actual layout (validation
     * catches exactly that mismatch). */
    vulkanTransition(cmd, &lensInput, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);

    elapsedGPU = profile.elapsed;
}

void VulkanLensPass::postUpdate() {
    vulkanLensPass.cpuElapsed = elapsedCPU;
    vulkanLensPass.gpuElapsed = elapsedGPU;
    elapsedCPU                = utils::nanos();
    elapsedCPU                = utils::nanos() - elapsedCPU;
}

void VulkanLensPass::removed() {
    destroyContext();
    destroyImages();
    if (scratchBuffer) {
        free(scratchBuffer);
        scratchBuffer     = nullptr;
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
}

void vulkanLensPassSetDisabled(char disabled) {
    lensDisabled = disabled;
}

char vulkanLensPassIsDisabled(void) {
    return lensDisabled;
}

void vulkanLensPassSetGrain(float amount) {
    grainAmount = amount < 0.0f ? 0.0f : (amount > 1.0f ? 1.0f : amount);
}

void vulkanLensPassSetChromAb(float amount) {
    chromAb = amount < 0.0f ? 0.0f : (amount > 1.0f ? 1.0f : amount);
}

void vulkanLensPassSetVignette(float amount) {
    vignette = amount < 0.0f ? 0.0f : (amount > 1.0f ? 1.0f : amount);
}

float vulkanLensPassGetGrain(void) {
    return grainAmount;
}

float vulkanLensPassGetChromAb(void) {
    return chromAb;
}

float vulkanLensPassGetVignette(void) {
    return vignette;
}

}  // namespace engine
