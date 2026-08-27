#include "VulkanSsgiPass.h"
#include "ecs/Ecs.h"
#include "ecs/system/System.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/window/WindowSystem.h"
#include "events/Events.h"
#include "logger/Logger.h"
#include "renderer/Renderer.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"

namespace engine {

VulkanSsgiPass vulkanSsgiPass;

VulkanSsgiPass::VulkanSsgiPass() : System("ssgi") {}

/* ── Push constants (must match ssgi*.comp) ─────────────────────── */

typedef struct SsgiPushConstants {
    u32 sceneColorIndex;
    u32 depthIndex;
    u32 worldNormalIndex;
    u32 materialIndex;
    u32 outputIndex;
    u32 width;
    u32 height;
    float maxDistance;
} SsgiPushConstants;

typedef struct SsgiSpatialPushConstants {
    u32 inputIndex;
    u32 depthIndex;
    u32 normalIndex;
    u32 outputIndex;
    u32 width;
    u32 height;
    float nearZ;
    float farZ;
} SsgiSpatialPushConstants;

typedef struct SsgiTemporalPushConstants {
    u32 currentIndex;
    u32 historyIndex;
    u32 velocityIndex;
    u32 depthIndex;
    u32 normalIndex;
    u32 outputIndex;
    u32 width;
    u32 height;
    u32 historyValid;
    float jitterDeltaX;
    float jitterDeltaY;
} SsgiTemporalPushConstants;

/* ── State ───────────────────────────────────────────────────────── */

static VulkanPipe ssgiPipe;
static VulkanPipe spatialPipe;
static VulkanPipe temporalPipe;

static VulkanImage rawImage;
static VulkanImage filteredImage;
static VulkanImage historyImages[2];
static int historyIndex;
static u32 historyWidth;
static u32 historyHeight;
static char historyValid;
static char csDisabled;
static char firstFrameDone;
static float ssgiDistance = 10.0f;

/* ── Helpers ─────────────────────────────────────────────────────── */

static void clearOutput(void) {
    historyValid = 0;
    vulkanResourceSetSsgiImageIndex(0);
    vulkanSsgiPass.gpuElapsed = 0;
}

static void destroyHistory(void) {
    for (int i = 0; i < 2; ++i) {
        if (historyImages[i].img) {
            vulkanDestroyImage(&historyImages[i], NULL);
            historyImages[i] = VulkanImage{};
        }
    }

    historyIndex  = 0;
    historyWidth  = 0;
    historyHeight = 0;
    historyValid  = 0;
}

static void ensureHistory(u32 w, u32 h) {
    if (historyWidth == w && historyHeight == h && historyImages[0].img) {
        return;
    }

    destroyHistory();

    historyWidth  = w;
    historyHeight = h;

    for (int i = 0; i < 2; ++i) {
        char nameBuf[48];
        snprintf(nameBuf, sizeof(nameBuf), "SsgiHistory%d", i);
        historyImages[i] =
            vulkanCreateImage(.name   = nameBuf,
                              .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                              .usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              .width  = (int)w,
                              .height = (int)h);
    }
}

static void swapchainCreated(void*) {
    if (filteredImage.img) {
        vulkanDestroyImage(&filteredImage, NULL);
        filteredImage = VulkanImage{};
    }
    if (rawImage.img) {
        vulkanDestroyImage(&rawImage, NULL);
        rawImage = VulkanImage{};
    }
    destroyHistory();
}

static void ensureImages(void) {
    if (rawImage.img && filteredImage.img) {
        return;
    }
    if (window.renderWidth <= 0 || window.renderHeight <= 0) {
        return;
    }

    rawImage = vulkanCreateImage(.name   = "ssgi_raw",
                                 .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                 .usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                 .width  = window.renderWidth,
                                 .height = window.renderHeight);

    filteredImage =
        vulkanCreateImage(.name   = "ssgi_filtered",
                          .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                          .usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          .width  = window.renderWidth,
                          .height = window.renderHeight);
}

static const VkMemoryBarrier2 computeBarrier = {
    .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
    .pNext         = nullptr,
    .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
    .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
    .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
    .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
};

static const VkDependencyInfo computeDepInfo = {
    .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
    .pNext                    = nullptr,
    .dependencyFlags          = 0,
    .memoryBarrierCount       = 1,
    .pMemoryBarriers          = &computeBarrier,
    .bufferMemoryBarrierCount = 0,
    .pBufferMemoryBarriers    = nullptr,
    .imageMemoryBarrierCount  = 0,
    .pImageMemoryBarriers     = nullptr,
};

/* ── Pass callbacks ──────────────────────────────────────────────── */

void VulkanSsgiPass::added() {
    utils::signalSubscribe("swapchainCreated", swapchainCreated);

    ssgiPipe =
        vulkanCreatePipe(.name = "ssgi",
                         .comp = "shaders/pass/ssgi/spv/ssgi.comp.spv");
    spatialPipe =
        vulkanCreatePipe(.name = "ssgi_spatial",
                         .comp = "shaders/pass/ssgi/spv/ssgi_spatial.comp.spv");
    temporalPipe =
        vulkanCreatePipe(.name = "ssgi_temporal",
                         .comp = "shaders/pass/ssgi/spv/ssgi_temporal.comp.spv");

    const char* env = getenv("ENGINE_SSGI_DISABLED");
    if (env && *env && atoi(env)) {
        csDisabled = 1;
    }
}

void VulkanSsgiPass::preUpdate() {
    vulkanResetProfile(vulkan.currentCmd, &ssgiPipe.profile, 0);
}

void VulkanSsgiPass::update() {
    if (vulkan.skipFrame) {
        return;
    }

    /* The scene color buffer is only valid from the second pass run
     * (it is written by the scene pass, which runs after us). */
    if (!firstFrameDone) {
        firstFrameDone = 1;
        clearOutput();
        return;
    }

    if (csDisabled) {
        clearOutput();
        return;
    }

    VulkanImage* depthImg   = vulkanFrameResourcesGetDepth();
    VulkanImage* normalImg  = vulkanFrameResourcesGetWorldNormal();
    VulkanImage* colorImg   = vulkanFrameResourcesGetSceneColor();
    VulkanImage* materialImg = vulkanFrameResourcesGetMaterial();
    if (!depthImg || !depthImg->img || !normalImg || !normalImg->img ||
        !colorImg || !colorImg->img || !materialImg || !materialImg->img) {
        clearOutput();
        return;
    }

    Entity* camEntity = cameraGetEntity();
    if (!camEntity) {
        clearOutput();
        return;
    }

    Camera* camera = getComponent(camEntity->scene, Camera, camEntity->id);
    if (!camera) {
        clearOutput();
        return;
    }

    ensureImages();
    if (!rawImage.img || !filteredImage.img) {
        clearOutput();
        return;
    }

    const u32 w       = depthImg->extent.width;
    const u32 h       = depthImg->extent.height;
    const u32 groupsX = (w + 7u) / 8u;
    const u32 groupsY = (h + 7u) / 8u;

    VulkanImage* velocityImg = vulkanFrameResourcesGetVelocity();

    char useTemporal = velocityImg && velocityImg->img;
    if (!useTemporal) {
        historyValid = 0;
    }

    float jitterX = camera->cameraUbo.jitterX;
    float jitterY = camera->cameraUbo.jitterY;

    VulkanCommand* cmd = vulkan.currentCmd;

    /* ── 1. Raw raymarch ──────────────────────────────────────────── */

    vulkanTransition(cmd, depthImg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, normalImg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, colorImg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, materialImg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    if (useTemporal) {
        vulkanTransition(cmd, velocityImg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    }
    vulkanTransition(cmd, &rawImage, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

    vulkanBeginProfile(cmd, &ssgiPipe.profile, 0);
    vulkanBindPipe(cmd, &ssgiPipe);

    SsgiPushConstants pc = {
        .sceneColorIndex  = (u32)colorImg->sampledPoolIndex,
        .depthIndex       = (u32)depthImg->sampledPoolIndex,
        .worldNormalIndex = (u32)normalImg->sampledPoolIndex,
        .materialIndex    = (u32)materialImg->sampledPoolIndex,
        .outputIndex      = (u32)rawImage.storagePoolIndex,
        .width            = w,
        .height           = h,
        .maxDistance      = ssgiDistance,
    };
    vulkanPush(cmd, &ssgiPipe, sizeof(pc), &pc);
    vulkanDispatch(cmd, &ssgiPipe, groupsX, groupsY, 1);

    vulkanEndProfile(cmd, &ssgiPipe.profile, 0);

    vkCmdPipelineBarrier2(cmd->cmd, &computeDepInfo);

    /* ── 2. Spatial filter ────────────────────────────────────────── */

    vulkanTransition(cmd, &rawImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, &filteredImage, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

    vulkanBindPipe(cmd, &spatialPipe);

    SsgiSpatialPushConstants spc = {
        .inputIndex  = (u32)rawImage.sampledPoolIndex,
        .depthIndex  = (u32)depthImg->sampledPoolIndex,
        .normalIndex = (u32)normalImg->sampledPoolIndex,
        .outputIndex = (u32)filteredImage.storagePoolIndex,
        .width       = w,
        .height      = h,
        .nearZ       = camera->znear,
        .farZ        = camera->zfar,
    };
    vulkanPush(cmd, &spatialPipe, sizeof(spc), &spc);
    vulkanDispatch(cmd, &spatialPipe, groupsX, groupsY, 1);

    u32 finalIndex = 0;

    /* ── 3. Temporal filter ───────────────────────────────────────── */

    if (useTemporal) {
        ensureHistory(w, h);

        VulkanImage* history = &historyImages[historyIndex];
        VulkanImage* output  = &historyImages[1 - historyIndex];

        vkCmdPipelineBarrier2(cmd->cmd, &computeDepInfo);

        vulkanTransition(cmd, &filteredImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, history, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, output, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

        vulkanBindPipe(cmd, &temporalPipe);

        float prevJitterX = camera->cameraUbo.prevJitterX;
        float prevJitterY = camera->cameraUbo.prevJitterY;

        SsgiTemporalPushConstants tpc = {
            .currentIndex  = (u32)filteredImage.sampledPoolIndex,
            .historyIndex  = (u32)history->sampledPoolIndex,
            .velocityIndex = (u32)velocityImg->sampledPoolIndex,
            .depthIndex    = (u32)depthImg->sampledPoolIndex,
            .normalIndex   = (u32)normalImg->sampledPoolIndex,
            .outputIndex   = (u32)output->storagePoolIndex,
            .width         = w,
            .height        = h,
            .historyValid  = historyValid ? 1u : 0u,
            .jitterDeltaX  = jitterX - prevJitterX,
            .jitterDeltaY  = jitterY - prevJitterY,
        };
        vulkanPush(cmd, &temporalPipe, sizeof(tpc), &tpc);
        vulkanDispatch(cmd, &temporalPipe, groupsX, groupsY, 1);

        vkCmdPipelineBarrier2(cmd->cmd, &computeDepInfo);
        vulkanTransition(cmd, output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

        finalIndex   = (u32)output->sampledPoolIndex;
        historyIndex = 1 - historyIndex;
        historyValid = 1;
    } else {
        vulkanTransition(cmd, &filteredImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        finalIndex = (u32)filteredImage.sampledPoolIndex;
    }

    /* Restore layouts expected by later passes. */
    vulkanTransition(cmd, depthImg, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, 1);
    vulkanTransition(cmd, normalImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    vulkanTransition(cmd, colorImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    vulkanTransition(cmd, materialImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    if (useTemporal) {
        vulkanTransition(cmd, velocityImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    }

    vulkanResourceSetSsgiImageIndex(finalIndex);
    vulkanSsgiPass.gpuElapsed = ssgiPipe.profile.elapsed;
}

void VulkanSsgiPass::removed() {
    if (filteredImage.img) {
        vulkanDestroyImage(&filteredImage, NULL);
        filteredImage = VulkanImage{};
    }
    if (rawImage.img) {
        vulkanDestroyImage(&rawImage, NULL);
        rawImage = VulkanImage{};
    }
    destroyHistory();

    vulkanDestroyPipe(&ssgiPipe);
    vulkanDestroyPipe(&spatialPipe);
    vulkanDestroyPipe(&temporalPipe);
}

/* ── Public API ──────────────────────────────────────────────────── */

void vulkanSsgiPassSetDisabled(char disabled) {
    csDisabled = disabled;
    if (disabled) {
        historyValid = 0;
    }
    utils::info("SSGI: %s", disabled ? "disabled" : "enabled");
}

char vulkanSsgiPassIsDisabled(void) {
    return csDisabled;
}

void vulkanSsgiPassSetDistance(float distance) {
    ssgiDistance = distance;
    utils::info("SSGI ray distance: %.2f", (double)distance);
}

float vulkanSsgiPassGetDistance(void) {
    return ssgiDistance;
}

VulkanImage* vulkanSsgiPassGetOutput(void) {
    if (csDisabled) return nullptr;
    /* After the historyIndex flip, historyImages[historyIndex] holds the
     * freshest accumulation. */
    if (historyValid && historyImages[historyIndex].img) {
        return &historyImages[historyIndex];
    }
    return filteredImage.img ? &filteredImage : nullptr;
}

VulkanImage* vulkanSsgiPassGetRawOutput(void) {
    return rawImage.img ? &rawImage : nullptr;
}
}  // namespace engine
