#include "VulkanAOPass.h"
#include "ecs/system/window/WindowSystem.h"
#include "events/Events.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pass/hiz/VulkanHiZPass.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include <stdlib.h>

namespace engine {

/* Mirrors VulkanHiZPass::MAX_HIZ_MIPS — the per-mip push-constant table is
 * sized to the full chain so the shader never has to index past it. */
#define MAX_HIZ_MIPS 16

static void swapchainCreated(void* _);
static void createAccumulators(void);
static void destroyAccumulators(void);
static float aoEnvHistoryWeight(void);
static void clearAccumulator(VulkanCommand* cmd, VulkanImage* img);

static double elapsedCPU;
static double elapsedGPU;
static char aoDisabled;

static VulkanPipe rayPipe;      // ao.comp — per-frame XeGTAO ray pass
static VulkanPipe temporalPipe; // ao_temporal.comp — G-TAO temporal accumulation

/* Ping-pong temporal accumulators (R16G16_SFLOAT: .r = occlusion,
 * .g = S-space inverse view depth, 0 = no data).  Same static-image +
 * swapchainCreated recreation pattern as TAA's taaA/taaB. */
static VulkanImage aoA;
static VulkanImage aoB;
static u32 aoWidth;
static u32 aoHeight;
static int aoFrame = 0;
static VulkanImage* aoCurrentOutput = NULL;

VulkanAOPass vulkanAOPass;

VulkanAOPass::VulkanAOPass() : System("ao") {}

/* Must match the GLSL push-constant block in ao.comp.  The per-mip HiZ
 * sampled pool indices are pushed because the bindless pool assigns each
 * mip its own image view (see VulkanHiZPass::createMipViews). */
typedef struct AoRayPushConstants {
    u32 depthIndex;
    u32 normalsIndex;
    u32 outputIndex;
    u32 width;
    u32 height;
    u32 hizMipCount;
    u32 _pad;
    u32 hizMipIndex[MAX_HIZ_MIPS];
} AoRayPushConstants;

/* Must match the GLSL push-constant block in ao_temporal.comp. */
typedef struct AoTemporalPushConstants {
    u32 aoIndex;
    u32 prevIndex;
    u32 outIndex;
    u32 velocityIndex;
    u32 depthIndex;
    u32 width;
    u32 height;
    float historyWeight;
    float depthThreshold;
} AoTemporalPushConstants;

void VulkanAOPass::added() {
    const char* env = getenv("ENGINE_AO_DISABLED");
    if (env && *env && atoi(env)) aoDisabled = 1;

    utils::signalSubscribe("swapchainCreated", swapchainCreated);

    rayPipe = vulkanCreatePipe(
        .name = "ao",
        .comp = "shaders/pass/ao/spv/ao.comp.spv");
    temporalPipe = vulkanCreatePipe(
        .name = "ao_temporal",
        .comp = "shaders/pass/ao/spv/ao_temporal.comp.spv");
}

void VulkanAOPass::preUpdate() {
    if (vulkan.skipFrame) {
        return;
    }
    vulkanResetProfile(vulkan.currentCmd, &rayPipe.profile, 0);
    vulkanResetProfile(vulkan.currentCmd, &temporalPipe.profile, 0);
}

static void swapchainCreated(void* _) {
    (void)_;
    destroyAccumulators();
    createAccumulators();
    aoFrame = 0;
}

static void createAccumulators(void) {
    if (window.renderWidth <= 0 || window.renderHeight <= 0) {
        return;
    }
    /* R16G16_SFLOAT: .r = accumulated AO, .g = S-space inverse view depth
     * (TAA's depthToInv; 0 = "no data" / sky).  TRANSFER_DST is required by
     * the initial clear (vkCmdClearColorImage); TRANSFER_SRC so the "ao"
     * debug dump can read the accumulator back. */
    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    aoA = vulkanCreateImage(.name   = "AoA",
                            .format = VK_FORMAT_R16G16_SFLOAT,
                            .usage  = usage,
                            .width  = window.renderWidth,
                            .height = window.renderHeight);
    aoB = vulkanCreateImage(.name   = "AoB",
                            .format = VK_FORMAT_R16G16_SFLOAT,
                            .usage  = usage,
                            .width  = window.renderWidth,
                            .height = window.renderHeight);
    aoWidth  = (u32)window.renderWidth;
    aoHeight = (u32)window.renderHeight;

    /* Clear both accumulators to (1.0, 0.0) = "no occlusion, no history"
     * so the first temporal blend degrades gracefully to the current frame
     * (S = 0 triggers rejection).  The images start in UNDEFINED layout;
     * transition them to their first real layout (GENERAL — how the update
     * path stages the write target before each dispatch) BEFORE clearing,
     * so vulkanClearColorImage' restore-back barrier targets a defined
     * layout (VUID 01198 forbids newLayout = UNDEFINED). */
    VulkanCommand* cmd = vulkanTransientBegin();
    clearAccumulator(cmd, &aoA);
    clearAccumulator(cmd, &aoB);
    vulkanTransientEnd(cmd, 1);
}

static void destroyAccumulators(void) {
    if (aoA.img) {
        vulkanDestroyImage(&aoA, NULL);
        aoA = VulkanImage{};
    }
    if (aoB.img) {
        vulkanDestroyImage(&aoB, NULL);
        aoB = VulkanImage{};
    }
    aoWidth  = 0;
    aoHeight = 0;
    aoCurrentOutput = NULL;
}

static void clearAccumulator(VulkanCommand* cmd, VulkanImage* img) {
    /* Stage the write target in GENERAL, then clear to (1.0, 0.0) =
     * "no occlusion, no history".  vulkanClearColorImage round-trips the
     * image through TRANSFER_DST_OPTIMAL and restores the previous layout. */
    vulkanTransition(cmd, img, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    VkClearColorValue clear;
    clear.float32[0] = 1.0f;
    clear.float32[1] = 0.0f;
    clear.float32[2] = 0.0f;
    clear.float32[3] = 0.0f;
    vulkanClearColorImage(cmd, img, clear);
}

void VulkanAOPass::update() {
    elapsedCPU = utils::nanos();

    if (vulkan.skipFrame) {
        elapsedCPU = utils::nanos() - elapsedCPU;
        return;
    }

    VulkanCommand* cmd        = vulkan.currentCmd;
    VulkanImage* depth        = vulkanFrameResourcesGetDepth();
    VulkanImage* normals      = vulkanFrameResourcesGetNormals();
    VulkanImage* velocity     = vulkanFrameResourcesGetVelocity();
    VulkanImage* perFrameAO   = vulkanFrameResourcesGetAO();
    if (!perFrameAO || !depth || !normals || !velocity) {
        elapsedCPU = utils::nanos() - elapsedCPU;
        return;
    }

    VulkanImage* prev = (aoFrame % 2) ? &aoA : &aoB;
    VulkanImage* out  = (aoFrame % 2) ? &aoB : &aoA;
    if (!aoA.img || !aoB.img || aoWidth != perFrameAO->extent.width ||
        aoHeight != perFrameAO->extent.height) {
        destroyAccumulators();
        createAccumulators();
        aoFrame = 0;
        prev = &aoA;
        out  = &aoB;
        if (!aoA.img || !aoB.img) {
            elapsedCPU = utils::nanos() - elapsedCPU;
            return;
        }
    }

    /* Disabled (or inputs missing above): clear the accumulator this frame
     * would have written so re-enabling starts from fresh history, and the
     * composite's "ao" debug dump reads clean 1.0s.  The composite skips the
     * multiply entirely while disabled (0xFFFFFFFF index), so the frame is
     * pixel-identical to pre-AO. */
    if (aoDisabled) {
        clearAccumulator(cmd, out);
        aoCurrentOutput = out;
        elapsedCPU = utils::nanos() - elapsedCPU;
        return;
    }

    /* Inputs readable.  Depth/normals were left SHADER_READ_ONLY by the
     * SSR pass; velocity may still be a render attachment — stage all of
     * them for the compute reads. */
    vulkanTransition(cmd, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, normals, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, velocity, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    /* ── Ray pass: per-frame XeGTAO → R8 per-frame buffer ── */
    vulkanBeginProfile(cmd, &rayPipe.profile, 0);
    vulkanTransition(cmd, perFrameAO, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    vulkanBindPipe(cmd, &rayPipe);

    VulkanImage* hiz = vulkanHiZGetCurrentImage();
    int mipCount = hiz ? vulkanHiZGetMipCount() : 1;
    if (mipCount > MAX_HIZ_MIPS) mipCount = MAX_HIZ_MIPS;

    AoRayPushConstants pc = {
        .depthIndex    = (u32)depth->sampledPoolIndex,
        .normalsIndex  = (u32)normals->sampledPoolIndex,
        .outputIndex   = (u32)perFrameAO->storagePoolIndex,
        .width         = perFrameAO->extent.width,
        .height        = perFrameAO->extent.height,
        .hizMipCount   = (u32)mipCount,
        ._pad          = 0,
        .hizMipIndex   = {0},
    };
    for (int m = 0; m < mipCount; m++) {
        pc.hizMipIndex[m] =
            hiz ? vulkanHiZGetMipSampledIndex(m) : (u32)depth->sampledPoolIndex;
    }
    vulkanPush(cmd, &rayPipe, sizeof(pc), &pc);

    u32 groupsX = (perFrameAO->extent.width  + 7) / 8;
    u32 groupsY = (perFrameAO->extent.height + 7) / 8;
    vulkanDispatch(cmd, &rayPipe, groupsX, groupsY, 1);
    vulkanEndProfile(cmd, &rayPipe.profile, 0);

    /* ── Temporal pass: G-TAO accumulation into the ping-pong ── */
    vulkanBeginProfile(cmd, &temporalPipe.profile, 0);
    vulkanTransition(cmd, perFrameAO, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, prev, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, out, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

    vulkanBindPipe(cmd, &temporalPipe);
    AoTemporalPushConstants tpc = {
        .aoIndex         = (u32)perFrameAO->sampledPoolIndex,
        .prevIndex       = (u32)prev->sampledPoolIndex,
        .outIndex        = (u32)out->storagePoolIndex,
        .velocityIndex   = (u32)velocity->sampledPoolIndex,
        .depthIndex      = (u32)depth->sampledPoolIndex,
        .width           = perFrameAO->extent.width,
        .height          = perFrameAO->extent.height,
        .historyWeight   = aoEnvHistoryWeight(),
        .depthThreshold  = 0.05f,
    };
    vulkanPush(cmd, &temporalPipe, sizeof(tpc), &tpc);
    vulkanDispatch(cmd, &temporalPipe, groupsX, groupsY, 1);
    vulkanEndProfile(cmd, &temporalPipe.profile, 0);

    vulkanTransition(cmd, out, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    aoCurrentOutput = out;
    aoFrame++;

    elapsedGPU = rayPipe.profile.elapsed + temporalPipe.profile.elapsed;
    elapsedCPU = utils::nanos() - elapsedCPU;
}

void VulkanAOPass::postUpdate() {
    vulkanAOPass.cpuElapsed = elapsedCPU;
    vulkanAOPass.gpuElapsed = elapsedGPU;
}

void VulkanAOPass::removed() {
    destroyAccumulators();
    if (rayPipe.pipe) {
        vulkanDestroyPipe(&rayPipe);
        rayPipe = VulkanPipe{};
    }
    if (temporalPipe.pipe) {
        vulkanDestroyPipe(&temporalPipe);
        temporalPipe = VulkanPipe{};
    }
}

void vulkanAOPassSetDisabled(char disabled) {
    aoDisabled = disabled;
    utils::info("AO: %s", aoDisabled ? "disabled" : "enabled");
}

char vulkanAOPassIsDisabled(void) {
    return aoDisabled;
}

VulkanImage* vulkanAOPassGetOutput(void) {
    /* The accumulator written by the most recently completed frame.  While
     * disabled it holds clean (1.0, 0.0) — safe to dump, ignored by the
     * composite (which uses the absent-sentinel index). */
    if (!aoA.img || !aoB.img) {
        return NULL;
    }
    return aoCurrentOutput ? aoCurrentOutput : &aoA;
}

/* Debug override for the temporal history weight (no recompile needed). */
static float aoEnvHistoryWeight(void) {
    const char* env = getenv("ENGINE_AO_WEIGHT");
    if (env && *env) {
        return (float)atof(env);
    }
    return 0.9f;
}
}  // namespace engine