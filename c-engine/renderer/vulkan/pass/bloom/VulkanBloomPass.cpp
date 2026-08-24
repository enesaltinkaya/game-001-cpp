#include "VulkanBloomPass.h"
#include "VulkanBloomPass.h"
#include "events/Events.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pass/fsr/VulkanFsrPass.h"
#include "renderer/vulkan/pass/taa/VulkanTaaPass.h"
#include "renderer/vulkan/pass/dof/VulkanDofPass.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/utils/VulkanUtils.h"

namespace engine {

static double elapsedCPU;
static double elapsedGPU;
static char   bloomDisabled;

VulkanBloomPass vulkanBloomPass;

VulkanBloomPass::VulkanBloomPass() : System("bloom") {}

#define BLOOM_MIP_COUNT 6

static VulkanPipe downsamplePipe;
static VulkanPipe upsamplePipe;

static VulkanImage bloomImage;          /* actual image with BLOOM_MIP_COUNT mips   */
static VkImageView mipViews[BLOOM_MIP_COUNT];

/* Lightweight VulkanImage wrappers used only to register per-mip views in
 * the global sampled / storage descriptor pools.  Each shares the
 * underlying VkImage from bloomImage but has its own VkImageView. */
static VulkanImage mipSampledImages[BLOOM_MIP_COUNT];
static VulkanImage mipStorageImages[BLOOM_MIP_COUNT];

static u32 cachedWidth;
static u32 cachedHeight;

/* Tunable parameters */
static const float bloomThreshold     = 1.0f;
static const float bloomSoftKnee      = 0.5f;
static const float bloomIntensity     = 1.0f;
static const float bloomRadius        = 1.0f;
static const float bloomStrengthValue = 0.015f;

typedef struct BloomDownsamplePC {
    u32   srcIndex;
    u32   dstIndex;
    u32   srcWidth;
    u32   srcHeight;
    u32   dstWidth;
    u32   dstHeight;
    float threshold;
    float softKnee;
    u32   isPrefilter;
} BloomDownsamplePC;

typedef struct BloomUpsamplePC {
    u32   srcIndex;
    u32   dstIndex;
    u32   dstWidth;
    u32   dstHeight;
    float radius;
    float intensity;
} BloomUpsamplePC;

/* --------------------------------------------------------------------- */
/* Per-mip view helpers                                                  */
/* --------------------------------------------------------------------- */

static void createMipViews(void) {
    for (int i = 0; i < BLOOM_MIP_COUNT; i++) {
        VkImageViewCreateInfo viewInfo = {
            .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .flags    = 0,
            .pNext    = nullptr,
            .image    = bloomImage.img,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format   = (VkFormat)bloomImage.format,
            .components = {VK_COMPONENT_SWIZZLE_R,
                           VK_COMPONENT_SWIZZLE_G,
                           VK_COMPONENT_SWIZZLE_B,
                           VK_COMPONENT_SWIZZLE_A},
            .subresourceRange = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = (u32)i,
                .levelCount     = 1,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
        };
        vkCreateImageView(vulkan.device, &viewInfo, nullptr, &mipViews[i]);

        /* Register a sampled-only fake image for this mip view */
        mipSampledImages[i] = VulkanImage{
            .img      = bloomImage.img,
            .view     = mipViews[i],
            .format   = bloomImage.format,
            .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
            .extent   = bloomImage.extent,
            .layers   = 1,
            .mipLevels = 1,
            .usage    = VK_IMAGE_USAGE_SAMPLED_BIT,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .layout   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        vulkanAddImageToPool(&mipSampledImages[i]);

        /* Register a storage-only fake image for this mip view */
        mipStorageImages[i] = VulkanImage{
            .img      = bloomImage.img,
            .view     = mipViews[i],
            .format   = bloomImage.format,
            .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
            .extent   = bloomImage.extent,
            .layers   = 1,
            .mipLevels = 1,
            .usage    = VK_IMAGE_USAGE_STORAGE_BIT,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .layout   = VK_IMAGE_LAYOUT_GENERAL,
        };
        vulkanAddImageToPool(&mipStorageImages[i]);
#if 1 /* TEMP DEBUG */
        utils::info("POOLDBG bloom mip%u sampled=%u storage=%u", i,
                    mipSampledImages[i].sampledPoolIndex, mipStorageImages[i].storagePoolIndex);
#endif
    }
}

static void destroyMipViews(void) {
    for (int i = 0; i < BLOOM_MIP_COUNT; i++) {
        if (mipViews[i]) {
            vulkanRemoveImageFromPool(&mipSampledImages[i]);
            vulkanRemoveImageFromPool(&mipStorageImages[i]);
            vkDestroyImageView(vulkan.device, mipViews[i], NULL);
            mipViews[i]          = VK_NULL_HANDLE;
            mipSampledImages[i]  = VulkanImage{};
            mipStorageImages[i]  = VulkanImage{};
        }
    }
}

static void destroyBloom(void) {
    destroyMipViews();
    if (bloomImage.img) {
        vulkanDestroyImage(&bloomImage, NULL);
        bloomImage = VulkanImage{};
    }
    cachedWidth  = 0;
    cachedHeight = 0;
}

static void createBloom(u32 halfW, u32 halfH) {
    bloomImage = vulkanCreateImage(
        .name      = "BloomMipChain",
        .format    = VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        .usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .aspect    = VK_IMAGE_ASPECT_COLOR_BIT,
        .width     = (int)halfW,
        .height    = (int)halfH,
        .mipLevels = BLOOM_MIP_COUNT,
        .noPool    = 1);

    createMipViews();
    cachedWidth  = halfW;
    cachedHeight = halfH;
}

/* --------------------------------------------------------------------- */
/* Transition a single mip of the bloom image                            */
/* --------------------------------------------------------------------- */

static void transitionMip(VulkanCommand* cmd, int mip,
                           VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                           VkImageLayout oldLayout, VkImageLayout newLayout,
                           VkPipelineStageFlags srcStage,
                           VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier barrier = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext         = nullptr,
        .image         = bloomImage.img,
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = (u32)mip,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
        .oldLayout             = oldLayout,
        .newLayout             = newLayout,
        .srcQueueFamilyIndex   = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex   = VK_QUEUE_FAMILY_IGNORED,
        .srcAccessMask         = srcAccess,
        .dstAccessMask         = dstAccess,
    };
    vkCmdPipelineBarrier(cmd->cmd, srcStage, dstStage,
                         0, 0, NULL, 0, NULL, 1, &barrier);
}

/* --------------------------------------------------------------------- */
/* Lifecycle                                                             */
/* --------------------------------------------------------------------- */

static void swapchainCreated(void*) {
    destroyBloom();
}

void VulkanBloomPass::added() {
    utils::signalSubscribe("swapchainCreated", swapchainCreated);

    downsamplePipe = vulkanCreatePipe(
        .name = "bloom_downsample",
        .comp = "shaders/pass/bloom/spv/bloom_downsample.comp.spv");
    upsamplePipe = vulkanCreatePipe(
        .name = "bloom_upsample",
        .comp = "shaders/pass/bloom/spv/bloom_upsample.comp.spv");
}

void VulkanBloomPass::preUpdate() {
    if (vulkan.skipFrame) return;

    /* DOF (when active) is the resolved color for the whole chain — in the
     * upscaler path it is the pre-upscale image the FSR pass consumed. */
    VulkanImage* resolved = vulkanDofPassGetOutput();
    if (!resolved) {
        resolved = vulkanTaaPassGetOutput();
    }
    if (!resolved) {
        resolved = vulkanFsrPassGetOutput();
    }
    if (!resolved) {
        resolved = vulkanFrameResourcesGetCompositeColor();
    }
    if (!resolved) {
        resolved = vulkanFrameResourcesGetSceneColor();
    }
    if (!resolved) return;

    if (!bloomImage.img) {
        u32 halfW = resolved->extent.width  > 1 ? resolved->extent.width  / 2 : 1;
        u32 halfH = resolved->extent.height > 1 ? resolved->extent.height / 2 : 1;
        createBloom(halfW, halfH);
    }

    vulkanResetProfile(vulkan.currentCmd, &downsamplePipe.profile, 0);
    vulkanResetProfile(vulkan.currentCmd, &upsamplePipe.profile, 0);
}

void VulkanBloomPass::update() {
    if (vulkan.skipFrame) return;
    if (bloomDisabled) {
        vulkanBloomPass.gpuElapsed = 0;
        return;
    }

    VulkanImage* resolved = vulkanDofPassGetOutput();
    if (!resolved) {
        resolved = vulkanTaaPassGetOutput();
    }
    if (!resolved) {
        resolved = vulkanFsrPassGetOutput();
    }
    if (!resolved) {
        resolved = vulkanFrameResourcesGetCompositeColor();
    }
    if (!resolved) {
        resolved = vulkanFrameResourcesGetSceneColor();
    }
    if (!resolved || !bloomImage.img) return;

    VulkanCommand* cmd = vulkan.currentCmd;
    VkImageLayout reusedMipOldLayout =
        bloomImage.layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_IMAGE_LAYOUT_UNDEFINED
            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    u32 srcW = resolved->extent.width;
    u32 srcH = resolved->extent.height;

    vulkanBeginProfile(cmd, &downsamplePipe.profile, 0);

    if (utils::isDebug()) {
        vulkanLabelBeginColor(cmd, "bloom downsample", 1.0f, 0.8f, 0.2f, 1.0f);
    }

    /* ── Prefilter → mip 0 ─────────────────────────────────────────── */
    /* ResolvedColor is already SHADER_READ_ONLY_OPTIMAL from the composite pass. */
    transitionMip(cmd, 0,
                  reusedMipOldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                      ? VK_ACCESS_SHADER_READ_BIT : 0,
                  VK_ACCESS_SHADER_WRITE_BIT,
                  reusedMipOldLayout, VK_IMAGE_LAYOUT_GENERAL,
                  reusedMipOldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                      ? (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
                      : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    u32 dstW = cachedWidth;
    u32 dstH = cachedHeight;

    vulkanBindPipe(cmd, &downsamplePipe);
    {
        BloomDownsamplePC pc = {
            .srcIndex    = (u32)resolved->sampledPoolIndex,
            .dstIndex    = (u32)mipStorageImages[0].storagePoolIndex,
            .srcWidth    = srcW,
            .srcHeight   = srcH,
            .dstWidth    = dstW,
            .dstHeight   = dstH,
            .threshold   = bloomThreshold,
            .softKnee    = bloomSoftKnee,
            .isPrefilter = 1,
        };
        vulkanPush(cmd, &downsamplePipe, sizeof(pc), &pc);
        vulkanDispatch(cmd, &downsamplePipe, (dstW + 7) / 8, (dstH + 7) / 8, 1);
    }

    /* ── Downsample chain (mips 1–5) ───────────────────────────────── */
    srcW = dstW;
    srcH = dstH;

    for (int mip = 1; mip < BLOOM_MIP_COUNT; mip++) {
        dstW = srcW > 1 ? srcW >> 1 : 1;
        dstH = srcH > 1 ? srcH >> 1 : 1;

        /* Previous mip: GENERAL → SHADER_READ_ONLY_OPTIMAL */
        transitionMip(cmd, mip - 1,
                      VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        /* Current mip: old layout → GENERAL */
        transitionMip(cmd, mip,
                      reusedMipOldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                          ? VK_ACCESS_SHADER_READ_BIT : 0,
                      VK_ACCESS_SHADER_WRITE_BIT,
                      reusedMipOldLayout, VK_IMAGE_LAYOUT_GENERAL,
                      reusedMipOldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                          ? (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
                          : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        vulkanBindPipe(cmd, &downsamplePipe);
        {
            BloomDownsamplePC pc = {
                .srcIndex    = (u32)mipSampledImages[mip - 1].sampledPoolIndex,
                .dstIndex    = (u32)mipStorageImages[mip].storagePoolIndex,
                .srcWidth    = srcW,
                .srcHeight   = srcH,
                .dstWidth    = dstW,
                .dstHeight   = dstH,
                .threshold   = bloomThreshold,
                .softKnee    = bloomSoftKnee,
                .isPrefilter = 0,
            };
            vulkanPush(cmd, &downsamplePipe, sizeof(pc), &pc);
            vulkanDispatch(cmd, &downsamplePipe,
                           (dstW + 7) / 8, (dstH + 7) / 8, 1);
        }

        srcW = dstW;
        srcH = dstH;
    }

    /* Last mip: write → read barrier */
    transitionMip(cmd, BLOOM_MIP_COUNT - 1,
                  VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    if (utils::isDebug()) {
        vulkanLabelEnd(cmd);
    }

    vulkanEndProfile(cmd, &downsamplePipe.profile, 0);

    /* ── Upsample chain (mips 4→0) ─────────────────────────────────── */
    vulkanBeginProfile(cmd, &upsamplePipe.profile, 0);

    if (utils::isDebug()) {
        vulkanLabelBeginColor(cmd, "bloom upsample", 0.2f, 1.0f, 0.8f, 1.0f);
    }

    for (int mip = BLOOM_MIP_COUNT - 2; mip >= 0; mip--) {
        /* Destination mip: SHADER_READ_ONLY → GENERAL for read-modify-write */
        transitionMip(cmd, mip,
                      VK_ACCESS_SHADER_READ_BIT,
                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        u32 mipW = cachedWidth  >> mip;
        u32 mipH = cachedHeight >> mip;
        if (mipW < 1) mipW = 1;
        if (mipH < 1) mipH = 1;

        vulkanBindPipe(cmd, &upsamplePipe);
        {
            BloomUpsamplePC pc = {
                .srcIndex  = (u32)mipSampledImages[mip + 1].sampledPoolIndex,
                .dstIndex  = (u32)mipStorageImages[mip].storagePoolIndex,
                .dstWidth  = mipW,
                .dstHeight = mipH,
                .radius    = bloomRadius,
                .intensity = bloomIntensity,
            };
            vulkanPush(cmd, &upsamplePipe, sizeof(pc), &pc);
            vulkanDispatch(cmd, &upsamplePipe,
                           (mipW + 7) / 8, (mipH + 7) / 8, 1);
        }

        /* Barrier: mip write → read */
        transitionMip(cmd, mip,
                      VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }

    if (utils::isDebug()) {
        vulkanLabelEnd(cmd);
    }

    vulkanEndProfile(cmd, &upsamplePipe.profile, 0);

    /* Mip 0 is now in SHADER_READ_ONLY_OPTIMAL, ready for the Final pass. */
    bloomImage.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
#if 1 /* TEMP DEBUG */
    static u32 bloomFrame;
    if (getenv("ENGINE_DUMP_BLOOM") && bloomFrame > 3) {
        vulkanSaveImage(&bloomImage, "/tmp/dbg_bloom.jpg");
    }
    bloomFrame++;
#endif

    elapsedGPU = downsamplePipe.profile.elapsed + upsamplePipe.profile.elapsed;
}

void VulkanBloomPass::postUpdate() {
    vulkanBloomPass.cpuElapsed = elapsedCPU;
    vulkanBloomPass.gpuElapsed = elapsedGPU;
    elapsedCPU                 = utils::nanos();
    elapsedCPU                 = utils::nanos() - elapsedCPU;
}

void VulkanBloomPass::removed() {
    destroyBloom();
    vulkanDestroyPipe(&downsamplePipe);
    vulkanDestroyPipe(&upsamplePipe);
}

/* --------------------------------------------------------------------- */
/* Public API                                                            */
/* --------------------------------------------------------------------- */

int vulkanBloomPassGetBloomSampledIndex(void)
{
    if (bloomDisabled || !bloomImage.img) return 0;
    return mipSampledImages[0].sampledPoolIndex;
}

struct VulkanImage* vulkanBloomPassGetBloomImage(void)
{
    if (!bloomImage.img) return NULL;
    return &mipSampledImages[0];
}

float vulkanBloomPassGetStrength(void) {
    if (bloomDisabled) return 0.0f;
    return bloomStrengthValue;
}

void vulkanBloomPassSetDisabled(char disabled) {
    bloomDisabled = disabled;
    utils::info("Bloom: %s", bloomDisabled ? "disabled" : "enabled");
}

char vulkanBloomPassIsDisabled(void) {
    return bloomDisabled;
}
}  // namespace engine
