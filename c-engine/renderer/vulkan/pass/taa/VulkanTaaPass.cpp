#include "VulkanTaaPass.h"
#include "ecs/Ecs.h"
#include "ecs/system/window/WindowSystem.h"
#include "events/Events.h"
#include "renderer/Renderer.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/pass/azgaar_weather/VulkanAzgaarWeatherPass.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/pipeline/VulkanProfile.h"

static void added(void);
static void preUpdate(void);
static void update(void);
static void postUpdate(void);
static void removed(void);
static void swapchainCreated(void* _);
static void createAccumulators(void);
static void destroyAccumulators(void);
static float taaEnvWeight(void);
static float taaEnvGhost(void);
static float taaEnvDepth(void);

static double elapsedCPU;
static double elapsedGPU;
static VulkanImage taaA;
static VulkanImage taaB;
static u32 taaWidth;
static u32 taaHeight;
static int taaFrame = 0;
static VulkanImage* taaCurrentOutput = NULL;
static VulkanProfile profile;
static char profileReady;
static VulkanPipe taaPipe;
static char taaPipeReady;
static char taaWasEnabled;

System vulkanTaaPass = {
    .name       = "taa",
    .added      = added,
    .preUpdate  = preUpdate,
    .update     = update,
    .postUpdate = postUpdate,
    .removed    = removed,
};

static void added(void) {
    signalSubscribe("swapchainCreated", swapchainCreated);
    profile      = vulkanCreateProfile("taa");
    profileReady = 1;
    taaPipe      = vulkanCreatePipe(.name = "taa",
                                    .comp  = "shaders/pass/taa/spv/taa.comp.spv");
    taaPipeReady = 1;
}

static void preUpdate(void) {
    if (profileReady) {
        vulkanResetProfile(vulkan.currentCmd, &profile, 0);
    }
}

static void swapchainCreated(void* _) {
    (void)_;
    destroyAccumulators();
    createAccumulators();
    taaFrame = 0;
}

static void createAccumulators(void) {
    if (window.renderWidth <= 0 || window.renderHeight <= 0) {
        return;
    }
    taaA = vulkanCreateImage(.name   = "TaaA",
                              .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                              .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              .width  = window.renderWidth,
                              .height = window.renderHeight);
    taaB = vulkanCreateImage(.name   = "TaaB",
                              .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                              .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              .width  = window.renderWidth,
                              .height = window.renderHeight);
    taaWidth  = (u32)window.renderWidth;
    taaHeight = (u32)window.renderHeight;

    /* Clear both accumulators to black so the first frame's temporal blend
     * degrades gracefully to the current frame (prev luminance ~ 0 triggers
     * rejection). The accumulators start in UNDEFINED layout; transition them
     * to their first real layout (general — how the update path below stages
     * the write target before each dispatch) BEFORE clearing, so
     * vulkanClearColorImage' restore-back barrier targets a defined layout
     * (VUID 01198 forbids newLayout = UNDEFINED). They carry STORAGE|SAMPLED
     * usage, not COLOR_ATTACHMENT, so ATTACHMENT_OPTIMAL would be invalid. */
    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanTransition(cmd, &taaA, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    vulkanTransition(cmd, &taaB, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    VkClearColorValue black;
    black.float32[0] = 0.0f;
    black.float32[1] = 0.0f;
    black.float32[2] = 0.0f;
    black.float32[3] = 0.0f;
    vulkanClearColorImage(cmd, &taaA, black);
    vulkanClearColorImage(cmd, &taaB, black);
    vulkanTransientEnd(cmd, 1);
}

static void destroyAccumulators(void) {
    if (taaA.img) {
        vulkanDestroyImage(&taaA, NULL);
        taaA = (VulkanImage){0};
    }
    if (taaB.img) {
        vulkanDestroyImage(&taaB, NULL);
        taaB = (VulkanImage){0};
    }
    taaWidth  = 0;
    taaHeight = 0;
    taaCurrentOutput = NULL;
}

typedef struct TaaPushConstants {
    u32 colorIndex;      // current frame color (sampled, R16G16B16A16_SFLOAT)
    u32 velocityIndex;   // motion vectors (sampled, R16G16_SFLOAT)
    u32 depthIndex;      // depth buffer (sampled, D32_SFLOAT)
    u32 prevIndex;      // previous TAA accumulator (sampled)
    u32 outIndex;       // current TAA accumulator (storage)
    u32 width;
    u32 height;
    float blendWeight;    // max temporal blend (0.9)
    // Relative color-rejection threshold. With YCoCg neighborhood clamping
    // in the shader, this can be tighter than before — the clamp prevents
    // ghosting independently, so the rejection only needs to catch truly
    // stale history (different surface), not all high-contrast edges.
    // 1.0 tolerates the per-frame jitter pop on alpha-tested leaves
    // (relative diff ~0.8) while catching real content changes.
    float ghostThreshold; // relative color rejection threshold
    // Relative temporal depth-rejection threshold (inverse view-depth
    // space; equals the relative view-distance difference).  The shader
    // stores each frame's depth in the accumulator's alpha and compares
    // it against the current pixel's depth, so this only fires on real
    // disocclusions (leaf<->gap transitions, camera pans) and never on
    // the canopy's within-frame depth noise.
    float depthThreshold; // relative depth rejection threshold
    // Weather particle coverage masks (sampled, R8_UNORM).  0xFFFFFFFF
    // means "no mask" (weather pass has no render target yet) — the shader
    // then skips the coverage rejection entirely.  `maskCur` was written by
    // this frame's weather pass (particles at their CURRENT positions);
    // `maskPrev` by last frame's (particles at the positions the
    // reprojected history still holds them at).
    u32 maskCurIndex;
    u32 maskPrevIndex;
} TaaPushConstants;

static void update(void) {
    if (vulkan.skipFrame || !rendererIsTAAEnabled()) {
        /* TAA disabled: drop the output pointer so the final pass falls
         * back to the live scene/composite color, and remember the state
         * so re-enabling resets the stale history. */
        taaCurrentOutput = NULL;
        taaWasEnabled    = 0;
        return;
    }
    if (!taaWasEnabled) {
        /* Re-enabled: previous accumulators hold stale history; clear them
         * and restart the ping-pong so the first frame starts fresh. */
        taaWasEnabled = 1;
        destroyAccumulators();
        createAccumulators();
        taaFrame = 0;
    }
    if (!taaPipeReady) {
        return;
    }
    if (!taaA.img || !taaB.img) {
        createAccumulators();
        if (!taaA.img || !taaB.img) {
            return;
        }
    }
    if (taaWidth != (u32)window.renderWidth || taaHeight != (u32)window.renderHeight) {
        destroyAccumulators();
        createAccumulators();
        taaFrame = 0;
        if (!taaA.img || !taaB.img) {
            return;
        }
    }

    VulkanImage* sceneColor   = vulkanFrameResourcesGetSceneColor();
    VulkanImage* compositeColor = vulkanFrameResourcesGetCompositeColor();
    VulkanImage* color      = compositeColor ? compositeColor : sceneColor;
    VulkanImage* depth      = vulkanFrameResourcesGetDepth();
    VulkanImage* velocity   = vulkanFrameResourcesGetVelocity();
    if (!color || !depth || !velocity || window.renderWidth <= 0 || window.renderHeight <= 0) {
        return;
    }

    VulkanImage* prev = (taaFrame % 2) ? &taaA : &taaB;
    VulkanImage* out  = (taaFrame % 2) ? &taaB : &taaA;

    /* Weather particle coverage masks (this frame + last frame).  The
     * weather pass runs before this one, so the masks are ready. */
    VulkanImage* maskCur  = vulkanAzgaarWeatherPassGetMask();
    VulkanImage* maskPrev = vulkanAzgaarWeatherPassGetPrevMask();

    /* The current frame's sub-pixel jitter is read directly by the TAA
     * shader from the camera UBO (sceneBuffer.cameras[0].jitterX/Y), which
     * the camera system updates before the renderer runs.  No jitter offset
     * is applied to the unjittered history-accumulator sample. */
    VulkanCommand* cmd = vulkan.currentCmd;
    vulkanTransition(cmd, color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, velocity, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, prev, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, out, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    if (maskCur) {
        vulkanTransition(cmd, maskCur, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    }
    if (maskPrev) {
        vulkanTransition(cmd, maskPrev, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    }

    vulkanBindPipe(cmd, &taaPipe);
    TaaPushConstants pc = {
        .colorIndex     = (u32)color->sampledPoolIndex,
        .velocityIndex  = (u32)velocity->sampledPoolIndex,
        .depthIndex     = (u32)depth->sampledPoolIndex,
        .prevIndex     = (u32)prev->sampledPoolIndex,
        .outIndex      = (u32)out->storagePoolIndex,
        .width          = (u32)window.renderWidth,
        .height         = (u32)window.renderHeight,
        .blendWeight    = taaEnvWeight(),
        .ghostThreshold = taaEnvGhost(),
        .depthThreshold = taaEnvDepth(),
        .maskCurIndex   = maskCur ? (u32)maskCur->sampledPoolIndex : 0xFFFFFFFFu,
        .maskPrevIndex  = maskPrev ? (u32)maskPrev->sampledPoolIndex : 0xFFFFFFFFu,
    };
    vulkanPush(cmd, &taaPipe, sizeof(pc), &pc);

    u32 groupsX = ((u32)window.renderWidth + 7) / 8;
    u32 groupsY = ((u32)window.renderHeight + 7) / 8;
    vulkanBeginProfile(cmd, &profile, 0);
    vulkanDispatch(cmd, &taaPipe, groupsX, groupsY, 1);
    vulkanEndProfile(cmd, &profile, 0);

    vulkanTransition(cmd, out, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    taaCurrentOutput = out;
    taaFrame++;
    elapsedGPU = profile.elapsed;
}

static void postUpdate(void) {
    vulkanTaaPass.cpuElapsed = elapsedCPU;
    vulkanTaaPass.gpuElapsed = elapsedGPU;
    elapsedCPU               = nanos();
    elapsedCPU               = nanos() - elapsedCPU;
}

/* Debug overrides for TAA tuning (no recompile needed). */
/* TAA tuning: read from renderer AA settings; env vars override. */
static float taaEnvWeight(void) {
    float v = rendererGetAASettings().taaWeight;
    const char* env = getenv("ENGINE_TAA_WEIGHT");
    if (env && *env) v = (float)atof(env);
    return v;
}

static float taaEnvGhost(void) {
    float v = rendererGetAASettings().taaGhost;
    const char* env = getenv("ENGINE_TAA_GHOST");
    if (env && *env) v = (float)atof(env);
    return v;
}

static float taaEnvDepth(void) {
    float v = rendererGetAASettings().taaDepth;
    const char* env = getenv("ENGINE_TAA_DEPTH");
    if (env && *env) v = (float)atof(env);
    return v;
}

static void removed(void) {
    destroyAccumulators();
    if (profileReady) {
        vulkanDestroyProfile(&profile);
        profile      = (VulkanProfile){0};
        profileReady = 0;
    }
    if (taaPipeReady) {
        vulkanDestroyPipe(&taaPipe);
        taaPipe      = (VulkanPipe){0};
        taaPipeReady = 0;
    }
}

VulkanImage* vulkanTaaPassGetOutput(void) {
    /* Return the accumulator written by the most recently completed frame.
     * When TAA is disabled the accumulators are stale, so hand back NULL
     * and let the final pass fall back to the live scene color. */
    if (!rendererIsTAAEnabled()) {
        return NULL;
    }
    return taaCurrentOutput;
}

char vulkanTaaPassIsEnabled(void) {
    return rendererIsTAAEnabled() && taaA.img && taaB.img;
}
