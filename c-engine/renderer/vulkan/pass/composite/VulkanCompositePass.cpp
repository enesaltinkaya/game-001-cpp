#include "VulkanCompositePass.h"
#include "renderer/vulkan/pass/azgaar_weather/VulkanAzgaarWeatherPass.h"
#include "renderer/vulkan/pass/volumetric/VulkanVolumetricPass.h"
#include "ecs/system/System.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"

static void added(void);
static void preUpdate(void);
static void update(void);
static void postUpdate(void);
static void removed(void);

static VulkanPipe pipeline;
static double     elapsedCPU;
static double     elapsedGPU;

System vulkanCompositePass = {
    .name                = "composite",
    .added               = added,
    .removed             = removed,
    .preUpdate           = preUpdate,
    .update              = update,
    .postUpdate          = postUpdate,
    .cpuElapsedLastFrame = 0.0,
    .cpuElapsed          = 0.0,
    .gpuElapsed          = 0.0,
    .priority            = 0,
};

typedef struct CompositePushConstants {
    u32 sceneColorIndex;
    u32 ssrColorIndex;
    u32 depthIndex;
    u32 normalsIndex;
    u32 materialIndex;
    u32 outputImageIndex;
    u32 volumetricColorIndex;
    u32 weatherMaskIndex;
    u32 width;
    u32 height;
} CompositePushConstants;

static void added(void) {
    pipeline = vulkanCreatePipe(
        .name = "composite",
        .comp = "shaders/pass/composite/spv/composite.comp.spv");

}

static void preUpdate(void) {
    vulkanResetProfile(vulkan.currentCmd, &pipeline.profile, 0);
}

static void update(void) {
    elapsedCPU = nanos();

    if (vulkan.skipFrame) {
        elapsedCPU = nanos() - elapsedCPU;
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
        elapsedCPU = nanos() - elapsedCPU;
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
    elapsedCPU = nanos() - elapsedCPU;
}

static void postUpdate(void) {
    vulkanCompositePass.cpuElapsed = elapsedCPU;
    vulkanCompositePass.gpuElapsed = elapsedGPU;
}

static void removed(void) {
    vulkanDestroyPipe(&pipeline);
}
