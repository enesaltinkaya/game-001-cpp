#include "VulkanFinalPass.h"
#include "VulkanFinalPass.h"
#include "ecs/system/System.h"
#include "renderer/Renderer.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/swapchain/VulkanSwapchain.h"
#include "renderer/vulkan/pass/fsr/VulkanFsrPass.h"
#include "renderer/vulkan/pass/lens/VulkanLensPass.h"
#include "renderer/vulkan/pass/taa/VulkanTaaPass.h"
#include "renderer/vulkan/pass/bloom/VulkanBloomPass.h"

namespace engine {

static double elapsedGPU;

VulkanFinalPass vulkanFinalPass;

VulkanFinalPass::VulkanFinalPass() : System("final") {}

static VulkanPipe pipeline;

struct FinalPushConstants {
    u32 colorTextureIndex = 0;
    u32 bloomTextureIndex = 0;
    float bloomStrength   = 0.0f;
    float contrast        = 0.0f;
    float rcasStrength    = 0.0f;
    u32 pad[3]           = {};
};

void VulkanFinalPass::added() {
    pipeline = vulkanCreatePipe(.name               = "final",
                                .vs                 = "shaders/pass/final/spv/final.vert.spv",
                                .fs                 = "shaders/pass/final/spv/final.frag.spv",
                                .colorFormat1       = VK_FORMAT_B8G8R8A8_SRGB,
                                .clearColor1Enabled = 1,
                                .clearColor1        = {0.0f, 0.0f, 0.0f, 1.0f});
}

void VulkanFinalPass::preUpdate() {
    vulkanResetProfile(vulkan.currentCmd, &pipeline.profile, 0);
}

void VulkanFinalPass::update() {
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

    /* Lens active: render into the lens input (also an SRGB attachment —
     * same pipeline); the lens pass then blits its output into the swapchain
     * before UI. Lens inactive: render straight into the swapchain. */
    char        lensActive = vulkanLensPassIsActive();
    VulkanPipe* pipe       = &pipeline;
    VulkanImage* target    = lensActive ? vulkanLensPassGetInput() : swapImage;

    vulkanBeginProfile(cmd, &pipe->profile, 0);
    vulkanTransition(cmd, colorImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    vulkanBeginRender(.cmd = cmd, .pipe = pipe, .color1 = target);

    vulkanViewport(cmd,
                   0,
                   swapImage->extent.height,
                   swapImage->extent.width,
                   -((i32)swapImage->extent.height));
    vulkanScissor(cmd, 0, 0, swapImage->extent.width, swapImage->extent.height);

    vulkanBindPipe(cmd, pipe);

    FinalPushConstants pc = {
        .colorTextureIndex = (u32)colorImage->sampledPoolIndex,
        .bloomTextureIndex = (u32)vulkanBloomPassGetBloomSampledIndex(),
        .bloomStrength     = vulkanBloomPassGetStrength(),
        .contrast          = CONTRAST,
        /* RCAS runs here only when the upscaler is off (TAA / raw path).
         * When the FSR3 upscaler is active it applies RCAS internally on
         * the upscaled image — pushing a nonzero strength here too would
         * sharpen twice (stacked kernels cause ringing). */
        .rcasStrength      = rendererIsUpscalerEnabled() ? 0.0f : rendererGetCasStrength(),
    };
    vulkanPush(cmd, pipe, sizeof(pc), &pc);
    vulkanDraw(cmd, 3, 1);

    vulkanEndRender(cmd);
    vulkanEndProfile(cmd, &pipe->profile, 0);
    if (lensActive) {
        vulkanLensPassMarkRendered();
    }
    elapsedGPU = pipe->profile.elapsed;

}

void VulkanFinalPass::postUpdate() {
    vulkanFinalPass.gpuElapsed = elapsedGPU;
}

void VulkanFinalPass::removed() {
    vulkanDestroyPipe(&pipeline);
}
}  // namespace engine
