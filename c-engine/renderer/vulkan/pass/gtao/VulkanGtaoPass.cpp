#include "VulkanGtaoPass.h"
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

static void added(void);
static void preUpdate(void);
static void update(void);
static void removed(void);

System vulkanGtaoPass = {
    .name      = "gtao",
    .added     = added,
    .preUpdate = preUpdate,
    .update    = update,
    .removed   = removed,
};

/* ── Push constants (must match GLSL) ────────────────────────────── */

typedef struct GtaoPushConstants {
    u32 depthIndex;
    u32 normalIndex;
    u32 outputIndex;
    u32 width;
    u32 height;
    float nearZ;
    float farZ;
    float projM00;
    float projM11;
    float jitterX;
    float jitterY;
    u32 frameIndex;
    u32 temporalAAActive; /* 1 = temporal accumulation is active */
    float radius;
    float falloffEnd;
    float strength;
} GtaoPushConstants;

typedef struct GtaoSpatialPushConstants {
    u32 inputIndex;
    u32 depthIndex;
    u32 normalIndex;
    u32 outputIndex;
    u32 width;
    u32 height;
    float nearZ;
    float farZ;
} GtaoSpatialPushConstants;

typedef struct GtaoTemporalPushConstants {
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
} GtaoTemporalPushConstants;

/* ── State ───────────────────────────────────────────────────────── */

static VulkanPipe gtaoPipe;
static VulkanPipe spatialPipe;
static VulkanPipe temporalPipe;

static VulkanImage gtaoRawImage;
static VulkanImage gtaoFilteredImage;
static VulkanImage gtaoHistoryImages[2];
static int gtaoHistoryIndex;
static u32 gtaoHistoryWidth;
static u32 gtaoHistoryHeight;
static char gtaoHistoryValid;
static char gtaoDisabled;
static float gtaoStrength = 1.0f;

/* ── Helpers ─────────────────────────────────────────────────────── */

/* Half-resolution GTAO — low-frequency effect, 4× pixel reduction. */
static u32 halfResWidth(u32 w) {
    return (w + 1u) / 2u;
}

static u32 halfResHeight(u32 h) {
    return (h + 1u) / 2u;
}

static void clearOutput(void) {
    gtaoHistoryValid = 0;
    vulkanResourceSetAoImageIndex(0);
    vulkanGtaoPass.gpuElapsed = 0;
}

static void destroyHistory(void) {
    for (int i = 0; i < 2; ++i) {
        if (gtaoHistoryImages[i].img) {
            vulkanDestroyImage(&gtaoHistoryImages[i], NULL);
            gtaoHistoryImages[i] = (VulkanImage){0};
        }
    }

    gtaoHistoryIndex  = 0;
    gtaoHistoryWidth  = 0;
    gtaoHistoryHeight = 0;
    gtaoHistoryValid  = 0;
}

static void ensureHistory(u32 w, u32 h) {
    if (gtaoHistoryWidth == w && gtaoHistoryHeight == h && gtaoHistoryImages[0].img) {
        return;
    }

    destroyHistory();

    gtaoHistoryWidth  = w;
    gtaoHistoryHeight = h;

    for (int i = 0; i < 2; ++i) {
        char nameBuf[32];
        snprintf(nameBuf, sizeof(nameBuf), "GtaoHistory%d", i);
        gtaoHistoryImages[i] =
            vulkanCreateImage(.name   = nameBuf,
                              .format = VK_FORMAT_R16_SFLOAT,
                              .usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                              .width  = (int)w,
                              .height = (int)h);
    }
}

static void swapchainCreated(void*) {
    if (gtaoFilteredImage.img) {
        vulkanDestroyImage(&gtaoFilteredImage, NULL);
        gtaoFilteredImage = (VulkanImage){0};
    }
    if (gtaoRawImage.img) {
        vulkanDestroyImage(&gtaoRawImage, NULL);
        gtaoRawImage = (VulkanImage){0};
    }
    destroyHistory();
}

static void ensureImages(void) {
    if (gtaoRawImage.img && gtaoFilteredImage.img) {
        return;
    }
    if (window.renderWidth <= 0 || window.renderHeight <= 0) {
        return;
    }

    u32 hw = halfResWidth((u32)window.renderWidth);
    u32 hh = halfResHeight((u32)window.renderHeight);

    gtaoRawImage =
        vulkanCreateImage(.name   = "gtao_raw",
                          .format = VK_FORMAT_R16_SFLOAT,
                          .usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          .width  = (int)hw,
                          .height = (int)hh);

    gtaoFilteredImage =
        vulkanCreateImage(.name   = "gtao_filtered",
                          .format = VK_FORMAT_R16_SFLOAT,
                          .usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          .width  = (int)hw,
                          .height = (int)hh);
}

static const VkMemoryBarrier2 computeBarrier = {
    .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
    .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
    .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
    .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
    .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
};

static const VkDependencyInfo computeDepInfo = {
    .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
    .memoryBarrierCount = 1,
    .pMemoryBarriers    = &computeBarrier,
};

/* ── Pass callbacks ──────────────────────────────────────────────── */

static void added(void) {
    signalSubscribe("swapchainCreated", swapchainCreated);

    gtaoPipe     = vulkanCreatePipe(.name = "gtao", .comp = "shaders/pass/gtao/spv/gtao.comp.spv");
    spatialPipe  = vulkanCreatePipe(.name = "gtao_spatial",
                                    .comp = "shaders/pass/gtao/spv/gtao_spatial.comp.spv");
    temporalPipe = vulkanCreatePipe(.name = "gtao_temporal",
                                    .comp = "shaders/pass/gtao/spv/gtao_temporal.comp.spv");

    const char* env = getenv("ENGINE_GTAO_DISABLED");
    if (env && *env && atoi(env)) {
        gtaoDisabled = 1;
    }
}

static void preUpdate(void) {
    vulkanResetProfile(vulkan.currentCmd, &gtaoPipe.profile, 0);
    vulkanResetProfile(vulkan.currentCmd, &spatialPipe.profile, 0);
    vulkanResetProfile(vulkan.currentCmd, &temporalPipe.profile, 0);
}

static void update(void) {
    if (vulkan.skipFrame) {
        return;
    }

    if (gtaoDisabled) {
        clearOutput();
        return;
    }

    VulkanImage* depthImg  = vulkanFrameResourcesGetDepth();
    VulkanImage* normalImg = vulkanFrameResourcesGetViewNormal();
    if (!depthImg || !depthImg->img || !normalImg || !normalImg->img) {
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
    if (!gtaoRawImage.img || !gtaoFilteredImage.img) {
        clearOutput();
        return;
    }

    /* Full-res dimensions (for texture access) and half-res dispatch. */
    const u32 fullW   = depthImg->extent.width;
    const u32 fullH   = depthImg->extent.height;
    const u32 halfW   = halfResWidth(fullW);
    const u32 halfH   = halfResHeight(fullH);
    const u32 groupsX = (halfW + 7u) / 8u;
    const u32 groupsY = (halfH + 7u) / 8u;

    char temporalAAActive    = rendererGetUpscalerMode() != RENDERER_UPSCALER_OFF;
    VulkanImage* velocityImg = vulkanFrameResourcesGetVelocity();

    /* Temporal accumulation is essential to smooth out per-pixel AO
     * noise and sub-pixel jitter variation when FSR / Native AA is
     * active.  Without it, GTAO noise "swims" with camera motion and
     * alpha-cutout edges (grass, foliage) flicker violently because
     * the raw GTAO + spatial filter cannot resolve sub-pixel changes.
     * The temporal shader uses a conservative blend (10% current for
     * static pixels) and variance clipping, mirroring the contact
     * shadow pass approach.  Double accumulation with FSR's own
     * temporal pass is harmless in practice.                              */
    char useTemporal = temporalAAActive && velocityImg && velocityImg->img;

    if (!useTemporal) {
        gtaoHistoryValid = 0;
    }

    float jitterX = temporalAAActive ? camera->cameraUbo.jitterX : 0.0f;
    float jitterY = temporalAAActive ? camera->cameraUbo.jitterY : 0.0f;

    VulkanCommand* cmd = vulkan.currentCmd;

    /* ── 1. Raw GTAO (half-res dispatch) ────────────────────────── */

    vulkanTransition(cmd, depthImg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, normalImg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    if (useTemporal) {
        vulkanTransition(cmd, velocityImg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    }
    vulkanTransition(cmd, &gtaoRawImage, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

    vulkanBeginProfile(cmd, &gtaoPipe.profile, 0);
    vulkanBindPipe(cmd, &gtaoPipe);

    GtaoPushConstants pc = {
        .depthIndex       = (u32)depthImg->sampledPoolIndex,
        .normalIndex      = (u32)normalImg->sampledPoolIndex,
        .outputIndex      = (u32)gtaoRawImage.storagePoolIndex,
        .width            = fullW,
        .height           = fullH,
        .nearZ            = camera->znear,
        .farZ             = camera->zfar,
        .projM00          = camera->cameraUbo.projection[0][0],
        .projM11          = camera->cameraUbo.projection[1][1],
        .jitterX          = jitterX,
        .jitterY          = jitterY,
        .frameIndex       = temporalAAActive ? (camera->frameIndex % 64u) : 0u,
        .temporalAAActive = temporalAAActive ? 1u : 0u,
        .radius           = 2.0f,
        .falloffEnd       = 4.0f,
        .strength         = gtaoStrength,
    };
    vulkanPush(cmd, &gtaoPipe, sizeof(pc), &pc);
    vulkanDispatch(cmd, &gtaoPipe, groupsX, groupsY, 1);

    vulkanEndProfile(cmd, &gtaoPipe.profile, 0);

    vkCmdPipelineBarrier2(cmd->cmd, &computeDepInfo);

    /* ── 2. Spatial filter (half-res dispatch) ──────────────────── */

    vulkanTransition(cmd, &gtaoRawImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, &gtaoFilteredImage, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

    vulkanBindPipe(cmd, &spatialPipe);

    GtaoSpatialPushConstants spc = {
        .inputIndex  = (u32)gtaoRawImage.sampledPoolIndex,
        .depthIndex  = (u32)depthImg->sampledPoolIndex,
        .normalIndex = (u32)normalImg->sampledPoolIndex,
        .outputIndex = (u32)gtaoFilteredImage.storagePoolIndex,
        .width       = fullW,
        .height      = fullH,
        .nearZ       = camera->znear,
        .farZ        = camera->zfar,
    };
    vulkanPush(cmd, &spatialPipe, sizeof(spc), &spc);
    vulkanBeginProfile(cmd, &spatialPipe.profile, 0);
    vulkanDispatch(cmd, &spatialPipe, groupsX, groupsY, 1);
    vulkanEndProfile(cmd, &spatialPipe.profile, 0);

    u32 finalAoIndex = 0;

    /* ── 3. Temporal filter (half-res dispatch) ────────────────── */

    if (useTemporal) {
        ensureHistory(halfW, halfH);

        VulkanImage* history = &gtaoHistoryImages[gtaoHistoryIndex];
        VulkanImage* output  = &gtaoHistoryImages[1 - gtaoHistoryIndex];

        vkCmdPipelineBarrier2(cmd->cmd, &computeDepInfo);

        vulkanTransition(cmd, &gtaoFilteredImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, history, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, output, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

        vulkanBindPipe(cmd, &temporalPipe);

        float jitterDeltaX = jitterX - camera->cameraUbo.prevJitterX;
        float jitterDeltaY = jitterY - camera->cameraUbo.prevJitterY;

        GtaoTemporalPushConstants tpc = {
            .currentIndex  = (u32)gtaoFilteredImage.sampledPoolIndex,
            .historyIndex  = (u32)history->sampledPoolIndex,
            .velocityIndex = (u32)velocityImg->sampledPoolIndex,
            .depthIndex    = (u32)depthImg->sampledPoolIndex,
            .normalIndex   = (u32)normalImg->sampledPoolIndex,
            .outputIndex   = (u32)output->storagePoolIndex,
            .width         = fullW,
            .height        = fullH,
            .historyValid  = gtaoHistoryValid ? 1u : 0u,
            .jitterDeltaX  = jitterDeltaX,
            .jitterDeltaY  = jitterDeltaY,
        };
        vulkanPush(cmd, &temporalPipe, sizeof(tpc), &tpc);
        vulkanBeginProfile(cmd, &temporalPipe.profile, 0);
        vulkanDispatch(cmd, &temporalPipe, groupsX, groupsY, 1);
        vulkanEndProfile(cmd, &temporalPipe.profile, 0);

        vkCmdPipelineBarrier2(cmd->cmd, &computeDepInfo);
        vulkanTransition(cmd, output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

        finalAoIndex     = (u32)output->sampledPoolIndex;
        gtaoHistoryIndex = 1 - gtaoHistoryIndex;
        gtaoHistoryValid = 1;
    } else {
        vulkanTransition(cmd, &gtaoFilteredImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        finalAoIndex = (u32)gtaoFilteredImage.sampledPoolIndex;
    }

    /* Restore layouts expected by later passes. */
    vulkanTransition(cmd, depthImg, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, 1);
    vulkanTransition(cmd, normalImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    if (useTemporal) {
        vulkanTransition(cmd, velocityImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    }

    vulkanResourceSetAoImageIndex(finalAoIndex);
    float totalGpuTime =
        gtaoPipe.profile.elapsed + spatialPipe.profile.elapsed + temporalPipe.profile.elapsed;
    vulkanGtaoPass.gpuElapsed = totalGpuTime;
}

static void removed(void) {
    if (gtaoFilteredImage.img) {
        vulkanDestroyImage(&gtaoFilteredImage, NULL);
        gtaoFilteredImage = (VulkanImage){0};
    }
    if (gtaoRawImage.img) {
        vulkanDestroyImage(&gtaoRawImage, NULL);
        gtaoRawImage = (VulkanImage){0};
    }
    destroyHistory();

    vulkanDestroyPipe(&gtaoPipe);
    vulkanDestroyPipe(&spatialPipe);
    vulkanDestroyPipe(&temporalPipe);
}

/* ── Public API ──────────────────────────────────────────────────── */

void vulkanGtaoPassSetDisabled(char disabled) {
    gtaoDisabled = disabled;
    if (disabled) {
        gtaoHistoryValid = 0;
    }
    info("GTAO: %s", disabled ? "disabled" : "enabled");
}

char vulkanGtaoPassIsDisabled(void) {
    return gtaoDisabled;
}

void vulkanGtaoPassSetStrength(float strength) {
    gtaoStrength = strength;
    info("GTAO strength: %.2f", (double)strength);
}

float vulkanGtaoPassGetStrength(void) {
    return gtaoStrength;
}
