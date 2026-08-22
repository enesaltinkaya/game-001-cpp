#include "VulkanCompositePass.h"
#include "VulkanCompositePass.h"
#include "renderer/vulkan/pass/ao/VulkanAOPass.h"
#include "renderer/vulkan/pass/azgaar_weather/VulkanAzgaarWeatherPass.h"
#include "renderer/vulkan/pass/volumetric/VulkanVolumetricPass.h"
#include "ecs/system/System.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"

namespace engine {

static VulkanPipe pipeline;
static double     elapsedCPU;
static double     elapsedGPU;

VulkanCompositePass vulkanCompositePass;

VulkanCompositePass::VulkanCompositePass() : System("composite") {}

typedef struct CompositePushConstants {
    u32 sceneColorIndex;
    u32 ssrColorIndex;
    u32 depthIndex;
    u32 normalsIndex;
    u32 materialIndex;
    u32 outputImageIndex;
    u32 volumetricColorIndex;
    u32 weatherMaskIndex;
    u32 aoIndex;
    u32 width;
    u32 height;
} CompositePushConstants;

void VulkanCompositePass::added() {
    pipeline = vulkanCreatePipe(
        .name = "composite",
        .comp = "shaders/pass/composite/spv/composite.comp.spv");

}

void VulkanCompositePass::preUpdate() {
    vulkanResetProfile(vulkan.currentCmd, &pipeline.profile, 0);
}

void VulkanCompositePass::update() {
    elapsedCPU = utils::nanos();

    if (vulkan.skipFrame) {
        elapsedCPU = utils::nanos() - elapsedCPU;
        return;
    }

    VulkanCommand *cmd        = vulkan.currentCmd;
    VulkanImage   *sceneColor = vulkanFrameResourcesGetSceneColor();
    VulkanImage   *depth      = vulkanFrameResourcesGetDepth();
    VulkanImage   *reflColor  = vulkanFrameResourcesGetReflectionColor();
    VulkanImage   *normals    = vulkanFrameResourcesGetNormals();
    VulkanImage   *material   = vulkanFrameResourcesGetMaterial();
    VulkanImage   *composite  = vulkanFrameResourcesGetCompositeColor();
    VulkanImage   *volumetric  = vulkanVolumetricPassGetOutput();
    /* Weather particle coverage mask — the weather pass runs earlier in the
     * frame, so this frame's mask is ready.  Used to keep the screen-space
     * fog from erasing particles that float in front of fogged geometry. */
    VulkanImage   *weatherMask = vulkanAzgaarWeatherPassGetMask();
    if (!sceneColor || !depth || !reflColor || !normals || !material || !composite) {
        elapsedCPU = utils::nanos() - elapsedCPU;
        return;
    }

    vulkanTransition(cmd, sceneColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, normals, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, material, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    if (volumetric) {
        vulkanTransition(cmd, volumetric, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    }
    if (weatherMask) {
        vulkanTransition(cmd, weatherMask, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    }
    vulkanTransition(cmd, composite, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

    vulkanBeginProfile(cmd, &pipeline.profile, 0);
    vulkanBindPipe(cmd, &pipeline);

    CompositePushConstants pc = {
        .sceneColorIndex      = (u32)sceneColor->sampledPoolIndex,
        .ssrColorIndex        = (u32)reflColor->sampledPoolIndex,
        .depthIndex           = (u32)depth->sampledPoolIndex,
        .normalsIndex         = (u32)normals->sampledPoolIndex,
        .materialIndex        = (u32)material->sampledPoolIndex,
        .outputImageIndex     = (u32)composite->storagePoolIndex,
        .volumetricColorIndex = volumetric ? (u32)volumetric->sampledPoolIndex : 0u,
        .weatherMaskIndex     = weatherMask ? (u32)weatherMask->sampledPoolIndex : 0xFFFFFFFFu,
        /* CACAO AO output (absent-sentinel pattern, like the weather
         * mask): while AO is disabled the multiply is skipped entirely so
         * the frame stays pixel-identical to pre-AO. */
        .aoIndex              = vulkanAOPassIsDisabled()
                                   ? 0xFFFFFFFFu
                                   : (vulkanAOPassGetOutput()
                                         ? (u32)vulkanAOPassGetOutput()->sampledPoolIndex
                                         : 0xFFFFFFFFu),
        .width                = composite->extent.width,
        .height               = composite->extent.height,
    };
    vulkanPush(cmd, &pipeline, sizeof(pc), &pc);

    u32 groupsX = (composite->extent.width  + 7) / 8;
    u32 groupsY = (composite->extent.height + 7) / 8;
    vulkanDispatch(cmd, &pipeline, groupsX, groupsY, 1);
    vulkanEndProfile(cmd, &pipeline.profile, 0);

    vulkanTransition(cmd, composite, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    elapsedGPU = pipeline.profile.elapsed;
    elapsedCPU = utils::nanos() - elapsedCPU;
}

void VulkanCompositePass::postUpdate() {
    vulkanCompositePass.cpuElapsed = elapsedCPU;
    vulkanCompositePass.gpuElapsed = elapsedGPU;
}

void VulkanCompositePass::removed() {
    vulkanDestroyPipe(&pipeline);
}
}  // namespace engine
