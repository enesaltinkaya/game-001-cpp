#include "VulkanSkyboxPass.h"
#include "VulkanSkyboxPass.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "renderer/Renderer.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/swapchain/VulkanSwapchain.h"

namespace engine {

static double elapsedCPU;
static double elapsedGPU;

VulkanSkyboxPass vulkanSkyboxPass;

VulkanSkyboxPass::VulkanSkyboxPass() : System("skybox") {}

static VulkanPipe skyboxPipe;

static void swapchainCreated(void*) {
    if (skyboxPipe.pipe) {
        vulkanDestroyPipe(&skyboxPipe);
    }
    skyboxPipe = vulkanCreatePipe(
        .name               = "skybox",
        .vs                 = "shaders/pass/skybox/spv/skybox.vert.spv",
        .fs                 = "shaders/pass/skybox/spv/skybox.frag.spv",
        .colorFormat1       = VK_FORMAT_R16G16B16A16_SFLOAT,
        .clearColor1        = {0, 0, 0, 0},
        .clearColor1Enabled = 0,  // load — don't overwrite terrain/scene
        /* Camera-motion velocity for sky pixels (LOAD: keep the scene's
         * velocity).  Without this the sky's MV stays at the (0,0) clear
         * value and TAA ghosts the sky on camera rotation. */
        .colorFormat2       = VK_FORMAT_R16G16_SFLOAT,
        .clearColor2        = {0, 0, 0, 0},
        .clearColor2Enabled = 0,  // load — keep the scene's velocity
        .depthFormat        = VK_FORMAT_D32_SFLOAT,
        .clearDepth         = {0, 0},
        .clearDepthEnabled  = 0,  // load — depth already filled by geometry
        .noCull                 = 1);
}

void VulkanSkyboxPass::added() {
    utils::signalSubscribe("swapchainCreated", swapchainCreated);
    swapchainCreated(NULL);
}

void VulkanSkyboxPass::preUpdate() {
    if (vulkan.skipFrame) return;

    /* The skybox appends camera-motion velocity to the velocity buffer, so
     * transition it back to an attachment layout (earlier temporal passes
     * may have left it in SHADER_READ_ONLY_OPTIMAL). */
    VulkanImage* velocityImage       = vulkanFrameResourcesGetVelocity();
    if (velocityImage) {
        vulkanTransition(vulkan.currentCmd, velocityImage,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    }

    vulkanResetProfile(vulkan.currentCmd, &skyboxPipe.profile, 0);
}

void VulkanSkyboxPass::update() {
    if (vulkan.skipFrame) return;

    VulkanCommand* cmd = vulkan.currentCmd;
    if (!cmd) return;

    VulkanImage* sceneColor    = vulkanFrameResourcesGetSceneColor();
    VulkanImage* depthImage    = vulkanFrameResourcesGetDepth();
    VulkanImage* velocityImage = vulkanFrameResourcesGetVelocity();

    if (!sceneColor || !depthImage || !velocityImage) return;

    vulkanBeginProfile(cmd, &skyboxPipe.profile, 0);

    vulkanBeginRender(.cmd     = cmd,
                      .pipe    = &skyboxPipe,
                      .color1  = sceneColor,
                      .color2  = velocityImage,
                      .depth   = depthImage);

    vulkanViewport(cmd,
                   0,
                   sceneColor->extent.height,
                   sceneColor->extent.width,
                   -((i32)sceneColor->extent.height));
    vulkanScissor(cmd, 0, 0, sceneColor->extent.width, sceneColor->extent.height);

    vulkanBindPipe(cmd, &skyboxPipe);

    // Skybox shader uses gl_VertexIndex to generate a fullscreen quad (3 vertices)
    // No vertex/index buffers needed — the vertex shader generates positions
    vkCmdDraw(cmd->cmd, 3, 1, 0, 0);

    renderer.drawCalls++;
    renderer.triangleCount++;

    vulkanEndRender(cmd);
    vulkanEndProfile(cmd, &skyboxPipe.profile, 0);
    elapsedGPU = skyboxPipe.profile.elapsed;
}

void VulkanSkyboxPass::postUpdate() {
    vulkanSkyboxPass.cpuElapsed = elapsedCPU;
    vulkanSkyboxPass.gpuElapsed = elapsedGPU;
}

void VulkanSkyboxPass::removed() {
    vulkanDestroyPipe(&skyboxPipe);
}
}  // namespace engine
