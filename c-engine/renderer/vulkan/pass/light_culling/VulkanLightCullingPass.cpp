#include "VulkanLightCullingPass.h"
#include "VulkanLightCullingPass.h"
#include "ecs/Ecs.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/window/WindowSystem.h"
#include "renderer/Renderer.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanBuffer.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"

namespace engine {

VulkanLightCullingPass vulkanLightCullingPass;

VulkanLightCullingPass::VulkanLightCullingPass() : System("light_culling") {}

/* Must match shaders/includes/globalset.shader */
#define MAX_LIGHTS_PER_TILE 64u

typedef struct LightCullingPC {
    u32   tileCountX;
    u32   tileCountY;
    float zNear;
    float zFar;
} LightCullingPC;

static VulkanPipe   pipe;
static VulkanBuffer lightGridBuffer;
static VulkanBuffer lightIndexBuffer;
static char         buffersReady;
static u32          allocatedTileCount;

static void createBuffers(u32 tileCount) {
    if (buffersReady && allocatedTileCount >= tileCount) return;

    if (buffersReady) {
        vulkanDestroyBuffer(&lightGridBuffer,  NULL);
        vulkanDestroyBuffer(&lightIndexBuffer, NULL);
        lightGridBuffer    = VulkanBuffer{};
        lightIndexBuffer   = VulkanBuffer{};
        buffersReady       = 0;
        allocatedTileCount = 0;
    }

    /* LightGrid: one uvec2 (8 bytes) per tile */
    lightGridBuffer =
        vulkanCreateGpuBuffer("lightGrid",
                              (u64)tileCount * sizeof(u32) * 2,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    /* LightIndexList: MAX_LIGHTS_PER_TILE uints per tile */
    lightIndexBuffer =
        vulkanCreateGpuBuffer("lightIndexList",
                              (u64)tileCount * MAX_LIGHTS_PER_TILE * sizeof(u32),
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    /* Upload addresses so all shaders can reach them via AddressBuffer */
    vulkanResourceSetLightBuffers(lightGridBuffer.address,
                                  lightIndexBuffer.address);
    buffersReady       = 1;
    allocatedTileCount = tileCount;

    utils::info("light_culling: allocated %u tiles (%u lights/tile)",
         allocatedTileCount, MAX_LIGHTS_PER_TILE);
}

void VulkanLightCullingPass::added() {
    pipe = vulkanCreatePipe(
        .name = "light_culling",
        .comp = "shaders/pass/light_culling/spv/light_culling.comp.spv");
}

void VulkanLightCullingPass::preUpdate() {
    if (!vulkan.skipFrame && window.renderWidth > 0 && window.renderHeight > 0) {
        u32 tileCountX = ((u32)window.renderWidth  + 15u) / 16u;
        u32 tileCountY = ((u32)window.renderHeight + 15u) / 16u;
        u32 tileCount  = tileCountX * tileCountY;
        if (tileCount > 0) createBuffers(tileCount);
    }

    vulkanResetProfile(vulkan.currentCmd, &pipe.profile, 0);
}

void VulkanLightCullingPass::update() {
    if (vulkan.skipFrame) return;

    Entity* camEntity = cameraGetEntity();
    Camera* camera    = getComponent(camEntity->scene, Camera, camEntity->id);
    if (!camera) return;

    u32 tileCountX = ((u32)window.renderWidth  + 15u) / 16u;
    u32 tileCountY = ((u32)window.renderHeight + 15u) / 16u;
    u32 tileCount  = tileCountX * tileCountY;

    if (tileCountX == 0 || tileCountY == 0) return;
    if (!buffersReady || allocatedTileCount < tileCount) createBuffers(tileCount);

    VulkanCommand* cmd  = vulkan.currentCmd;
    VulkanImage*  depth = vulkanFrameResourcesGetDepth();

    /* Transition depth to shader read for compute sampling */
    if (depth) vulkanTransition(cmd, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    vulkanBeginProfile(cmd, &pipe.profile, 0);
    vulkanBindPipe(cmd, &pipe);

    LightCullingPC pc = {
        .tileCountX = tileCountX,
        .tileCountY = tileCountY,
        .zNear      = camera->cameraUbo.znear,
        .zFar       = camera->cameraUbo.zfar,
    };
    vulkanPush(cmd, &pipe, sizeof(LightCullingPC), &pc);

    /* Dispatch one workgroup per tile */
    vulkanDispatch(cmd, &pipe, tileCountX, tileCountY, 1);

    vulkanEndProfile(cmd, &pipe.profile, 0);

    /* Barrier: compute writes → fragment reads */
    VkMemoryBarrier2 writeDone = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .pNext         = nullptr,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
    };
    VkDependencyInfo depDone = {
        .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext                    = nullptr,
        .dependencyFlags          = 0,
        .memoryBarrierCount       = 1,
        .pMemoryBarriers          = &writeDone,
        .bufferMemoryBarrierCount = 0,
        .pBufferMemoryBarriers    = nullptr,
        .imageMemoryBarrierCount  = 0,
        .pImageMemoryBarriers     = nullptr,
    };
    vkCmdPipelineBarrier2(cmd->cmd, &depDone);

    /* Transition depth back for subsequent graphics passes */
    if (depth) vulkanTransition(cmd, depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, 1);

    vulkanLightCullingPass.gpuElapsed = pipe.profile.elapsed;
}

void VulkanLightCullingPass::removed() {
    vulkanDestroyPipe(&pipe);

    if (buffersReady) {
        vulkanDestroyBuffer(&lightGridBuffer,  NULL);
        vulkanDestroyBuffer(&lightIndexBuffer, NULL);
        lightGridBuffer    = VulkanBuffer{};
        lightIndexBuffer   = VulkanBuffer{};
        buffersReady       = 0;
        allocatedTileCount = 0;
    }
}
}  // namespace engine
