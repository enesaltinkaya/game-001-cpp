#include "VulkanContactShadowPass.h"
#include "BendDispatchList.h"
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
#include "renderer/vulkan/resources/VulkanIbl.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"

static void added(void);
static void preUpdate(void);
static void update(void);
static void removed(void);

System vulkanContactShadowPass = {
    .name      = "contact_shadow",
    .added     = added,
    .preUpdate = preUpdate,
    .update    = update,
    .removed   = removed,
};

/* ── Push constants (must match contact_shadow.comp) ───────────── */

typedef struct BendShadowPushConstants {
    vec4 lightCoordinate;
    ivec2 waveOffset;
    vec2 invDepthTextureSize;
    vec2 depthBounds;

    float surfaceThickness;
    float bilinearThreshold;
    float shadowContrast;
    float farDepthValue;
    float nearDepthValue;

    int ignoreEdgePixels;
    int usePrecisionOffset;
    int bilinearSamplingOffsetMode;
    int useEarlyOut;
    int debugOutputEdgeMask;
    int debugOutputThreadIndex;
    int debugOutputWaveIndex;

    u32 depthImageIndex;
    u32 normalImageIndex;
    u32 outputImageIndex;
} BendShadowPushConstants;

typedef struct ContactShadowSpatialPushConstants {
    u32 inputIndex;
    u32 depthIndex;
    u32 normalIndex;
    u32 outputIndex;
    u32 width;
    u32 height;
    float nearZ;
    float farZ;
} ContactShadowSpatialPushConstants;

typedef struct ContactShadowTemporalPushConstants {
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
} ContactShadowTemporalPushConstants;

/* ── State ───────────────────────────────────────────────────────── */

static VulkanPipe contactShadowPipe;
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
static float csRayLength = 0.5f;
static float csThickness = 0.005f;

/* ── Helpers ─────────────────────────────────────────────────────── */

static void clearOutput(void) {
    historyValid = 0;
    vulkanResourceSetContactShadowImageIndex(0);
    vulkanContactShadowPass.gpuElapsed = 0;
}

static void destroyHistory(void) {
    for (int i = 0; i < 2; ++i) {
        if (historyImages[i].img) {
            vulkanDestroyImage(&historyImages[i], NULL);
            historyImages[i] = (VulkanImage){0};
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
        snprintf(nameBuf, sizeof(nameBuf), "ContactShadowHistory%d", i);
        historyImages[i] =
            vulkanCreateImage(.name   = nameBuf,
                              .format = VK_FORMAT_R16_SFLOAT,
                              .usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                              .width  = (int)w,
                              .height = (int)h);
    }
}

static void swapchainCreated(void*) {
    if (filteredImage.img) {
        vulkanDestroyImage(&filteredImage, NULL);
        filteredImage = (VulkanImage){0};
    }
    if (rawImage.img) {
        vulkanDestroyImage(&rawImage, NULL);
        rawImage = (VulkanImage){0};
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

    rawImage = vulkanCreateImage(.name   = "contact_shadow_raw",
                                 .format = VK_FORMAT_R16_SFLOAT,
                                 .usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                 .width  = window.renderWidth,
                                 .height = window.renderHeight);

    filteredImage =
        vulkanCreateImage(.name   = "contact_shadow_filtered",
                          .format = VK_FORMAT_R16_SFLOAT,
                          .usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          .width  = window.renderWidth,
                          .height = window.renderHeight);
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

    contactShadowPipe =
        vulkanCreatePipe(.name = "contact_shadow",
                         .comp = "shaders/pass/contact_shadow/spv/contact_shadow.comp.spv");
    spatialPipe =
        vulkanCreatePipe(.name = "contact_shadow_spatial",
                         .comp = "shaders/pass/contact_shadow/spv/contact_shadow_spatial.comp.spv");
    temporalPipe =
        vulkanCreatePipe(.name = "contact_shadow_temporal",
                         .comp =
                             "shaders/pass/contact_shadow/spv/contact_shadow_temporal.comp.spv");

    const char* env = getenv("ENGINE_CONTACT_SHADOW_DISABLED");
    if (env && *env && atoi(env)) {
        csDisabled = 1;
    }
}

static void preUpdate(void) {
    vulkanResetProfile(vulkan.currentCmd, &contactShadowPipe.profile, 0);
}

static void update(void) {
    if (vulkan.skipFrame) {
        return;
    }

    if (csDisabled) {
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
    if (!rawImage.img || !filteredImage.img) {
        clearOutput();
        return;
    }

    const u32 w       = depthImg->extent.width;
    const u32 h       = depthImg->extent.height;
    const u32 groupsX = (w + 7u) / 8u;
    const u32 groupsY = (h + 7u) / 8u;

    char temporalAAActive    = rendererGetUpscalerMode() != RENDERER_UPSCALER_OFF;
    VulkanImage* velocityImg = vulkanFrameResourcesGetVelocity();

    /* Temporal accumulation is essential to smooth out sub-pixel jitter
     * variation when FSR is active.  Without it, the contact shadow
     * edges shift every frame with the jittered depth buffer, causing
     * visible flickering that FSR's own temporal accumulator cannot fix
     * (it doesn't know about the contact shadow effect).
     *
     * The temporal shader uses a conservative blend (10% current for
     * static pixels) and variance clipping, so the double accumulation
     * (our temporal + FSR's temporal) is harmless in practice.           */
    char useTemporal = velocityImg && velocityImg->img;

    if (!useTemporal) {
        historyValid = 0;
    }

    float jitterX = camera->cameraUbo.jitterX;
    float jitterY = camera->cameraUbo.jitterY;

    VulkanCommand* cmd = vulkan.currentCmd;

    /* ── 1. Bend Studio ray march via dispatch list ────────────── */

    vulkanTransition(cmd, depthImg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, normalImg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    if (useTemporal) {
        vulkanTransition(cmd, velocityImg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    }
    vulkanTransition(cmd, &rawImage, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

    vulkanBeginProfile(cmd, &contactShadowPipe.profile, 0);
    vulkanBindPipe(cmd, &contactShadowPipe);

    /* Project directional light direction into clip space.
     * For a directional light, w=0 so the projected point goes to infinity
     * and rays become highly parallel - exactly what Bend describes. */
    vec4 lightClip;
    {
        /* IblSunLight.direction points TOWARD the sun — that's exactly
         * the direction we want the shadow rays to march. */
        IblSunLight iblSun = vulkanIblGetExtractedSun();
        vec3 toLight;
        glm_vec3_copy(iblSun.direction, toLight);
        glm_vec3_normalize(toLight);

        /* Fallback when sun extraction gave us zero */
        if (glm_vec3_norm(toLight) < 0.001f) {
            toLight[0] = 0.577f;
            toLight[1] = 0.577f;
            toLight[2] = 0.577f;
        }

        vec4 toLightH = {toLight[0], toLight[1], toLight[2], 0.0f};
        /* Use the jittered viewProjection so the projected light
         * direction lives in the same coordinate system as the depth
         * buffer (which is rasterised with the jittered projection).
         * Using viewProjectionNoJitter caused a per-frame misalignment
         * that made the ray-march miss thin alpha-cutout features
         * (grass, foliage) differently each frame → flickering. */
        glm_mat4_mulv(camera->cameraUbo.viewProjection, toLightH, lightClip);
    }

    /* Build the dispatch list. */
    int vpSize[2]    = {(int)w, (int)h};
    int minBounds[2] = {0, 0};
    int maxBounds[2] = {(int)w, (int)h};

    BendDispatchList dispatchList =
        bendBuildDispatchList(lightClip,
                              vpSize,
                              minBounds,
                              maxBounds,
                              false, /* not expanded Z range (Vulkan 0..1) */
                              64     /* wave size */
        );

    /* Prepare push constants (fields that are the same for all dispatches). */
    BendShadowPushConstants pc = {};
    memcpy(pc.lightCoordinate, dispatchList.LightCoordinate_Shader, sizeof(vec4));

    pc.invDepthTextureSize[0] = 1.0f / (float)w;
    pc.invDepthTextureSize[1] = 1.0f / (float)h;
    pc.depthBounds[0]         = 0.0f;
    pc.depthBounds[1]         = 1.0f;

    pc.nearDepthValue = 1.0f; /* reverse-Z near */
    pc.farDepthValue  = 0.0f; /* reverse-Z far  */

    pc.surfaceThickness           = csThickness;
    pc.bilinearThreshold          = 0.02f;
    pc.shadowContrast             = 2.0f;
    pc.ignoreEdgePixels           = 0;
    pc.usePrecisionOffset         = 0;
    pc.bilinearSamplingOffsetMode = 0;
    pc.useEarlyOut                = 0;
    pc.debugOutputEdgeMask        = 0;
    pc.debugOutputThreadIndex     = 0;
    pc.debugOutputWaveIndex       = 0;

    pc.depthImageIndex  = (u32)depthImg->sampledPoolIndex;
    pc.normalImageIndex = (u32)normalImg->sampledPoolIndex;
    pc.outputImageIndex = (u32)rawImage.storagePoolIndex;

    /* Issue one dispatch per entry in the list. */
    for (int i = 0; i < dispatchList.DispatchCount; i++) {
        BendDispatchData* d = &dispatchList.Dispatch[i];
        pc.waveOffset[0]    = d->WaveOffset_Shader[0];
        pc.waveOffset[1]    = d->WaveOffset_Shader[1];

        vulkanPush(cmd, &contactShadowPipe, sizeof(pc), &pc);
        vulkanDispatch(cmd, &contactShadowPipe, d->WaveCount[0], d->WaveCount[1], d->WaveCount[2]);
    }

    vulkanEndProfile(cmd, &contactShadowPipe.profile, 0);

    vkCmdPipelineBarrier2(cmd->cmd, &computeDepInfo);

    /* ── 2. Spatial filter ─────────────────────────────────────── */

    vulkanTransition(cmd, &rawImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, &filteredImage, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

    vulkanBindPipe(cmd, &spatialPipe);

    ContactShadowSpatialPushConstants spc = {
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

    /* ── 3. Temporal filter (FSR / Native AA only) ─────────────── */

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

        ContactShadowTemporalPushConstants tpc = {
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
    if (useTemporal) {
        vulkanTransition(cmd, velocityImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    }

    vulkanResourceSetContactShadowImageIndex(finalIndex);
    vulkanContactShadowPass.gpuElapsed = contactShadowPipe.profile.elapsed;
}

static void removed(void) {
    if (filteredImage.img) {
        vulkanDestroyImage(&filteredImage, NULL);
        filteredImage = (VulkanImage){0};
    }
    if (rawImage.img) {
        vulkanDestroyImage(&rawImage, NULL);
        rawImage = (VulkanImage){0};
    }
    destroyHistory();

    vulkanDestroyPipe(&contactShadowPipe);
    vulkanDestroyPipe(&spatialPipe);
    vulkanDestroyPipe(&temporalPipe);
}

/* ── Public API ──────────────────────────────────────────────────── */

void vulkanContactShadowPassSetDisabled(char disabled) {
    csDisabled = disabled;
    if (disabled) {
        historyValid = 0;
    }
    info("Contact shadows: %s", disabled ? "disabled" : "enabled");
}

char vulkanContactShadowPassIsDisabled(void) {
    return csDisabled;
}

void vulkanContactShadowPassSetLength(float length) {
    csRayLength = length;
    info("Contact shadow ray length: %.2f", (double)length);
}

float vulkanContactShadowPassGetLength(void) {
    return csRayLength;
}

void vulkanContactShadowPassSetThickness(float thickness) {
    csThickness = thickness;
    info("Contact shadow thickness: %.4f", (double)thickness);
}

float vulkanContactShadowPassGetThickness(void) {
    return csThickness;
}
