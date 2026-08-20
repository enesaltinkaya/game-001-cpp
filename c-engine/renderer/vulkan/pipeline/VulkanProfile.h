#pragma once

#include "renderer/Renderer.h"

struct VulkanCommand;

// Pipeline statistics counters (matches VkPipelineStatisticFlags)
typedef struct VulkanPipelineStats {
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
} VulkanPipelineStats;

typedef struct VulkanProfile {
    u64 results[2];
    VkQueryPool pools[FRAMES_IN_FLIGHT];
    char used[FRAMES_IN_FLIGHT]; // set to 1 after first successful timestamp write
    double elapsed;

    // Pipeline statistics (optional, enabled when stats gui is visible)
    VkQueryPool statPools[FRAMES_IN_FLIGHT];
    char statUsed[FRAMES_IN_FLIGHT];
    char hasStatPools;
    VulkanPipelineStats stats;
} VulkanProfile;

struct VulkanProfile vulkanCreateProfile(const char* name);
void vulkanDestroyProfile(struct VulkanProfile* profile);

void vulkanResetProfile(struct VulkanCommand* cmd, struct VulkanProfile* profile, char force);
void vulkanBeginProfile(struct VulkanCommand* cmd, struct VulkanProfile* profile, char force);
void vulkanEndProfile(struct VulkanCommand* cmd, struct VulkanProfile* profile, char force);

// Pipeline statistics helpers
void vulkanResetProfileStats(struct VulkanCommand* cmd, struct VulkanProfile* profile, char force);
void vulkanBeginProfileStats(struct VulkanCommand* cmd, struct VulkanProfile* profile, char force);
void vulkanEndProfileStats(struct VulkanCommand* cmd, struct VulkanProfile* profile, char force);
