#include "VulkanVolumetricPass.h"
#include "VulkanVolumetricPass.h"
#include "ecs/Ecs.h"
#include "ecs/system/System.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "events/Events.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/utils/VulkanBlur.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"

namespace engine {
static void destroyHistory(void);
static void ensureHistory(u32 w, u32 h);

static double elapsedCPU;
static double elapsedGPU;
static char   volumetricDisabled;

VulkanVolumetricPass vulkanVolumetricPass;

VulkanVolumetricPass::VulkanVolumetricPass() : System("volumetric") {}

static VulkanPipe lightSourcePipe;
static VulkanPipe lightShaftsPipe;
static VulkanPipe temporalPipe;
static VulkanImage lightSource;
static VulkanImage lightShafts;
static VulkanImage blurTemp;
static VulkanImage historyImages[2];
static VulkanImage* volumetricOutput;
static int historyIndex;
static u32 historyWidth;
static u32 historyHeight;
static char historyValid;

/* The volumetric effect renders at half resolution: light shafts are a
 * low-frequency screen-space effect that is blurred and temporally
 * accumulated, then upscaled by the linear sampler in the composite pass.
 * `width`/`height` are the output (half-res) dimensions; `fullWidth`/`fullHeight`
 * are the full-res dimensions of the shared depth/velocity/normal images. */
typedef struct LightSourcePC {
    u32 sceneColorIndex;
    u32 depthIndex;
    u32 outputIndex;
    u32 width;
    u32 height;
    u32 fullWidth;
    u32 fullHeight;
} LightSourcePC;

typedef struct LightShaftsPC {
    u32 depthIndex;
    u32 lightSourceIndex;
    u32 outputIndex;
    u32 width;
    u32 height;
    u32 fullWidth;
    u32 fullHeight;
} LightShaftsPC;

typedef struct LightShaftsTemporalPC {
    u32 currentIndex;
    u32 historyIndex;
    u32 velocityIndex;
    u32 depthIndex;
    u32 normalIndex;
    u32 outputIndex;
    u32 width;
    u32 height;
    u32 fullWidth;
    u32 fullHeight;
    u32 historyValid;
    float jitterDeltaX;
    float jitterDeltaY;
} LightShaftsTemporalPC;

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
        snprintf(nameBuf, sizeof(nameBuf), "VolumetricShaftsHistory%d", i);
        historyImages[i] = vulkanCreateImage(
            .name   = nameBuf,
            .format = VK_FORMAT_R16G16B16A16_SFLOAT,
            .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            .width  = (int)w,
            .height = (int)h);
    }
}

static void destroyImages(void) {
    volumetricOutput = NULL;
    destroyHistory();

    if (lightSource.img) {
        vulkanDestroyImage(&lightSource, NULL);
        lightSource = VulkanImage{};
    }
    if (lightShafts.img) {
        vulkanDestroyImage(&lightShafts, NULL);
        lightShafts = VulkanImage{};
    }
    if (blurTemp.img) {
        vulkanDestroyImage(&blurTemp, NULL);
        blurTemp = VulkanImage{};
    }
}

static void createImages(u32 w, u32 h) {
    lightSource = vulkanCreateImage(
        .name   = "VolumetricLightSource",
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        .width  = (int)w,
        .height = (int)h);

    lightShafts = vulkanCreateImage(
        .name   = "VolumetricShafts",
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .width  = (int)w,
        .height = (int)h,
        .noPool = 0);

    blurTemp = vulkanCreateImage(
        .name   = "VolumetricBlurTemp",
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        .width  = (int)w,
        .height = (int)h);
}

static void clearShafts(VulkanCommand* cmd) {
    historyValid      = 0;
    volumetricOutput = NULL;

    if (!lightShafts.img) return;

    vulkanTransition(cmd, &lightShafts, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);

    VkClearColorValue clear = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkImageSubresourceRange range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    vkCmdClearColorImage(cmd->cmd, lightShafts.img,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

    vulkanTransition(cmd, &lightShafts, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
}

static void swapchainCreated(void*) {
    destroyImages();
}

void VulkanVolumetricPass::added() {
    utils::signalSubscribe("swapchainCreated", swapchainCreated);

    lightSourcePipe = vulkanCreatePipe(
        .name = "volumetric_light_source",
        .comp = "shaders/pass/volumetric/spv/light_source.comp.spv");

    lightShaftsPipe = vulkanCreatePipe(
        .name = "volumetric_light_shafts",
        .comp = "shaders/pass/volumetric/spv/light_shafts.comp.spv");

    temporalPipe = vulkanCreatePipe(
        .name = "volumetric_light_shafts_temporal",
        .comp = "shaders/pass/volumetric/spv/light_shafts_temporal.comp.spv");
}

void VulkanVolumetricPass::preUpdate() {
    if (vulkan.skipFrame) return;

    VulkanImage* depth = vulkanFrameResourcesGetDepth();
    if (!depth) return;

    if (!lightSource.img) {
        createImages((depth->extent.width + 1) / 2, (depth->extent.height + 1) / 2);
    }

    vulkanResetProfile(vulkan.currentCmd, &lightSourcePipe.profile, 0);
    vulkanResetProfile(vulkan.currentCmd, &lightShaftsPipe.profile, 0);
    vulkanResetProfile(vulkan.currentCmd, &temporalPipe.profile, 0);
}

void VulkanVolumetricPass::update() {
    elapsedCPU = utils::nanos();
    elapsedGPU = 0.0;

    if (vulkan.skipFrame) {
        elapsedCPU = utils::nanos() - elapsedCPU;
        return;
    }

    VulkanCommand* cmd = vulkan.currentCmd;
    VulkanImage* sceneColor = vulkanFrameResourcesGetSceneColor();
    VulkanImage* depth = vulkanFrameResourcesGetDepth();

    if (!sceneColor || !depth || !lightShafts.img) {
        volumetricOutput = NULL;
        elapsedCPU = utils::nanos() - elapsedCPU;
        return;
    }

    if (volumetricDisabled) {
        clearShafts(cmd);
        elapsedCPU = utils::nanos() - elapsedCPU;
        return;
    }

    struct Entity* camEntity = cameraGetEntity();
    Camera* camera = NULL;
    if (camEntity) {
        camera = getComponent(camEntity->scene, Camera, camEntity->id);
    }

    VulkanImage* velocity = vulkanFrameResourcesGetVelocity();
    VulkanImage* normal   = vulkanFrameResourcesGetViewNormal();
    char useTemporal = camera && velocity && velocity->img && normal && normal->img;
    if (!useTemporal) {
        historyValid = 0;
    }

    /* Ensure inputs are readable */
    vulkanTransition(cmd, sceneColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    u32 w = lightShafts.extent.width;
    u32 h = lightShafts.extent.height;
    u32 gx = (w + 7) / 8;
    u32 gy = (h + 7) / 8;

    /* ── 1. Light Source ──────────────────────────────────────────── */
    vulkanTransition(cmd, &lightSource, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

    vulkanBeginProfile(cmd, &lightSourcePipe.profile, 0);
    vulkanBindPipe(cmd, &lightSourcePipe);

    LightSourcePC pc = {
        .sceneColorIndex = (u32)sceneColor->sampledPoolIndex,
        .depthIndex      = (u32)depth->sampledPoolIndex,
        .outputIndex     = (u32)lightSource.storagePoolIndex,
        .width           = w,
        .height          = h,
        .fullWidth       = (u32)depth->extent.width,
        .fullHeight      = (u32)depth->extent.height,
    };
    vulkanPush(cmd, &lightSourcePipe, sizeof(pc), &pc);
    vulkanDispatch(cmd, &lightSourcePipe, gx, gy, 1);
    vulkanEndProfile(cmd, &lightSourcePipe.profile, 0);

    vulkanTransition(cmd, &lightSource, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    /* ── 2. Light Shafts ──────────────────────────────────────────── */
    vulkanTransition(cmd, &lightShafts, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

    vulkanBeginProfile(cmd, &lightShaftsPipe.profile, 0);
    vulkanBindPipe(cmd, &lightShaftsPipe);

    LightShaftsPC pc2 = {
        .depthIndex         = (u32)depth->sampledPoolIndex,
        .lightSourceIndex   = (u32)lightSource.sampledPoolIndex,
        .outputIndex        = (u32)lightShafts.storagePoolIndex,
        .width              = w,
        .height             = h,
        .fullWidth          = (u32)depth->extent.width,
        .fullHeight         = (u32)depth->extent.height,
    };
    vulkanPush(cmd, &lightShaftsPipe, sizeof(pc2), &pc2);
    vulkanDispatch(cmd, &lightShaftsPipe, gx, gy, 1);
    vulkanEndProfile(cmd, &lightShaftsPipe.profile, 0);

    /* ── 3. Blur ──────────────────────────────────────────────────── */
    vulkanBlur(cmd, &lightShafts, &blurTemp);

    /* ── 4. Temporal accumulation ─────────────────────────────────── */
    volumetricOutput = &lightShafts;

    if (useTemporal) {
        ensureHistory(w, h);

        VulkanImage* history = &historyImages[historyIndex];
        VulkanImage* output  = &historyImages[1 - historyIndex];

        if (history->img && output->img) {
            vulkanTransition(cmd, &lightShafts, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
            vulkanTransition(cmd, history, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
            vulkanTransition(cmd, velocity, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
            vulkanTransition(cmd, normal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
            vulkanTransition(cmd, output, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

            vulkanBeginProfile(cmd, &temporalPipe.profile, 0);
            vulkanBindPipe(cmd, &temporalPipe);

            LightShaftsTemporalPC tpc = {
                .currentIndex  = (u32)lightShafts.sampledPoolIndex,
                .historyIndex  = (u32)history->sampledPoolIndex,
                .velocityIndex = (u32)velocity->sampledPoolIndex,
                .depthIndex    = (u32)depth->sampledPoolIndex,
                .normalIndex   = (u32)normal->sampledPoolIndex,
                .outputIndex   = (u32)output->storagePoolIndex,
                .width         = w,
                .height        = h,
                .fullWidth     = (u32)depth->extent.width,
                .fullHeight    = (u32)depth->extent.height,
                .historyValid  = historyValid ? 1u : 0u,
                .jitterDeltaX  = camera->cameraUbo.jitterX - camera->cameraUbo.prevJitterX,
                .jitterDeltaY  = camera->cameraUbo.jitterY - camera->cameraUbo.prevJitterY,
            };
            vulkanPush(cmd, &temporalPipe, sizeof(tpc), &tpc);
            vulkanDispatch(cmd, &temporalPipe, gx, gy, 1);
            vulkanEndProfile(cmd, &temporalPipe.profile, 0);

            vulkanTransition(cmd, output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

            volumetricOutput = output;
            historyIndex = 1 - historyIndex;
            historyValid = 1;
        } else {
            historyValid = 0;
            vulkanTransition(cmd, &lightShafts, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        }
    } else {
        vulkanTransition(cmd, &lightShafts, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    }

    elapsedGPU = lightSourcePipe.profile.elapsed + lightShaftsPipe.profile.elapsed +
                 temporalPipe.profile.elapsed;
    elapsedCPU = utils::nanos() - elapsedCPU;
}

void VulkanVolumetricPass::postUpdate() {
    vulkanVolumetricPass.cpuElapsed = elapsedCPU;
    vulkanVolumetricPass.gpuElapsed = elapsedGPU;
}

void VulkanVolumetricPass::removed() {
    destroyImages();
    vulkanDestroyPipe(&lightSourcePipe);
    vulkanDestroyPipe(&lightShaftsPipe);
    vulkanDestroyPipe(&temporalPipe);
}

void vulkanVolumetricPassSetDisabled(char disabled) {
    volumetricDisabled = disabled;
    if (disabled) {
        historyValid      = 0;
        volumetricOutput = NULL;
    }
    utils::info("Volumetric fog: %s", volumetricDisabled ? "disabled" : "enabled");
}

char vulkanVolumetricPassIsDisabled(void) {
    return volumetricDisabled;
}

VulkanImage* vulkanVolumetricPassGetOutput(void) {
    if (volumetricDisabled) return NULL;
    if (volumetricOutput && volumetricOutput->img) return volumetricOutput;
    if (lightShafts.img) return &lightShafts;
    return NULL;
}
}  // namespace engine
