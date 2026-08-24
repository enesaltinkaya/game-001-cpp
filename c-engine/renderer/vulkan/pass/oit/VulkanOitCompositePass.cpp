#include "VulkanOitCompositePass.h"
#include "VulkanOitCompositePass.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"

namespace engine {

static VulkanPipe pipeline;
static double     elapsedCPU;
static double     elapsedGPU;

VulkanOitCompositePass vulkanOitCompositePass;

VulkanOitCompositePass::VulkanOitCompositePass() : System("oit_composite") {}

typedef struct OitCompositePushConstants {
    u32 oitAccumIndex;
    u32 oitRevealIndex;
    u32 sceneColorIndex;
    u32 width;
    u32 height;
} OitCompositePushConstants;

void VulkanOitCompositePass::added() {
    pipeline = vulkanCreatePipe(
        .name = "oit_composite",
        .comp = "shaders/pass/oit/spv/oit_composite.comp.spv");
}

void VulkanOitCompositePass::preUpdate() {
    vulkanResetProfile(vulkan.currentCmd, &pipeline.profile, 0);
}

void VulkanOitCompositePass::update() {
    if (vulkan.skipFrame) return;

    VulkanCommand* cmd       = vulkan.currentCmd;
    VulkanImage*   oitAccum  = vulkanFrameResourcesGetOitAccum();
    VulkanImage*   oitReveal = vulkanFrameResourcesGetOitReveal();
    VulkanImage*   sceneColor = vulkanFrameResourcesGetSceneColor();

    if (!oitAccum || !oitReveal || !sceneColor) return;

    // Transition OIT textures to sampled, sceneColor to general for read-write
    vulkanTransition(cmd, oitAccum,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, oitReveal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, sceneColor, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

    vulkanBeginProfile(cmd, &pipeline.profile, 0);
    vulkanBindPipe(cmd, &pipeline);

    OitCompositePushConstants pc = {
        .oitAccumIndex   = (u32)oitAccum->sampledPoolIndex,
        .oitRevealIndex  = (u32)oitReveal->sampledPoolIndex,
        .sceneColorIndex = (u32)sceneColor->storagePoolIndex,
        .width           = sceneColor->extent.width,
        .height          = sceneColor->extent.height,
    };
    vulkanPush(cmd, &pipeline, sizeof(pc), &pc);

    u32 groupsX = (sceneColor->extent.width  + 7) / 8;
    u32 groupsY = (sceneColor->extent.height + 7) / 8;
    vulkanDispatch(cmd, &pipeline, groupsX, groupsY, 1);
    vulkanEndProfile(cmd, &pipeline.profile, 0);

    // Transition sceneColor back for subsequent passes (FSR, final, etc.)
    vulkanTransition(cmd, sceneColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    elapsedGPU = pipeline.profile.elapsed;
}

void VulkanOitCompositePass::postUpdate() {
    vulkanOitCompositePass.cpuElapsed = elapsedCPU;
    vulkanOitCompositePass.gpuElapsed = elapsedGPU;
    elapsedCPU                        = utils::nanos();
    elapsedCPU                        = utils::nanos() - elapsedCPU;
}

void VulkanOitCompositePass::removed() {
    vulkanDestroyPipe(&pipeline);
}
}  // namespace engine
