#include "VulkanSpd.h"

#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/barrier/VulkanBarrier.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/resources/VulkanBuffer.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <FidelityFX/host/ffx_spd.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#pragma GCC diagnostic pop

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace engine {

/* ── FFX backend + context (lazy, single MEAN/LOAD context) ─────────────── */

static void*          scratchBuffer;
static size_t         scratchBufferSize;
static FfxInterface   backendInterface;
static char           backendReady;
static FfxSpdContext spdContext;
static char           contextReady;
static char           contextBroken; /* creation failed — don't retry per frame */

static VkImageCreateInfo makeImageCreateInfo(VulkanImage* image) {
    VkImageCreateInfo info       = {};
    info.sType                   = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType               = VK_IMAGE_TYPE_2D;
    info.format                  = image->format;
    info.extent                  = image->extent;
    info.mipLevels               = (u32)image->mipLevels;
    info.arrayLayers             = (u32)image->layers;
    info.samples                 = image->samples;
    info.tiling                  = VK_IMAGE_TILING_OPTIMAL;
    info.usage                   = image->usage;
    info.sharingMode             = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    return info;
}

static char ensureContext(void) {
    if (contextReady) return 1;
    if (contextBroken) return 0;

    if (!backendReady) {
        scratchBufferSize = ffxGetScratchMemorySizeVK(vulkan.physicalDevice, FFX_SPD_CONTEXT_COUNT);
        scratchBuffer     = calloc(1, scratchBufferSize);
        if (!scratchBuffer) {
            utils::error("vulkanSpd: failed to allocate %zu bytes of backend scratch memory",
                         scratchBufferSize);
            contextBroken = 1;
            return 0;
        }

        VkDeviceContext deviceContext = {
            .vkDevice         = vulkan.device,
            .vkPhysicalDevice = vulkan.physicalDevice,
            .vkDeviceProcAddr = vkGetDeviceProcAddr,
        };
        FfxDevice   device        = ffxGetDeviceVK(&deviceContext);
        FfxErrorCode backendResult = ffxGetInterfaceVK(
            &backendInterface, device, scratchBuffer, scratchBufferSize, FFX_SPD_CONTEXT_COUNT);
        if (backendResult != FFX_OK) {
            utils::error("vulkanSpd: ffxGetInterfaceVK failed: %d", backendResult);
            free(scratchBuffer);
            scratchBuffer     = nullptr;
            scratchBufferSize = 0;
            contextBroken     = 1;
            return 0;
        }
        backendReady = 1;
    }

    FfxSpdContextDescription desc = {};
    /* LOAD sampling (imageLoad of mip 0, no sampler dependency), LDS wave
     * interop (portable; wave-ops permutation is the faster alternative on
     * RDNA), f32 math. MEAN = box filter — the standard mip-chain filter. */
    desc.flags            = FFX_SPD_SAMPLER_LOAD | FFX_SPD_WAVE_INTEROP_LDS;
    desc.downsampleFilter = FFX_SPD_DOWNSAMPLE_FILTER_MEAN;
    desc.backendInterface = backendInterface;

    FfxErrorCode result = ffxSpdContextCreate(&spdContext, &desc);
    if (result != FFX_OK) {
        utils::error("vulkanSpd: ffxSpdContextCreate failed: %d", result);
        spdContext   = FfxSpdContext{};
        contextBroken = 1;
        return 0;
    }

    contextReady = 1;
    utils::info("vulkanSpd: created SPD context (MEAN, LOAD, LDS)");
    return 1;
}

char vulkanSpdGenerateMips(VulkanCommand* cmd, VulkanImage* image) {
    if (!cmd || !image || !image->img) return 0;
    if (image->format != VK_FORMAT_R16G16B16A16_SFLOAT) {
        utils::error("vulkanSpd: unsupported format %d (needs R16G16B16A16_SFLOAT — "
                     "the fork's shader blobs are compiled for rgba16f)",
                     (int)image->format);
        return 0;
    }
    if (image->mipLevels < 2 || image->mipLevels > 13) {
        utils::error("vulkanSpd: mipLevels %d out of range [2, 13] (SPD_MAX_MIP_LEVELS)",
                     image->mipLevels);
        return 0;
    }
    if (!(image->usage & VK_IMAGE_USAGE_STORAGE_BIT)) {
        utils::error("vulkanSpd: image lacks STORAGE usage");
        return 0;
    }
    if (!ensureContext()) return 0;

    /* GENERAL covers both the compute reads (mip 0) and UAV writes (mips
     * 1..N); FFX's backend inserts its own pipeline barriers on top. */
    vulkanTransition(cmd, image, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

    VkImageCreateInfo createInfo   = makeImageCreateInfo(image);
    /* ARRAYVIEW: SPD's GLSL declares its UAVs as image2DArray; for non-array
     * images the backend would create VK_IMAGE_VIEW_TYPE_2D views, which is
     * a validation error against Arrayed=1 OpTypeImage. A 2D_ARRAY view on a
     * single-layer image is legal Vulkan. */
    FfxResourceUsage usage        = FfxResourceUsage(FFX_RESOURCE_USAGE_READ_ONLY |
                                             FFX_RESOURCE_USAGE_UAV |
                                             FFX_RESOURCE_USAGE_ARRAYVIEW);
    FfxResourceDescription ffxDesc = ffxGetImageResourceDescriptionVK(image->img, createInfo, usage);

    FfxSpdDispatchDescription dispatch = {};
    dispatch.commandList               = ffxGetCommandListVK(cmd->cmd);
    dispatch.resource                  = ffxGetResourceVK(image->img,
                                          ffxDesc,
                                          L"spd_input",
                                          FFX_RESOURCE_STATE_UNORDERED_ACCESS);

    FfxErrorCode result = ffxSpdContextDispatch(&spdContext, &dispatch);
    if (result != FFX_OK) {
        utils::error("vulkanSpd: ffxSpdContextDispatch failed: %d", result);
        return 0;
    }

    vulkanTransition(cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    return 1;
}

void vulkanSpdDestroy(void) {
    if (contextReady) {
        ffxSpdContextDestroy(&spdContext);
        spdContext   = FfxSpdContext{};
        contextReady = 0;
    }
    contextBroken = 0;
    if (scratchBuffer) {
        free(scratchBuffer);
        scratchBuffer     = nullptr;
        scratchBufferSize = 0;
    }
    backendInterface = FfxInterface{};
    backendReady     = 0;
}

/* ── Self test (ENGINE_SPD_SELFTEST=1) ──────────────────────────────────── */

static u16 floatToHalfBits(float f) {
    /* Only used for the exact values 0.25 / 0.75 — a full converter is
     * overkill; assert via the readback side instead. */
    u32 bits;
    memcpy(&bits, &f, 4);
    u32 sign = (bits >> 16) & 0x8000;
    u32 exp  = (bits >> 23) & 0xff;
    u32 man  = bits & 0x7fffff;
    u16 half = (u16)(sign | (((exp - 127 + 15) & 0x1f) << 10) | (man >> 13));
    return half;
}

static float halfBitsToFloat(u16 h) {
    u32 sign = (u32)(h & 0x8000) << 16;
    u32 exp  = ((h >> 10) & 0x1f) + 112;
    u32 man  = (u32)(h & 0x3ff) << 13;
    /* denormals/zero/inf not needed for the test values */
    u32 bits = sign | (exp << 23) | man;
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

void vulkanSpdRunSelfTest(void) {
    const char* env = getenv("ENGINE_SPD_SELFTEST");
    if (!env || !atoi(env)) return;

    const u32    size       = 256;
    const u32    mips       = 9; /* 256 -> 1 */
    const size_t mip0Bytes  = (size_t)size * size * 8;
    const size_t mip1Bytes  = (size_t)(size / 2) * (size / 2) * 8;
    const size_t lastBytes  = 8;

    /* 1×1 checker of 0.25/0.75 in every channel: every 2×2 neighborhood
     * holds exactly two lo + two hi texels, so every mip >= 1 must be
     * exactly 0.5 (all values are exact f16, mean of powers of two). */
    std::vector<u16> pattern(mip0Bytes / 2);
    const u16 lo = floatToHalfBits(0.25f);
    const u16 hi = floatToHalfBits(0.75f);
    for (u32 y = 0; y < size; y++) {
        for (u32 x = 0; x < size; x++) {
            u16 v = (((x + y) % 2) == 0) ? lo : hi;
            for (int c = 0; c < 4; c++) pattern[((size_t)y * size + x) * 4 + c] = v;
        }
    }

    VulkanImage img = vulkanCreateImage(.name      = "SpdSelfTest",
                                        .format    = VK_FORMAT_R16G16B16A16_SFLOAT,
                                        .usage     = VK_IMAGE_USAGE_SAMPLED_BIT |
                                                 VK_IMAGE_USAGE_STORAGE_BIT |
                                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                        .width     = (int)size,
                                        .height    = (int)size,
                                        .mipLevels = (int)mips);

    VulkanBuffer readback = vulkanCreateReadbackBuffer("spd_selftest", mip1Bytes + lastBytes, 0);

    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanTransition(cmd, &img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);
    vulkanCopy(.cmd = cmd, .target.img = &img, .source.data = pattern.data(),
               .size = (u32)mip0Bytes);

    char ok = vulkanSpdGenerateMips(cmd, &img);

    if (ok) {
        /* Per-mip copies (the generic copy helper only covers mip 0). */
        vulkanTransition(cmd, &img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1);
        VkBufferImageCopy regions[2] = {};
        regions[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        regions[0].imageSubresource.mipLevel   = 1;
        regions[0].imageSubresource.layerCount = 1;
        regions[0].imageExtent                 = {size / 2, size / 2, 1};
        regions[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        regions[1].imageSubresource.mipLevel   = mips - 1;
        regions[1].imageSubresource.layerCount = 1;
        regions[1].imageExtent                 = {1, 1, 1};
        regions[1].bufferOffset                = mip1Bytes;
        vkCmdCopyImageToBuffer(cmd->cmd, img.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback.buf, 2, regions);
        vulkanBarrier(cmd, DEVICE_WRITE_TO_HOST_READ);
    }
    vulkanTransientEnd(cmd, 1);

    if (ok) {
        const u16* mip1 = static_cast<const u16*>(readback.vmaInfo.pMappedData);
        const u16* last = static_cast<const u16*>(readback.vmaInfo.pMappedData) + mip1Bytes / 2;

        char fail = 0;
        for (size_t i = 0; i < mip1Bytes / 2 && !fail; i++) {
            float v = halfBitsToFloat(mip1[i]);
            if (std::fabs(v - 0.5f) > 1e-3f) fail = 1;
        }
        for (int c = 0; c < 4 && !fail; c++) {
            float v = halfBitsToFloat(last[c]);
            if (std::fabs(v - 0.5f) > 1e-3f) fail = 1;
        }

        if (fail) {
            utils::error("vulkanSpd selftest: FAIL — mip content deviates from expected 0.5 "
                         "(first mip1 half = %f)",
                         mip1 ? halfBitsToFloat(mip1[0]) : -1.0f);
        } else {
            utils::info("vulkanSpd selftest: PASS — %ux%u checker downsampled to %u mips, "
                        "mip1..%u verified uniform 0.5",
                        size,
                        size,
                        mips,
                        mips - 1);
        }
    }

    vulkanDestroyBuffer(&readback, nullptr);
    vulkanDestroyImage(&img, nullptr);
}

}  // namespace engine
