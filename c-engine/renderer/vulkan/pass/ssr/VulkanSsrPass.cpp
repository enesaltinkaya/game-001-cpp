#include "VulkanSsrPass.h"
#include "VulkanSsrPass.h"
#include "ecs/system/System.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pass/hiz/VulkanHiZPass.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"

namespace engine {

static VulkanPipe pipeline;
static double     elapsedCPU;
static double     elapsedGPU;
static char       ssrDisabled;

static void clearReflectionColor(VulkanCommand *cmd, VulkanImage *reflColor) {
    vulkanTransition(cmd, reflColor, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);

    VkClearColorValue clearColor = {
        .float32 = {0.0f, 0.0f, 0.0f, 0.0f},
    };
    VkImageSubresourceRange range = {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,
    };
    vkCmdClearColorImage(cmd->cmd,
                         reflColor->img,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clearColor,
                         1,
                         &range);

    vulkanTransition(cmd, reflColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
}

VulkanSsrPass vulkanSsrPass;

VulkanSsrPass::VulkanSsrPass() : System("ssr") {}

typedef struct SsrPushConstants {
    u32 sceneColorIndex;
    u32 depthIndex;
    u32 hizIndex;
    u32 normalsIndex;
    u32 materialIndex;
    u32 outputIndex;
    u32 outputWidth;
    u32 outputHeight;
    u32 hizMipCount;
} SsrPushConstants;

void VulkanSsrPass::added() {
    const char *env = getenv("ENGINE_SSR_DISABLED");
    if (env && *env && atoi(env)) ssrDisabled = 1;

    pipeline = vulkanCreatePipe(
        .name = "ssr",
        .comp = "shaders/pass/ssr/spv/ssr.comp.spv");
}

void VulkanSsrPass::preUpdate() {
    vulkanResetProfile(vulkan.currentCmd, &pipeline.profile, 0);
}

void VulkanSsrPass::update() {
    elapsedCPU = utils::nanos();

    if (vulkan.skipFrame) {
        elapsedCPU = utils::nanos() - elapsedCPU;
        return;
    }

    VulkanCommand *cmd        = vulkan.currentCmd;
    VulkanImage   *sceneColor = vulkanFrameResourcesGetSceneColor();
    VulkanImage   *depth      = vulkanFrameResourcesGetDepth();
    VulkanImage   *normals    = vulkanFrameResourcesGetNormals();
    VulkanImage   *material   = vulkanFrameResourcesGetMaterial();
    VulkanImage   *reflColor  = vulkanFrameResourcesGetReflectionColor();

    if (!reflColor) {
        elapsedCPU = utils::nanos() - elapsedCPU;
        return;
    }

    if (ssrDisabled || !sceneColor || !depth || !normals || !material) {
        clearReflectionColor(cmd, reflColor);
        elapsedCPU = utils::nanos() - elapsedCPU;
        return;
    }

    /* Ensure inputs are readable */
    vulkanTransition(cmd, sceneColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, normals, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, material, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    /* ReflectionColor is the SSR output (RGBA16F) */
    vulkanTransition(cmd, reflColor, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

    vulkanBeginProfile(cmd, &pipeline.profile, 0);
    vulkanBindPipe(cmd, &pipeline);

    SsrPushConstants pc = {
        .sceneColorIndex = (u32)sceneColor->sampledPoolIndex,
        .depthIndex      = (u32)depth->sampledPoolIndex,
        .hizIndex        = vulkanHiZGetCurrentImage()
                               ? (u32)vulkanHiZGetCurrentImage()->sampledPoolIndex
                               : (u32)depth->sampledPoolIndex,
        .normalsIndex    = (u32)normals->sampledPoolIndex,
        .materialIndex   = (u32)material->sampledPoolIndex,
        .outputIndex     = (u32)reflColor->storagePoolIndex,
        .outputWidth     = reflColor->extent.width,
        .outputHeight    = reflColor->extent.height,
        .hizMipCount     = vulkanHiZGetCurrentImage()
                               ? (u32)vulkanHiZGetMipCount()
                               : 1,
    };
    vulkanPush(cmd, &pipeline, sizeof(pc), &pc);

    u32 groupsX = (reflColor->extent.width  + 7) / 8;
    u32 groupsY = (reflColor->extent.height + 7) / 8;
    vulkanDispatch(cmd, &pipeline, groupsX, groupsY, 1);
    vulkanEndProfile(cmd, &pipeline.profile, 0);

    vulkanTransition(cmd, reflColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    elapsedGPU = pipeline.profile.elapsed;
    elapsedCPU = utils::nanos() - elapsedCPU;
}

void VulkanSsrPass::postUpdate() {
    vulkanSsrPass.cpuElapsed = elapsedCPU;
    vulkanSsrPass.gpuElapsed = elapsedGPU;
}

void VulkanSsrPass::removed() {
    vulkanDestroyPipe(&pipeline);
}

void vulkanSsrPassSetDisabled(char disabled) {
    ssrDisabled = disabled;
    utils::info("SSR: %s", ssrDisabled ? "disabled" : "enabled");
}

char vulkanSsrPassIsDisabled(void) {
    return ssrDisabled;
}
}  // namespace engine
