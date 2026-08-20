#include "VulkanCullingPass.h"
#include "ecs/system/scene/Scene.h"
#include "events/Events.h"
#include "renderer/Renderer.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pass/hiz/VulkanHiZPass.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/scene/VulkanScene.h"
#include "renderer/vulkan/scene/VulkanVisibleScenes.h"

static void added(void);
static void preUpdate(void);
static void update(void);
static void postUpdate(void);
static void removed(void);

static double elapsedCPU;
static double elapsedGPU;

System vulkanCullingPass = {
    .name       = "culling",
    .added      = added,
    .preUpdate  = preUpdate,
    .update     = update,
    .postUpdate = postUpdate,
    .removed    = removed,
};

static VulkanPipe cullingPipe;

typedef struct CullingPushConstants {
    u64 drawInstanceBufferAddress;
    u64 transformBufferAddress;
    u64 indirectBufferAddress;
    u64 culledBufferAddress;
    u64 drawCountBufferAddress;
    u64 dsIndirectBufferAddress;
    u64 dsCulledBufferAddress;
    u64 dsDrawCountBufferAddress;
    u64 transIndirectBufferAddress;
    u64 transCulledBufferAddress;
    u64 transDrawCountBufferAddress;
    u64 visibilityBufferAddress;
    u32 maxDrawInstances;
    u32 hizTextureIndex;
} CullingPushConstants;

static void recreatePipeline(void) {
    if (cullingPipe.pipe) {
        vulkanDestroyPipe(&cullingPipe);
    }

    cullingPipe = vulkanCreatePipe(
        .name = "scene_culling",
        .comp = "shaders/pass/scene/spv/scene_culling.comp.spv");
}

static void swapchainCreated(void*) {
    recreatePipeline();
}

static void added(void) {
    signalSubscribe("swapchainCreated", swapchainCreated);
    recreatePipeline();
}

static void preUpdate(void) {
    if (vulkan.skipFrame) return;
    vulkanResetProfile(vulkan.currentCmd, &cullingPipe.profile, 0);
}

static void dispatchCulling(VulkanCommand* cmd, VulkanScene* vs, u32 fi) {
    // Zero the draw count buffers
    vkCmdFillBuffer(cmd->cmd, vs->drawCountBuffer[fi].buf, 0, sizeof(u32), 0);
    vkCmdFillBuffer(cmd->cmd, vs->dsDrawCountBuffer[fi].buf, 0, sizeof(u32), 0);
    vkCmdFillBuffer(cmd->cmd, vs->transDrawCountBuffer[fi].buf, 0, sizeof(u32), 0);

    // Barrier: transfer → compute
    VkMemoryBarrier fillBarrier = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
    };
    vkCmdPipelineBarrier(cmd->cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &fillBarrier, 0, NULL, 0, NULL);

    // Dispatch culling compute
    vulkanBindPipe(cmd, &cullingPipe);

    // Use previous frame's HiZ for occlusion culling
    VulkanImage* prevHiZ = vulkanHiZGetPreviousImage();
    u32 hizIdx = prevHiZ ? (u32)prevHiZ->sampledPoolIndex : 0xFFFFFFFFu;

    CullingPushConstants pc = {
        .drawInstanceBufferAddress  = vs->drawInstanceBuffer.address,
        .transformBufferAddress     = vs->transformBuffer[fi].address,
        .indirectBufferAddress      = vs->indirectBuffer[fi].address,
        .culledBufferAddress        = vs->culledBuffer[fi].address,
        .drawCountBufferAddress     = vs->drawCountBuffer[fi].address,
        .dsIndirectBufferAddress    = vs->dsIndirectBuffer[fi].address,
        .dsCulledBufferAddress      = vs->dsCulledBuffer[fi].address,
        .dsDrawCountBufferAddress   = vs->dsDrawCountBuffer[fi].address,
        .transIndirectBufferAddress = vs->transIndirectBuffer[fi].address,
        .transCulledBufferAddress   = vs->transCulledBuffer[fi].address,
        .transDrawCountBufferAddress = vs->transDrawCountBuffer[fi].address,
        .visibilityBufferAddress    = vs->visibilityBuffer[fi].address,
        .maxDrawInstances           = vs->totalDraws,
        .hizTextureIndex            = hizIdx,
    };
    vulkanPush(cmd, &cullingPipe, sizeof(pc), &pc);

    int groupCount = (vs->totalDraws + 63) / 64;
    vulkanDispatch(cmd, &cullingPipe, groupCount, 1, 1);

    // Barrier: compute → draw indirect
    VkMemoryBarrier computeBarrier = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
    };
    vkCmdPipelineBarrier(cmd->cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                         0, 1, &computeBarrier, 0, NULL, 0, NULL);
}

static void update(void) {
    if (vulkan.skipFrame) return;

    VulkanCommand* cmd = vulkan.currentCmd;
    u32 fi             = renderer.flightIndex;

    vulkanBeginProfile(cmd, &cullingPipe.profile, 0);

    u32 visibleSceneCount = 0;
    Scene** visibleScenes = vulkanGetVisibleScenes(&visibleSceneCount);
    u32 totalDraws = 0;
    for (u32 si = 0; si < visibleSceneCount; si++) {
        Scene* scene = visibleScenes[si];
        if (!scene->backendScene) continue;
        VulkanScene* vs  = static_cast<VulkanScene*>(scene->backendScene);
        if (!vs->totalDraws) continue;
        totalDraws += vs->totalDraws;
        dispatchCulling(cmd, vs, fi);
    }

    vulkanEndProfile(cmd, &cullingPipe.profile, 0);
    elapsedGPU = cullingPipe.profile.elapsed;


}

static void postUpdate(void) {
    vulkanCullingPass.cpuElapsed = elapsedCPU;
    vulkanCullingPass.gpuElapsed = elapsedGPU;
}

static void removed(void) {
    vulkanDestroyPipe(&cullingPipe);
}
