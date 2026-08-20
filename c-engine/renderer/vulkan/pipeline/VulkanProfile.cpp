#include "VulkanProfile.h"
#include "../Vulkan.h"
#include "Utils.h"
#include "ecs/Ecs.h"
#include "renderer/Renderer.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/utils/VulkanUtils.h"

struct VulkanProfile vulkanCreateProfile(const char* name) {
    struct VulkanProfile vulkanProfile = {};
    vulkanProfile.hasStatPools         = 0;

    // Timestamp query pool (2 queries: begin + end)
    VkQueryPoolCreateInfo createInfo   = {};
    createInfo.sType                   = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    createInfo.queryType               = VK_QUERY_TYPE_TIMESTAMP;
    createInfo.queryCount              = 2;
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        vkCreateQueryPool(vulkan.device, &createInfo, nullptr, &vulkanProfile.pools[i]);
        if (isDebug()) {
            vulkanUtilsSetName((u64)vulkanProfile.pools[i],
                               VK_OBJECT_TYPE_QUERY_POOL,
                               strtmp("%s%s #%d", "querypool ", name, i));
        }
    }

    // Pipeline statistics query pool (1 query per flight, carrying all flags)
    VkQueryPoolCreateInfo statCreateInfo = {};
    statCreateInfo.sType                 = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    statCreateInfo.queryType             = VK_QUERY_TYPE_PIPELINE_STATISTICS;
    statCreateInfo.pipelineStatistics    = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT
        | VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT
        | VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT
        | VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_CONTROL_SHADER_PATCHES_BIT
        | VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_EVALUATION_SHADER_INVOCATIONS_BIT
        | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT
        | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT
        | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT
        | VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
    statCreateInfo.queryCount            = 1;
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        vkCreateQueryPool(vulkan.device, &statCreateInfo, nullptr, &vulkanProfile.statPools[i]);
        if (isDebug()) {
            vulkanUtilsSetName((u64)vulkanProfile.statPools[i],
                               VK_OBJECT_TYPE_QUERY_POOL,
                               strtmp("%s%s_stats #%d", "querypool ", name, i));
        }
    }
    vulkanProfile.hasStatPools = 1;
    return vulkanProfile;
}

void vulkanDestroyProfile(struct VulkanProfile* profile) {
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        vkDestroyQueryPool(vulkan.device, profile->pools[i], nullptr);
    }
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        vkDestroyQueryPool(vulkan.device, profile->statPools[i], nullptr);
    }
}

void vulkanResetProfile(struct VulkanCommand* cmd, struct VulkanProfile* profile, bool force) {
    if (vulkan.skipFrame) {
        return;
    }
    if (cmd->transient) {
        return;
    }
    if (!force && !ecs.showStats) {
        return;
    }

    int fi = renderer.flightIndex;

    // Retrieve results from the PREVIOUS use of this flight index.
    // The fence for this flight index was waited at swapchain begin, so
    // the GPU has finished executing the previous command buffer and the
    // timestamps are guaranteed to be available — but only if the pool
    // was actually used in a previous frame (not freshly created).
    if (profile->used[fi]) {
        VkResult result = vkGetQueryPoolResults(vulkan.device,
                                                profile->pools[fi],
                                                0,
                                                2,
                                                16,
                                                profile->results,
                                                8,
                                                VK_QUERY_RESULT_64_BIT);
        if (result == VK_SUCCESS) {
            profile->elapsed = (double)(profile->results[1] - profile->results[0]) *
                               vulkan.deviceProperties.limits.timestampPeriod;
        } else {
            profile->elapsed = 0;
        }
    } else {
        profile->elapsed = 0;
    }

    // Reset for the new recording.
    vkCmdResetQueryPool(cmd->cmd, profile->pools[fi], 0, 2);
}

void vulkanBeginProfile(struct VulkanCommand* cmd, struct VulkanProfile* profile, bool force) {
    if (vulkan.skipFrame) {
        return;
    }
    if (cmd->transient) {
        return;
    }
    if (!force && !ecs.showStats) {
        return;
    }

    int fi = renderer.flightIndex;
    vkCmdWriteTimestamp(cmd->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, profile->pools[fi], 0);
}

void vulkanEndProfile(struct VulkanCommand* cmd, struct VulkanProfile* profile, bool force) {
    if (vulkan.skipFrame) {
        return;
    }
    if (cmd->transient) {
        return;
    }
    if (!force && !ecs.showStats) {
        return;
    }

    int fi = renderer.flightIndex;
    vkCmdWriteTimestamp(cmd->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, profile->pools[fi], 1);
    profile->used[fi] = 1;
}

void vulkanResetProfileStats(struct VulkanCommand* cmd, struct VulkanProfile* profile, bool force) {
    if (vulkan.skipFrame || cmd->transient || !profile->hasStatPools) {
        return;
    }
    if (!force && !ecs.showStats) {
        return;
    }

    int fi = renderer.flightIndex;

    // Retrieve pipeline stats from the previous use of this flight index.
    // vkCmdCopyQueryPoolResults writes 10 u64 values (one per statistic flag).
    if (profile->statUsed[fi]) {
        u64 results[10] = {};
        VkResult result = vkGetQueryPoolResults(vulkan.device,
                                                profile->statPools[fi],
                                                0, 1,
                                                10 * sizeof(u64),
                                                results,
                                                sizeof(u64),
                                                VK_QUERY_RESULT_64_BIT);
        if (result == VK_SUCCESS) {
            // Results are in the same order as the flags in pipelineStatistics:
            // 0: INPUT_ASSEMBLY_VERTICES
            // 1: INPUT_ASSEMBLY_PRIMITIVES
            // 2: VERTEX_SHADER_INVOCATIONS
            // 3: TESSELLATION_CONTROL_SHADER_PATCHES
            // 4: TESSELLATION_EVALUATION_SHADER_INVOCATIONS
            // 5: CLIPPING_INVOCATIONS
            // 6: CLIPPING_PRIMITIVES
            // 7: FRAGMENT_SHADER_INVOCATIONS
            // 8: COMPUTE_SHADER_INVOCATIONS
            profile->stats.inputAssemblyVertices      = results[0];
            profile->stats.inputAssemblyPrimitives     = results[1];
            profile->stats.vertexShaderInvocations     = results[2];
            profile->stats.tessellationControlPatches  = results[3];
            profile->stats.tessellationEvalInvocations = results[4];
            profile->stats.clippingInvocations         = results[5];
            profile->stats.clippingPrimitives          = results[6];
            profile->stats.fragmentShaderInvocations   = results[7];
            profile->stats.computeShaderInvocations    = results[8];
            // vertexCount and primitiveCount are not directly queryable via
            // pipeline stats; they map to inputAssemblyVertices/Primitives.
            profile->stats.vertexCount    = results[0];
            profile->stats.primitiveCount = results[1];
        }
    } else {
        profile->stats = VulkanPipelineStats{};
    }

    // Reset for the new recording.
    vkCmdResetQueryPool(cmd->cmd, profile->statPools[fi], 0, 1);
}

void vulkanBeginProfileStats(struct VulkanCommand* cmd, struct VulkanProfile* profile, bool force) {
    if (vulkan.skipFrame || cmd->transient || !profile->hasStatPools) {
        return;
    }
    if (!force && !ecs.showStats) {
        return;
    }

    int fi = renderer.flightIndex;
    vkCmdBeginQuery(cmd->cmd, profile->statPools[fi], 0, 0);
}

void vulkanEndProfileStats(struct VulkanCommand* cmd, struct VulkanProfile* profile, bool force) {
    if (vulkan.skipFrame || cmd->transient || !profile->hasStatPools) {
        return;
    }
    if (!force && !ecs.showStats) {
        return;
    }

    int fi = renderer.flightIndex;
    vkCmdEndQuery(cmd->cmd, profile->statPools[fi], 0);
    profile->statUsed[fi] = 1;
}
