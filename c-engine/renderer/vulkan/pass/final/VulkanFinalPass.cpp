#include "VulkanFinalPass.h"
#include "ecs/system/System.h"
#include "renderer/Renderer.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/swapchain/VulkanSwapchain.h"
#include "renderer/vulkan/pass/fsr/VulkanFsrPass.h"
#include "renderer/vulkan/pass/taa/VulkanTaaPass.h"
#include "renderer/vulkan/pass/bloom/VulkanBloomPass.h"

static void added(void);
static void preUpdate(void);
static void update(void);
static void postUpdate(void);
static void removed(void);

static double elapsedGPU;

System vulkanFinalPass = {
    .name       = "final",
    .added      = added,
    .preUpdate  = preUpdate,
    .update     = update,
    .postUpdate = postUpdate,
    .removed    = removed,
};

static VulkanPipe pipeline;

typedef struct FinalPushConstants {
    u32 colorTextureIndex;
    u32 bloomTextureIndex;
    float bloomStrength;
    float casStrength;
    float contrast;
    u32 pad[3];
} FinalPushConstants;

static void added(void) {
    pipeline = vulkanCreatePipe(.name               = "final",
                                .vs                 = "shaders/pass/final/spv/final.vert.spv",
                                .fs                 = "shaders/pass/final/spv/final.frag.spv",
                                .colorFormat1       = VK_FORMAT_B8G8R8A8_SRGB,
                                .clearColor1Enabled = 1,
                                .clearColor1        = {0.0f, 0.0f, 0.0f, 1.0f});
}

static void preUpdate(void) {
    vulkanResetProfile(vulkan.currentCmd, &pipeline.profile, 0);
}

static void update(void) {
    if (vulkan.skipFrame) return;

    VulkanCommand* cmd      = vulkan.currentCmd;
    VulkanImage* taaImage       = vulkanTaaPassGetOutput();
    VulkanImage* fsrImage       = vulkanFsrPassGetOutput();
    VulkanImage* compositeImage = vulkanFrameResourcesGetCompositeColor();
    VulkanImage* colorImage     = taaImage       ? taaImage
                           : fsrImage       ? fsrImage
                           : compositeImage ? compositeImage
                                           : vulkanFrameResourcesGetSceneColor();
    VulkanImage* swapImage  = vulkanSwapchain.currentSwapchainImage;

    if (!colorImage || !swapImage) return;

    vulkanBeginProfile(cmd, &pipeline.profile, 0);
    vulkanTransition(cmd, colorImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    vulkanBeginRender(.cmd = cmd, .pipe = &pipeline, .color1 = swapImage);

    vulkanViewport(cmd,
                   0,
                   swapImage->extent.height,
                   swapImage->extent.width,
                   -((i32)swapImage->extent.height));
    vulkanScissor(cmd, 0, 0, swapImage->extent.width, swapImage->extent.height);

    vulkanBindPipe(cmd, &pipeline);

    FinalPushConstants pc = {
        .colorTextureIndex = (u32)colorImage->sampledPoolIndex,
        .bloomTextureIndex = (u32)vulkanBloomPassGetBloomSampledIndex(),
        .bloomStrength     = vulkanBloomPassGetStrength(),
        .casStrength       = rendererGetCasStrength(),
        .contrast          = CONTRAST,
    };
    vulkanPush(cmd, &pipeline, sizeof(pc), &pc);
    vulkanDraw(cmd, 3, 1);

    vulkanEndRender(cmd);
    vulkanEndProfile(cmd, &pipeline.profile, 0);
    elapsedGPU = pipeline.profile.elapsed;

}

static void postUpdate(void) {
    vulkanFinalPass.gpuElapsed = elapsedGPU;
}

static void removed(void) {
    vulkanDestroyPipe(&pipeline);
}
