#include "VulkanDiffuseGIPass.h"
#include "ecs/Ecs.h"
#include "events/Events.h"
#include "futuretask/FutureTask.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include <stdlib.h>

namespace engine {

typedef struct DiffuseGiPushConstants {
    u32 srcIndex;
    u32 depthIndex;
    u32 normalsIndex;
    u32 outIndex;
    u32 width;
    u32 height;
    u32 dir;
    float sigma;
    float depthEdge;
    float normalEdgeMin;
    float normalEdgeMax;
} DiffuseGiPushConstants;

typedef struct DiffuseGiTemporalPushConstants {
    u32 spatialIndex;
    u32 historyIndex;
    u32 velocityIndex;
    u32 depthIndex;
    u32 outIndex;
    u32 width;
    u32 height;
    u32 fullWidth;
    u32 fullHeight;
    float temporalWeight;
    float depthThreshold;
    float colorThreshold;
} DiffuseGiTemporalPushConstants;

static VulkanPipe pipeline;
static char       pipelineReady;
static VulkanPipe temporalPipeline;
static char       temporalPipelineReady;
static char       giDisabled;
static int        giIterations = 2;   /* H+V separable steps per iteration */
static float      giStrength   = 0.4f;/* blend toward diffused (0..1) */
static float      giRadius     = 20.f;/* gaussian sigma, render-res px  */
static float      giDepthEdge  = 0.05f;
static float      giNdMin      = -1.f;/* <= -1: normal edge gate off   */
static float      giNdMax      = 0.9f;
/* The diffusion is low-frequency data: the ping-pong buffers run at a
 * fraction of the render resolution and the composite upsamples linearly.
 * 0.25 = the usual case (16x fewer pixels than full-res). */
static float      giResScale   = 0.25f;

static VulkanImage giA;
static VulkanImage giB;
/* Temporal history (ping-pong); also the pass output the composite
 * samples.  Alpha carries the inverse view depth (0 = no data). */
static VulkanImage giHA;
static VulkanImage giHB;
static u32         giWidth;
static u32         giHeight;
static u32         giFrame;
static VulkanImage* giOutput;
static float       giTemporalWeight;
static float       giDepthThreshold;
static float       giGhostThreshold;
static double      elapsedCPU;
static double      elapsedGPU;

VulkanDiffuseGIPass vulkanDiffuseGIPass;

static double      giToggleMsAfterLoad = -1.0;

static void giToggleOff(void) {
    utils::info("Diffusion GI: ENGINE_GI_TOGGLE_AT_MS fired, disabling");
    vulkanDiffuseGIPassSetDisabled(1);
}

static void giOnGameLoadedForToggle(void* _) {
    (void)_;
    if (giToggleMsAfterLoad < 0.0) {
        return;
    }
    utils::futureTaskAddNoParam(giToggleMsAfterLoad, giToggleOff);
    giToggleMsAfterLoad = -1.0;
}

VulkanDiffuseGIPass::VulkanDiffuseGIPass() : System("diffuse_gi") {}

static void giOnSwapchainCreated(void* _);

static float giEnvFloat(const char* name, float def) {
    const char* env = getenv(name);
    return (env && *env) ? (float)atof(env) : def;
}

void VulkanDiffuseGIPass::added() {
    const char* env = getenv("ENGINE_GI_DISABLED");
    if (env && *env && atoi(env)) giDisabled = 1;
    int iterEnv = 2;
    env = getenv("ENGINE_GI_ITER");
    if (env && *env) iterEnv = atoi(env);
    giIterations = (iterEnv < 1) ? 1 : ((iterEnv > 8) ? 8 : iterEnv);
    giStrength   = giEnvFloat("ENGINE_GI_STRENGTH", 0.4f);
    if (giStrength < 0.0f) giStrength = 0.0f;
    if (giStrength > 1.0f) giStrength = 1.0f;
    giRadius     = giEnvFloat("ENGINE_GI_RADIUS", 20.f);
    if (giRadius < 2.f) giRadius = 2.f;
    if (giRadius > 48.f) giRadius = 48.f;
    giDepthEdge  = giEnvFloat("ENGINE_GI_DEPTH_EDGE", 0.05f);
    giNdMin      = giEnvFloat("ENGINE_GI_NDOT_MIN", -1.f);
    giNdMax      = giEnvFloat("ENGINE_GI_NDOT_MAX", 0.9f);
    if (giNdMax <= giNdMin) giNdMax = giNdMin + 0.2f;
    giResScale   = giEnvFloat("ENGINE_GI_RES", 0.25f);
    if (giResScale < 0.25f) giResScale = 0.25f;
    if (giResScale > 1.0f) giResScale = 1.0f;
    giTemporalWeight = giEnvFloat("ENGINE_GI_TEMPORAL", 0.75f);
    if (giTemporalWeight < 0.0f) giTemporalWeight = 0.0f;
    if (giTemporalWeight > 1.0f) giTemporalWeight = 1.0f;
    giDepthThreshold = giEnvFloat("ENGINE_GI_DEPTH_THRESH", 0.05f);
    giGhostThreshold = giEnvFloat("ENGINE_GI_GHOST", 0.15f);
    if (giGhostThreshold < 0.01f) giGhostThreshold = 0.01f;
    if (giGhostThreshold > 1.0f) giGhostThreshold = 1.0f;

    /* ENGINE_GI_TOGGLE_AT_MS=<ms>: at startup, disable the pass again after
     * <ms> relative to "gameLoaded" so one run captures aligned A/B phases
     * (shimmer baseline with GI on vs. off along the same camera path). */
    const char* toggleEnv = getenv("ENGINE_GI_TOGGLE_AT_MS");
    if (toggleEnv && *toggleEnv) {
        giToggleMsAfterLoad = atof(toggleEnv);
    }

    /* Resolution changes (render scale / FSR mode) recreate the swapchain
     * after a wait-idle and emit "swapchainCreated" — destroying the
     * ping-pong buffers there is safe; creating them happens lazily in
     * update(). */
    utils::signalSubscribe("swapchainCreated", giOnSwapchainCreated);
    if (giToggleMsAfterLoad >= 0.0) {
        utils::signalSubscribe("gameLoaded", giOnGameLoadedForToggle);
    }

    pipeline      = vulkanCreatePipe(.name = "diffuse_gi",
                                     .comp = "shaders/pass/diffuse_gi/spv/diffuse_gi.comp.spv");
    pipelineReady = 1;
    temporalPipeline      = vulkanCreatePipe(.name = "diffuse_gi_temporal",
                                             .comp = "shaders/pass/diffuse_gi/spv/diffuse_gi_temporal.comp.spv");
    temporalPipelineReady = 1;
}

void VulkanDiffuseGIPass::preUpdate() {
    if (pipelineReady) {
        vulkanResetProfile(vulkan.currentCmd, &pipeline.profile, 0);
    }
}

static void giDestroyAccumulators(void) {
    if (giA.img) {
        vulkanDestroyImage(&giA, NULL);
        giA = VulkanImage{};
    }
    if (giB.img) {
        vulkanDestroyImage(&giB, NULL);
        giB = VulkanImage{};
    }
    if (giHA.img) {
        vulkanDestroyImage(&giHA, NULL);
        giHA = VulkanImage{};
    }
    if (giHB.img) {
        vulkanDestroyImage(&giHB, NULL);
        giHB = VulkanImage{};
    }
    giWidth  = 0;
    giHeight = 0;
    giFrame  = 0;
    giOutput = NULL;
}

static void giOnSwapchainCreated(void* _) {
    (void)_;
    giDestroyAccumulators();
}

/* Zero the temporal history (alpha 0 = "no data" -> the temporal filter
 * degrades to the pure spatial result until it re-converges).  Same
 * transient-clear pattern as the TAA accumulators: transition to GENERAL
 * first, so the clear's restore-back barrier targets a defined layout. */
static void giClearHistories(void) {
    if (!giHA.img || !giHB.img) {
        return;
    }
    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanTransition(cmd, &giHA, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    vulkanTransition(cmd, &giHB, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    VkClearColorValue black;
    black.float32[0] = 0.0f;
    black.float32[1] = 0.0f;
    black.float32[2] = 0.0f;
    black.float32[3] = 0.0f;
    vulkanClearColorImage(cmd, &giHA, black);
    vulkanClearColorImage(cmd, &giHB, black);
    vulkanTransientEnd(cmd, 1);
}

/* Create the ping-pong buffers if the current size is not backed yet.
 * Destruction of the old buffers happens on the "swapchainCreated" signal
 * (device is idle there); both buffers are fully written every frame
 * (iteration 1 always reads the scene colour, never a GI buffer), so no
 * initial clear is needed. */
static char giEnsureAccumulators(u32 width, u32 height) {
    if (giA.img && giB.img && giHA.img && giHB.img && giWidth == width &&
        giHeight == height) {
        return 1;
    }

    giA = vulkanCreateImage(.name   = "DiffuseGiA",
                           .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                           .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                           .width  = (int)width,
                           .height = (int)height);
    giB = vulkanCreateImage(.name   = "DiffuseGiB",
                           .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                           .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                           .width  = (int)width,
                           .height = (int)height);
    giHA = vulkanCreateImage(.name   = "DiffuseGiHistoryA",
                           .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                           /* TRANSFER_SRC: the debug frame-image dump
                            * (ENGINE_DEBUG_DUMP_IMAGES=giRaw) blits the
                            * history through a TRANSFER_SRC transition. */
                           .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           .width  = (int)width,
                           .height = (int)height);
    giHB = vulkanCreateImage(.name   = "DiffuseGiHistoryB",
                           .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                           .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           .width  = (int)width,
                           .height = (int)height);
    if (!giA.img || !giB.img || !giHA.img || !giHB.img) {
        giDestroyAccumulators();
        return 0;
    }
    giWidth      = width;
    giHeight     = height;
    giFrame      = 0;
    giClearHistories();
    return 1;
}

void VulkanDiffuseGIPass::update() {
    elapsedCPU = utils::nanos();

    if (vulkan.skipFrame) {
        elapsedCPU = utils::nanos() - elapsedCPU;
        return;
    }

    VulkanCommand* cmd        = vulkan.currentCmd;
    VulkanImage*   sceneColor = vulkanFrameResourcesGetSceneColor();
    VulkanImage*   depth      = vulkanFrameResourcesGetDepth();
    VulkanImage*   normals    = vulkanFrameResourcesGetNormals();

    if (giDisabled || !sceneColor || !depth || !normals) {
        /* Disabled: the composite skips the bounce term entirely
         * (absent-sentinel index), so no output needs producing. */
        giOutput = NULL;
        elapsedCPU = utils::nanos() - elapsedCPU;
        return;
    }

    u32 width  = sceneColor->extent.width;
    u32 height = sceneColor->extent.height;
    u32 giW    = (u32)(width  * giResScale);
    u32 giH    = (u32)(height * giResScale);
    if (giW < 1) giW = 1;
    if (giH < 1) giH = 1;
    if (!giEnsureAccumulators(giW, giH)) {
        giOutput = NULL;
        elapsedCPU = utils::nanos() - elapsedCPU;
        return;
    }

    vulkanTransition(cmd, sceneColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, normals, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    vulkanBeginProfile(cmd, &pipeline.profile, 0);
    vulkanBindPipe(cmd, &pipeline);

    u32 groupsX = (giW + 7) / 8;
    u32 groupsY = (giH + 7) / 8;
    /* sigma in GI-buffer px (the kernel is +-8, so clamp to +-8 px) */
    float sigma = giRadius / giResScale;
    if (sigma < 1.0f) sigma = 1.0f;
    if (sigma > 8.0f) sigma = 8.0f;

    VulkanImage* src = sceneColor;
    for (int i = 0; i < giIterations; i++) {
        for (int d = 0; d < 2; d++) {
            /* d = 0: horizontal step, d = 1: vertical step; ping-pong */
            VulkanImage* out = d ? &giB : &giA;

            vulkanTransition(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
            vulkanTransition(cmd, out, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

            DiffuseGiPushConstants pc = {
                .srcIndex        = (u32)src->sampledPoolIndex,
                .depthIndex      = (u32)depth->sampledPoolIndex,
                .normalsIndex    = (u32)normals->sampledPoolIndex,
                .outIndex        = (u32)out->storagePoolIndex,
                .width           = giW,
                .height          = giH,
                .dir             = (u32)d,
                .sigma           = sigma,
                .depthEdge        = giDepthEdge,
                .normalEdgeMin   = giNdMin,
                .normalEdgeMax   = giNdMax,
            };
            vulkanPush(cmd, &pipeline, sizeof(pc), &pc);
            vulkanDispatch(cmd, &pipeline, groupsX, groupsY, 1);

            src = out;
        }
    }

    vulkanEndProfile(cmd, &pipeline.profile, 0);

    /* ── Temporal filter ──
     * The spatial field is a fresh screen-aligned blur every frame; as the
     * camera moves it wiggles frame-to-frame and TAA's rejections fire
     * unevenly on high-frequency surfaces (grass shimmer).  Reproject
     * last frame's GI field with the same motion vectors TAA uses and
     * blend it in, so the field handed to the composite is
     * reprojection-consistent (standard screen-space-GI temporal filter). */
    VulkanImage* temporalOut;
    if (giTemporalWeight > 0.0f && temporalPipelineReady) {
        VulkanImage* velocity = vulkanFrameResourcesGetVelocity();
        VulkanImage* hPrev = (giFrame & 1) ? &giHB : &giHA;
        temporalOut = (giFrame & 1) ? &giHA : &giHB;

        if (velocity) {
            vulkanTransition(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
            vulkanTransition(cmd, velocity, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
            vulkanTransition(cmd, hPrev, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
            vulkanTransition(cmd, temporalOut, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

            vulkanBindPipe(cmd, &temporalPipeline);
            DiffuseGiTemporalPushConstants pcT = {
                .spatialIndex    = (u32)src->sampledPoolIndex,
                .historyIndex    = (u32)hPrev->sampledPoolIndex,
                .velocityIndex   = (u32)velocity->sampledPoolIndex,
                .depthIndex      = (u32)depth->sampledPoolIndex,
                .outIndex        = (u32)temporalOut->storagePoolIndex,
                .width           = giW,
                .height          = giH,
                .fullWidth       = width,
                .fullHeight      = height,
                .temporalWeight  = giTemporalWeight,
                .depthThreshold  = giDepthThreshold,
                .colorThreshold  = giGhostThreshold,
            };
            vulkanPush(cmd, &temporalPipeline, sizeof(pcT), &pcT);
            vulkanDispatch(cmd, &temporalPipeline, groupsX, groupsY, 1);
            vulkanTransition(cmd, temporalOut, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
            giFrame++;
        } else {
            temporalOut = NULL;
        }
    } else {
        temporalOut = NULL;
    }

    if (temporalOut) {
        /* The temporally filtered field is what the composite samples. */
        giOutput = temporalOut;
    } else {
        /* No temporal step (weight 0, no velocity, or first frame before
         * a history exists): fall back to the raw spatial result. */
        vulkanTransition(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        giOutput = src;
    }

    elapsedGPU = pipeline.profile.elapsed;
    elapsedCPU = utils::nanos() - elapsedCPU;
}

void VulkanDiffuseGIPass::postUpdate() {
    vulkanDiffuseGIPass.cpuElapsed = elapsedCPU;
    vulkanDiffuseGIPass.gpuElapsed = elapsedGPU;
}

void VulkanDiffuseGIPass::removed() {
    giDestroyAccumulators();
    if (pipelineReady) {
        vulkanDestroyPipe(&pipeline);
        pipeline      = VulkanPipe{};
        pipelineReady = 0;
    }
    if (temporalPipelineReady) {
        vulkanDestroyPipe(&temporalPipeline);
        temporalPipeline      = VulkanPipe{};
        temporalPipelineReady = 0;
    }
}

void vulkanDiffuseGIPassSetDisabled(char disabled) {
    if (disabled != giDisabled) {
        giDisabled = disabled;
        if (!disabled) {
            /* Re-enabled: the history holds a stale field (and the
             * spatial state is cold) — clear it so the temporal filter
             * starts fresh instead of ghosting the old frame. */
            giFrame = 0;
            giClearHistories();
        }
        utils::info("Diffusion GI: %s", giDisabled ? "disabled" : "enabled");
    }
}

char vulkanDiffuseGIPassIsDisabled(void) {
    return giDisabled;
}

float vulkanDiffuseGIPassGetStrength(void) {
    return giStrength;
}

VulkanImage* vulkanDiffuseGIPassGetOutput(void) {
    if (giDisabled) {
        return NULL;
    }
    return giOutput;
}
}  // namespace engine
