#include "VulkanDiffuseGIPass.h"
#include "ecs/Ecs.h"
#include "events/Events.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
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
    float radius;
    float depthEdge;
    float normalEdgeMin;
    float normalEdgeMax;
} DiffuseGiPushConstants;

static VulkanPipe pipeline;
static char       pipelineReady;
static char       giDisabled;
static int        giIterations = 3;
static float      giStrength   = 1.0f;
static float      giRadius     = 1.4f;
static float      giDepthEdge  = 0.05f;
static float      giNdMin      = 0.4f;
static float      giNdMax      = 0.9f;
/* The diffusion is low-frequency data: the ping-pong buffers run at a
 * fraction of the render resolution and the composite upsamples linearly.
 * 0.5 = the usual case (4x fewer pixels than full-res). */
static float      giResScale   = 0.5f;

static VulkanImage giA;
static VulkanImage giB;
static u32         giWidth;
static u32         giHeight;
static VulkanImage* giOutput;
static double      elapsedCPU;
static double      elapsedGPU;

VulkanDiffuseGIPass vulkanDiffuseGIPass;

VulkanDiffuseGIPass::VulkanDiffuseGIPass() : System("diffuse_gi") {}

static void giOnSwapchainCreated(void* _);

static float giEnvFloat(const char* name, float def) {
    const char* env = getenv(name);
    return (env && *env) ? (float)atof(env) : def;
}

void VulkanDiffuseGIPass::added() {
    const char* env = getenv("ENGINE_GI_DISABLED");
    if (env && *env && atoi(env)) giDisabled = 1;
    int iterEnv = 3;
    env = getenv("ENGINE_GI_ITER");
    if (env && *env) iterEnv = atoi(env);
    giIterations = (iterEnv < 1) ? 1 : ((iterEnv > 8) ? 8 : iterEnv);
    giStrength   = giEnvFloat("ENGINE_GI_STRENGTH", 1.0f);
    giRadius     = giEnvFloat("ENGINE_GI_RADIUS", 1.4f);
    giDepthEdge  = giEnvFloat("ENGINE_GI_DEPTH_EDGE", 0.05f);
    giNdMin      = giEnvFloat("ENGINE_GI_NDOT_MIN", 0.4f);
    giNdMax      = giEnvFloat("ENGINE_GI_NDOT_MAX", 0.9f);
    if (giNdMax <= giNdMin) giNdMax = giNdMin + 0.2f;
    giResScale   = giEnvFloat("ENGINE_GI_RES", 0.5f);
    if (giResScale < 0.25f) giResScale = 0.25f;
    if (giResScale > 1.0f) giResScale = 1.0f;

    /* Resolution changes (render scale / FSR mode) recreate the swapchain
     * after a wait-idle and emit "swapchainCreated" — destroying the
     * ping-pong buffers there is safe; creating them happens lazily in
     * update(). */
    utils::signalSubscribe("swapchainCreated", giOnSwapchainCreated);

    pipeline      = vulkanCreatePipe(.name = "diffuse_gi",
                                     .comp = "shaders/pass/diffuse_gi/spv/diffuse_gi.comp.spv");
    pipelineReady = 1;
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
    giWidth    = 0;
    giHeight   = 0;
    giOutput   = NULL;
}

static void giOnSwapchainCreated(void* _) {
    (void)_;
    giDestroyAccumulators();
}

/* Create the ping-pong buffers if the current size is not backed yet.
 * Destruction of the old buffers happens on the "swapchainCreated" signal
 * (device is idle there); both buffers are fully written every frame
 * (iteration 1 always reads the scene colour, never a GI buffer), so no
 * initial clear is needed. */
static char giEnsureAccumulators(u32 width, u32 height) {
    if (giA.img && giB.img && giWidth == width && giHeight == height) {
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
    if (!giA.img || !giB.img) {
        giDestroyAccumulators();
        return 0;
    }
    giWidth  = width;
    giHeight = height;
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

    VulkanImage* src = sceneColor;
    for (int i = 0; i < giIterations; i++) {
        VulkanImage* out = (i % 2) ? &giB : &giA;

        vulkanTransition(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, out, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

        DiffuseGiPushConstants pc = {
            .srcIndex        = (u32)src->sampledPoolIndex,
            .depthIndex      = (u32)depth->sampledPoolIndex,
            .normalsIndex    = (u32)normals->sampledPoolIndex,
            .outIndex        = (u32)out->storagePoolIndex,
            .width           = giW,
            .height          = giH,
            /* radius in full-res px, converted to GI-buffer px */
            .radius          = giRadius / giResScale,
            .depthEdge        = giDepthEdge,
            .normalEdgeMin   = giNdMin,
            .normalEdgeMax   = giNdMax,
        };
        vulkanPush(cmd, &pipeline, sizeof(pc), &pc);

        u32 groupsX = (giW + 7) / 8;
        u32 groupsY = (giH + 7) / 8;
        vulkanDispatch(cmd, &pipeline, groupsX, groupsY, 1);

        src = out;
    }

    vulkanEndProfile(cmd, &pipeline.profile, 0);

    /* Leave the output sampled for the composite pass. */
    vulkanTransition(cmd, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    giOutput = src;

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
}

void vulkanDiffuseGIPassSetDisabled(char disabled) {
    giDisabled = disabled;
    utils::info("Diffusion GI: %s", giDisabled ? "disabled" : "enabled");
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
