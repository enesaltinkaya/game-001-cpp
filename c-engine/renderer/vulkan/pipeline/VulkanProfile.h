#pragma once

#include "renderer/Renderer.h"

namespace engine {
struct VulkanCommand;

// Pipeline statistics counters (matches VkPipelineStatisticFlags)
struct VulkanPipelineStats {
    u64 inputAssemblyVertices;
    u64 inputAssemblyPrimitives;
    u64 vertexShaderInvocations;
    u64 tessellationControlPatches;
    u64 tessellationEvalInvocations;
    u64 clippingInvocations;
    u64 clippingPrimitives;
    u64 fragmentShaderInvocations;
    u64 computeShaderInvocations;
    u64 vertexCount;       // total vertices consumed by rasterizer
    u64 primitiveCount;    // total primitives consumed by rasterizer
};

struct VulkanProfile {
    u64 results[2];
    VkQueryPool pools[FRAMES_IN_FLIGHT];
    bool used[FRAMES_IN_FLIGHT]; // set to true after first successful timestamp write
    double elapsed;

    // Pipeline statistics (optional, enabled when stats gui is visible)
    VkQueryPool statPools[FRAMES_IN_FLIGHT];
    bool statUsed[FRAMES_IN_FLIGHT];
    bool hasStatPools;
    VulkanPipelineStats stats;
};

struct VulkanProfile vulkanCreateProfile(const char* name);
void vulkanDestroyProfile(struct VulkanProfile* profile);

void vulkanResetProfile(struct VulkanCommand* cmd, struct VulkanProfile* profile, bool force);
void vulkanBeginProfile(struct VulkanCommand* cmd, struct VulkanProfile* profile, bool force);
void vulkanEndProfile(struct VulkanCommand* cmd, struct VulkanProfile* profile, bool force);

// Pipeline statistics helpers
void vulkanResetProfileStats(struct VulkanCommand* cmd, struct VulkanProfile* profile, bool force);
void vulkanBeginProfileStats(struct VulkanCommand* cmd, struct VulkanProfile* profile, bool force);
void vulkanEndProfileStats(struct VulkanCommand* cmd, struct VulkanProfile* profile, bool force);
}  // namespace engine
