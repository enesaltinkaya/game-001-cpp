#include "VulkanGiPass.h"
#include "events/Events.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/pipeline/VulkanProfile.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include <stdlib.h>

namespace engine {

    static void swapchainCreated(void* _);
    static void estimateDestroyOutput(void);
    static char estimateEnsureOutput(u32 sourceWidth, u32 sourceHeight);
    static void estimateDispatch(VulkanCommand* cmd, VulkanImage* depth, VulkanImage* normals,
                                 VulkanImage* albedo, VulkanImage* material);
    static void temporalDestroyAccumulators(void);
    static char temporalEnsureAccumulators(u32 width, u32 height);
    static void temporalDispatch(VulkanCommand* cmd, VulkanImage* depth, VulkanImage* velocity);
    static void publishToSceneBuffer(void);

    static double elapsedCPU;
    static double elapsedGPU;
    static char giDisabled;

    /* Half-res per-frame estimate (see gi_estimate.comp): rgb = cosine-
     * weighted irradiance, a = confidence.  Single slot, overwritten every
     * enabled frame; the temporal pass accumulates it into the full-res
     * ping-pong history below. */
    static VulkanPipe estimatePipe;
    static char estimatePipeReady;
    static VulkanImage giCurrent;
    static u32 giWidth;
    static u32 giHeight;

    /* Temporal accumulation of the estimate (see gi_temporal.comp) — the
     * AO-pass pattern: ping-pong history with reprojection, disocclusion
     * rejection and a wide neighborhood clamp.  The history carries rgb =
     * filtered irradiance + a = confidence (the consumer contract), while
     * the inverse view depth needed for the NEXT frame's disocclusion test
     * lives in a separate R16_SFLOAT ping-pong pair: the plan's D2/D4
     * history channel table lists five values (rgb, inv-depth, confidence)
     * for a four-channel format, so the depth is split out rather than
     * dropping the consumer's confidence gate (the units contract pins
     * gi.a as the mix factor).  R16F storage+sampled ping-pong is the
     * contact-shadow history's proven pattern.  Cleared to 0 = "no
     * history".  ENGINE_GI_TEMPORAL=0 disables the accumulation (the
     * output getter falls back to the raw estimate). */
    static VulkanPipe temporalPipe;
    static char temporalPipeReady;
    static char temporalEnabled = 1;
    static VulkanImage giHistoryA;
    static VulkanImage giHistoryB;
    static VulkanImage giDepthA;
    static VulkanImage giDepthB;
    static u32 giHistWidth;
    static u32 giHistHeight;
    static u32 giHistFrame;
    static VulkanImage* temporalOutput;
    static char giWasDisabled;

    /* Env-parsed knobs with fallback ("" falls back to the default too);
     re-read every frame like the AO knobs so A/B runs only need a
     restart. */
    static float giEnvFloat(const char* name, float def) {
        const char* env = getenv(name);
        return (env && *env) ? (float)atof(env) : def;
    }

    static u32 giEnvUint(const char* name, u32 def) {
        const char* env = getenv(name);
        return (env && *env) ? (u32)atoi(env) : def;
    }

    VulkanGiPass vulkanGiPass;

    VulkanGiPass::VulkanGiPass() : System("gi") {}

    typedef struct GiEstimatePushConstants {
        u32 depthIndex;   /* full-res depth (sampled, reversed-Z)          */
        u32 normalsIndex; /* oct-encoded world normals (sampled)           */
        u32 albedoIndex;  /* base albedo (sampled)                         */
        u32 materialIndex; /* roughness/metallic/alphaMask (sampled)       */
        u32 outputIndex;  /* half-res estimate (storage)                   */
        u32 sourceWidth;  /* full-res G-buffer size (depth/normals)        */
        u32 sourceHeight;
        u32 outputWidth;  /* half-res estimate size                        */
        u32 outputHeight;
        u32 rayCount;     /* hemisphere rays per texel                     */
        float distScale;    /* max-distance multiplier                       */
    } GiEstimatePushConstants;

    void VulkanGiPass::added() {
        const char* env = getenv("ENGINE_GI_DISABLED");
        if (env && *env && atoi(env)) giDisabled = 1;
        env = getenv("ENGINE_GI_TEMPORAL");
        if (env && *env && !atoi(env)) temporalEnabled = 0;

        utils::signalSubscribe("swapchainCreated", swapchainCreated);

        estimatePipe = vulkanCreatePipe(.name = "gi_estimate",
                                        .comp = "shaders/pass/gi/spv/gi_estimate.comp.spv");
        estimatePipeReady = 1;

        temporalPipe = vulkanCreatePipe(.name = "gi_temporal",
                                        .comp = "shaders/pass/gi/spv/gi_temporal.comp.spv");
        temporalPipeReady = 1;
    }

    void VulkanGiPass::preUpdate() {
        if (vulkan.skipFrame) {
            return;
        }
        if (estimatePipeReady) {
            vulkanResetProfile(vulkan.currentCmd, &estimatePipe.profile, 0);
        }
        if (temporalPipeReady) {
            vulkanResetProfile(vulkan.currentCmd, &temporalPipe.profile, 0);
        }
    }

    static void swapchainCreated(void* _) {
        (void)_;
        estimateDestroyOutput();
        temporalDestroyAccumulators();
    }

    static void estimateDestroyOutput(void) {
        if (giCurrent.img) {
            vulkanDestroyImage(&giCurrent, NULL);
            giCurrent = VulkanImage{};
        }
        giWidth  = 0;
        giHeight = 0;
    }

    static char estimateEnsureOutput(u32 sourceWidth, u32 sourceHeight) {
        /* Half internal resolution, rounded up so odd sizes keep every full-
         * res texel covered by the 2x2 origin blocks. */
        u32 width  = (sourceWidth + 1) / 2;
        u32 height = (sourceHeight + 1) / 2;

        if (giCurrent.img && giWidth == width && giHeight == height) {
            return 1;
        }

        estimateDestroyOutput();

        giCurrent =
            vulkanCreateImage(.name   = "GiCurrent",
                              .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                              .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              .width  = (int)width,
                              .height = (int)height);
        if (!giCurrent.img) {
            return 0;
        }
        giWidth  = width;
        giHeight = height;

        VulkanCommand* cmd = vulkanTransientBegin();
        vulkanTransition(cmd, &giCurrent, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        vulkanTransientEnd(cmd, 1);

        utils::info("vulkanGiPass: created %ux%u estimate (source %ux%u)",
                    width, height, sourceWidth, sourceHeight);
        return 1;
    }

    static void estimateDispatch(VulkanCommand* cmd, VulkanImage* depth, VulkanImage* normals,
                                 VulkanImage* albedo, VulkanImage* material) {
        if (!estimateEnsureOutput(depth->extent.width, depth->extent.height)) {
            return;
        }

        /* Depth/normals/material were left SHADER_READ_ONLY by the SSR
         * pass. */
        vulkanTransition(cmd, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, normals, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, albedo, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, material, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, &giCurrent, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

        u32 rayCount  = giEnvUint("ENGINE_GI_RAYS", 6);
        rayCount      = rayCount < 1 ? 1 : (rayCount > 16 ? 16 : rayCount);
        float distScale = giEnvFloat("ENGINE_GI_DIST_SCALE", 1.0f);
        if (distScale < 0.1f) distScale = 0.1f;
        if (distScale > 4.0f) distScale = 4.0f;

        GiEstimatePushConstants pc = {
            .depthIndex    = (u32)depth->sampledPoolIndex,
            .normalsIndex  = (u32)normals->sampledPoolIndex,
            .albedoIndex   = (u32)albedo->sampledPoolIndex,
            .materialIndex = (u32)material->sampledPoolIndex,
            .outputIndex   = (u32)giCurrent.storagePoolIndex,
            .sourceWidth   = depth->extent.width,
            .sourceHeight  = depth->extent.height,
            .outputWidth   = giWidth,
            .outputHeight  = giHeight,
            .rayCount      = rayCount,
            .distScale     = distScale,
        };

        vulkanBindPipe(cmd, &estimatePipe);
        vulkanPush(cmd, &estimatePipe, sizeof(pc), &pc);

        u32 groupsX = (giWidth + 7) / 8;
        u32 groupsY = (giHeight + 7) / 8;
        vulkanBeginProfile(cmd, &estimatePipe.profile, 0);
        vulkanDispatch(cmd, &estimatePipe, groupsX, groupsY, 1);
        vulkanEndProfile(cmd, &estimatePipe.profile, 0);

        vulkanTransition(cmd, &giCurrent, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    }

    /* ── Temporal GI accumulation ─────────────────────────────────────────── */

    typedef struct GiTemporalPushConstants {
        u32 giIndex;        /* current half-res estimate (sampled)         */
        u32 velocityIndex; /* motion vectors (sampled)                    */
        u32 depthIndex;    /* full-res depth buffer (sampled)             */
        u32 prevIndex;     /* previous history (sampled)                  */
        u32 prevDepthIndex; /* previous depth history (sampled)           */
        u32 outIndex;      /* current history (storage)                   */
        u32 outDepthIndex; /* current depth history (storage)             */
        u32 width;         /* full internal resolution                    */
        u32 height;
        float blendWeight;    /* max temporal blend                        */
        float depthThreshold; /* relative S-space rejection threshold      */
        float clampSlack;     /* AABB expansion (fraction of local range)  */
        float clampFloor;     /* AABB expansion (absolute floor)           */
        float devStart;       /* luminance deviation damping start         */
        float devEnd;         /* luminance deviation damping end           */
        float lumaClamp;      /* per-frame relative luma delta cap         */
    } GiTemporalPushConstants;

    static void temporalDestroyAccumulators(void) {
        if (giHistoryA.img) {
            vulkanDestroyImage(&giHistoryA, NULL);
            giHistoryA = VulkanImage{};
        }
        if (giHistoryB.img) {
            vulkanDestroyImage(&giHistoryB, NULL);
            giHistoryB = VulkanImage{};
        }
        if (giDepthA.img) {
            vulkanDestroyImage(&giDepthA, NULL);
            giDepthA = VulkanImage{};
        }
        if (giDepthB.img) {
            vulkanDestroyImage(&giDepthB, NULL);
            giDepthB = VulkanImage{};
        }
        giHistWidth    = 0;
        giHistHeight   = 0;
        giHistFrame    = 0;
        temporalOutput = NULL;
    }

    static char temporalEnsureAccumulators(u32 width, u32 height) {
        if (giHistoryA.img && giHistoryB.img && giDepthA.img && giDepthB.img &&
            giHistWidth == width && giHistHeight == height) {
            return 1;
        }

        temporalDestroyAccumulators();

        /* Cleared to 0 = "no history": the first frame's depth test rejects
         * it and starts accumulation cleanly (AO pattern). */
        giHistoryA =
            vulkanCreateImage(.name   = "GiHistoryA",
                              .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                              .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              .width  = (int)width,
                              .height = (int)height);
        giHistoryB =
            vulkanCreateImage(.name   = "GiHistoryB",
                              .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                              .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              .width  = (int)width,
                              .height = (int)height);
        /* Inverse view depth for the disocclusion test — same fp16 precision
         * the AO/TAA histories store it in (see gi_temporal.comp). */
        giDepthA =
            vulkanCreateImage(.name   = "GiDepthA",
                              .format = VK_FORMAT_R16_SFLOAT,
                              .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              .width  = (int)width,
                              .height  = (int)height);
        giDepthB =
            vulkanCreateImage(.name   = "GiDepthB",
                              .format = VK_FORMAT_R16_SFLOAT,
                              .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              .width  = (int)width,
                              .height  = (int)height);
        if (!giHistoryA.img || !giHistoryB.img || !giDepthA.img || !giDepthB.img) {
            temporalDestroyAccumulators();
            return 0;
        }
        giHistWidth  = width;
        giHistHeight = height;
        giHistFrame  = 0;

        VulkanCommand* cmd = vulkanTransientBegin();
        vulkanTransition(cmd, &giHistoryA, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        vulkanTransition(cmd, &giHistoryB, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        vulkanTransition(cmd, &giDepthA, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        vulkanTransition(cmd, &giDepthB, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        VkClearColorValue black = {};
        vulkanClearColorImage(cmd, &giHistoryA, black);
        vulkanClearColorImage(cmd, &giHistoryB, black);
        vulkanClearColorImage(cmd, &giDepthA, black);
        vulkanClearColorImage(cmd, &giDepthB, black);
        vulkanTransientEnd(cmd, 1);

        utils::info("vulkanGiPass: created %ux%u temporal history", width, height);
        return 1;
    }

    static void temporalDispatch(VulkanCommand* cmd, VulkanImage* depth, VulkanImage* velocity) {
        if (!temporalPipeReady || !temporalEnsureAccumulators(depth->extent.width,
                                                             depth->extent.height)) {
            return;
        }

        VulkanImage* prevHist  = (giHistFrame % 2) ? &giHistoryA : &giHistoryB;
        VulkanImage* outHist   = (giHistFrame % 2) ? &giHistoryB : &giHistoryA;
        VulkanImage* prevDepth = (giHistFrame % 2) ? &giDepthA : &giDepthB;
        VulkanImage* outDepth  = (giHistFrame % 2) ? &giDepthB : &giDepthA;

        /* estimateDispatch left depth + giCurrent in SHADER_READ_ONLY. */
        vulkanTransition(cmd, velocity, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, prevHist, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, prevDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, outHist, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
        vulkanTransition(cmd, outDepth, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

        GiTemporalPushConstants pc = {
            .giIndex        = (u32)giCurrent.sampledPoolIndex,
            .velocityIndex  = (u32)velocity->sampledPoolIndex,
            .depthIndex     = (u32)depth->sampledPoolIndex,
            .prevIndex      = (u32)prevHist->sampledPoolIndex,
            .prevDepthIndex = (u32)prevDepth->sampledPoolIndex,
            .outIndex       = (u32)outHist->storagePoolIndex,
            .outDepthIndex  = (u32)outDepth->storagePoolIndex,
            .width          = giHistWidth,
            .height         = giHistHeight,
            .blendWeight    = giEnvFloat("ENGINE_GI_TWEIGHT", 0.92f),
            .depthThreshold = giEnvFloat("ENGINE_GI_TDEPTH", 0.05f),
            .clampSlack     = giEnvFloat("ENGINE_GI_TCLAMP", 0.35f),
            .clampFloor     = giEnvFloat("ENGINE_GI_TFLOOR", 0.15f),
            .devStart       = giEnvFloat("ENGINE_GI_TDEV0", 0.12f),
            .devEnd         = giEnvFloat("ENGINE_GI_TDEV1", 0.50f),
            .lumaClamp      = giEnvFloat("ENGINE_GI_TLUMA", 0.15f),
        };

        vulkanBindPipe(cmd, &temporalPipe);
        vulkanPush(cmd, &temporalPipe, sizeof(pc), &pc);

        u32 groupsX = (giHistWidth + 7) / 8;
        u32 groupsY = (giHistHeight + 7) / 8;
        vulkanBeginProfile(cmd, &temporalPipe.profile, 0);
        vulkanDispatch(cmd, &temporalPipe, groupsX, groupsY, 1);
        vulkanEndProfile(cmd, &temporalPipe.profile, 0);

        /* The output stays readable for next frame's consumer binding
         * (one-frame latency — the write next frame goes to the other
         * slot, so the bound slot is never written while bound). */
        vulkanTransition(cmd, outHist, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, outDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        temporalOutput = outHist;
        giHistFrame++;
    }

    /* ── Consumer wiring (Phase 3, plans/ssgi.md D5/D6) ───────────────────── */

    /* Publish the GI texture index + intensity master into the scene
     * buffer BEFORE this frame's dispatches touch temporalOutput: the
     * scene (idx 10) and azgaar_props passes run EARLIER in the pass list
     * but their GPU work executes after the whole frame was recorded, so
     * they sample the value published here — the previous frame's
     * history.  NULL output (startup, resize, disabled) publishes the
     * 0xFFFFFFFFu absent-sentinel and consumers keep the plain IBL
     * ambient (vulkanResourceSetGiData patches only the current frame's
     * buffer, so in-flight frames keep their own slot). */
    static void publishToSceneBuffer(void) {
        VulkanImage* output = vulkanGiPassGetOutput();
        VulkanGiData gi     = {
            .giImageIndex = output ? (u32)output->sampledPoolIndex : 0xFFFFFFFFu,
            .giIntensity  = giEnvFloat("ENGINE_GI_INTENSITY", 1.0f),
        };
        vulkanResourceSetGiData(&gi);
    }

    /* AO: CACAO runs at its own strength (ENGINE_AO_STRENGTH / default) while
     * GI is on — the GI pass installs no runtime override.  The old D5
     * attenuation (ENGINE_GI_AO_SCALE, default 0.5) was measured a no-op on
     * the final image at the parked vantage (plans/ssgi-halo.md: AO field and
     * final image identical for 0.5 vs 1.0, ≤0.08/255, re-verified
     * 2026-09-01), so there is nothing to release on disable either. */

    void VulkanGiPass::update() {
        elapsedCPU = utils::nanos();

        if (vulkan.skipFrame) {
            elapsedCPU = utils::nanos() - elapsedCPU;
            return;
        }

        VulkanCommand* cmd    = vulkan.currentCmd;
        VulkanImage* depth    = vulkanFrameResourcesGetDepth();
        VulkanImage* normals  = vulkanFrameResourcesGetNormals();
        VulkanImage* albedo   = vulkanFrameResourcesGetAlbedo();
        VulkanImage* material = vulkanFrameResourcesGetMaterial();

        if (giWasDisabled && !giDisabled) {
            /* Re-enabled after a disable: history is stale — reset it BEFORE
             * this frame's accumulation runs. */
            temporalDestroyAccumulators();
        }
        giWasDisabled = giDisabled;

        publishToSceneBuffer();

        /* While disabled nothing is dispatched and no image is created
         * (the buffers are created lazily below) — frame cost
         * returns to the no-GI baseline. */
        if (!giDisabled && depth && normals && albedo && material) {
            estimateDispatch(cmd, depth, normals, albedo, material);
            /* Accumulate the per-frame MC noise (per-texel/per-frame ray
             * hashing) into the history so consumers get a stable signal —
             * the color TAA pass alone passes it through (see
             * gi_temporal.comp). */
            if (temporalEnabled && giCurrent.img) {
                VulkanImage* velocity = vulkanFrameResourcesGetVelocity();
                if (velocity) {
                    temporalDispatch(cmd, depth, velocity);
                }
            }
        }

        elapsedGPU = 0.0;
        if (estimatePipeReady) {
            elapsedGPU += estimatePipe.profile.elapsed;
        }
        if (temporalPipeReady) {
            elapsedGPU += temporalPipe.profile.elapsed;
        }
        elapsedCPU = utils::nanos() - elapsedCPU;
    }

    void VulkanGiPass::postUpdate() {
        vulkanGiPass.cpuElapsed = elapsedCPU;
        vulkanGiPass.gpuElapsed = elapsedGPU;
    }

    void VulkanGiPass::removed() {
        temporalDestroyAccumulators();
        estimateDestroyOutput();
        if (estimatePipeReady) {
            vulkanDestroyPipe(&estimatePipe);
            estimatePipe      = VulkanPipe{};
            estimatePipeReady = 0;
        }
        if (temporalPipeReady) {
            vulkanDestroyPipe(&temporalPipe);
            temporalPipe      = VulkanPipe{};
            temporalPipeReady = 0;
        }
    }

    void vulkanGiPassSetDisabled(char disabled) {
        giDisabled = disabled;
        utils::info("GI: %s", giDisabled ? "disabled" : "enabled");
    }

    char vulkanGiPassIsDisabled(void) {
        return giDisabled;
    }

    VulkanImage* vulkanGiPassGetOutput(void) {
        /* Temporally accumulated history when available (rgb = filtered
         * irradiance, a = confidence); the raw half-res estimate when the
         * temporal filter is disabled (ENGINE_GI_TEMPORAL=0).  NULL until
         * the first enabled frame after swapchain creation and while
         * disabled (a runtime disable must stop consumers reading the
         * stale history — the sentinel path reverts them to IBL). */
        if (giDisabled) {
            return NULL;
        }
        if (temporalEnabled && temporalOutput) {
            return temporalOutput;
        }
        return giCurrent.img ? &giCurrent : NULL;
    }

    VulkanImage* vulkanGiPassGetEstimate(void) {
        /* Raw per-frame half-res estimate — the ray-debugging view (the
         * giEstimate dump token), independent of the temporal filter. */
        return giCurrent.img ? &giCurrent : NULL;
    }
}  // namespace engine
