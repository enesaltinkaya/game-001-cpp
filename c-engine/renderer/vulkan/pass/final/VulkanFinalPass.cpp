#include "VulkanFinalPass.h"
#include "VulkanFinalPass.h"
#include "ecs/system/System.h"
#include "renderer/Renderer.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/pass/fsr/VulkanFsrPass.h"
#include "renderer/vulkan/pass/lpm/VulkanLpmPass.h"
#include "renderer/vulkan/pass/taa/VulkanTaaPass.h"
#include "renderer/vulkan/pass/bloom/VulkanBloomPass.h"
#include "renderer/vulkan/pass/dof/VulkanDofPass.h"

namespace engine {

static double elapsedGPU;

VulkanFinalPass vulkanFinalPass;

VulkanFinalPass::VulkanFinalPass() : System("final") {}

static VulkanPipe pipeline;

struct FinalPushConstants {
    u32 colorTextureIndex = 0;
    u32 bloomTextureIndex = 0;
    float bloomStrength   = 0.0f;
    float rcasStrength    = 0.0f;
    u32 pad[4]           = {};
};

void VulkanFinalPass::added() {
    /* R16F attachment: the pass stores the LINEAR HDR composite; the LPM
     * pass tone/gamut-maps it (replacing the custom tonemapping curves)
     * and blits its 8-bit result into the lens input (when the lens is
     * active) or the swapchain. */
    pipeline = vulkanCreatePipe(.name               = "final",
                                .vs                 = "shaders/pass/final/spv/final.vert.spv",
                                .fs                 = "shaders/pass/final/spv/final.frag.spv",
                                .colorFormat1       = VK_FORMAT_R16G16B16A16_SFLOAT,
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
    VulkanImage* dofImage       = vulkanDofPassGetOutput();
    VulkanImage* compositeImage = vulkanFrameResourcesGetCompositeColor();
    /* TAA keeps priority (a stale FSR output from a previous upscaler
     * session must not win while TAA is on). DOF output replaces the TAA
     * color in the non-upscaler path; the upscaler path already consumed
     * the DOF output, so its output takes priority there. */
    VulkanImage* colorImage     = taaImage ? (dofImage ? dofImage : taaImage)
                           : fsrImage       ? fsrImage
                           : dofImage       ? dofImage
                           : compositeImage ? compositeImage
                                           : vulkanFrameResourcesGetSceneColor();
    /* LPM input (R16F): the pass stores the linear HDR composite here;
     * the LPM pass tone-maps it and blits the 8-bit result into the lens
     * input (lens active) or the swapchain. */
    VulkanImage* lpmImage = vulkanLpmPassGetInput();

    if (!colorImage || !lpmImage) return;

    vulkanBeginProfile(cmd, &pipeline.profile, 0);
    vulkanTransition(cmd, colorImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    vulkanBeginRender(.cmd = cmd, .pipe = &pipeline, .color1 = lpmImage);

    vulkanViewport(cmd,
                   0,
                   lpmImage->extent.height,
                   lpmImage->extent.width,
                   -((i32)lpmImage->extent.height));
    vulkanScissor(cmd, 0, 0, lpmImage->extent.width, lpmImage->extent.height);

    vulkanBindPipe(cmd, &pipeline);

    FinalPushConstants pc = {
        .colorTextureIndex = (u32)colorImage->sampledPoolIndex,
        .bloomTextureIndex = (u32)vulkanBloomPassGetBloomSampledIndex(),
        .bloomStrength     = vulkanBloomPassGetStrength(),
        /* RCAS runs here only when the upscaler is off (TAA / raw path).
         * When the FSR3 upscaler is active it applies RCAS internally on
         * the upscaled image — pushing a nonzero strength here too would
         * sharpen twice (stacked kernels cause ringing). */
        .rcasStrength      = rendererIsUpscalerEnabled() ? 0.0f : rendererGetCasStrength(),
    };
    vulkanPush(cmd, &pipeline, sizeof(pc), &pc);
    vulkanDraw(cmd, 3, 1);

    vulkanEndRender(cmd);
    vulkanEndProfile(cmd, &pipeline.profile, 0);

    /* The LPM pass dispatches on frames where the Final pass rendered. */
    vulkanLpmPassMarkRendered();

    elapsedGPU = pipeline.profile.elapsed;
}

void VulkanFinalPass::postUpdate() {
    vulkanFinalPass.gpuElapsed = elapsedGPU;
}

void VulkanFinalPass::removed() {
    vulkanDestroyPipe(&pipeline);
}
}  // namespace engine
