#include "renderer/vulkan/pass/azgaar_weather/VulkanAzgaarWeatherPass.h"
#include "renderer/Renderer.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/pipeline/VulkanProfile.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/resources/VulkanBuffer.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/window/WindowSystem.h"
#include "thread/Thread.h"
#include <memory>
#include <stdlib.h>

namespace engine {

VulkanAzgaarWeatherPass vulkanAzgaarWeatherPass;

VulkanAzgaarWeatherPass::VulkanAzgaarWeatherPass() : System("azgaar_weather") {}

// Particle pool.  65 536 × 16 B = 1 MB of device-local memory; the dispatch
// is 1024 work groups of 64 (≈ 0.1 ms) and the draw is one instanced call.
#define MAX_WEATHER_PARTICLES 65536u

// Per-particle metadata packed into posSeed.w — must match the shaders.
#define W_META_TYPE_MASK     0xFFu
#define W_META_SEED_SHIFT    8u
#define W_META_SEED_MASK     0x7FFFFFu
#define W_META_DISABLED_BIT  0x80000000u
#define W_TYPE_SNOW          0u
#define W_TYPE_RAIN          1u
#define W_TYPE_DUST          2u
#define W_TYPE_LEAVES        3u

// Must match the GLSL push-constant block in weather_update.comp.
struct WeatherSimPushConstants {
    u64 particleAddress = 0;  // GpuWeatherParticle buffer device address
    u32 depthIndex      = 0;  // scene depth image in the global sampled pool
    u32 maxParticles    = 0;
    u32 _pad            = 0;
};

// Must match the GLSL push-constant block in azgaar_weather.vert/.frag.
typedef struct WeatherDrawPushConstants {
    u32 depthIndex;
    u32 width;
    u32 height;
    float nearZ;
    float farZ;
    float projM00;
    float projM11;
} WeatherDrawPushConstants;

static VulkanPipe simPipe;   // weather_update.comp
static VulkanPipe drawPipe;  // instanced billboards / streaks
static VulkanPipe maskPipe;  // TAA ghost-rejection coverage mask
static VulkanImage maskA;    // ping-pong coverage masks (R8, render res)
static VulkanImage maskB;
static int maskFrame = 0;
static VulkanBuffer particleBuffer;
static u32 particleCount = MAX_WEATHER_PARTICLES;

// Pending re-seed (set on game thread, consumed on render thread).
static utils::Thread uploadLock = {.mutex = PTHREAD_MUTEX_INITIALIZER};
static bool pendingReseed = false;
static VulkanWeatherData pendingReseedWeather;

// Instance-rate vertex input: the whole particle (posSeed) is one vec4 per
// instance; quad corners come from gl_VertexIndex (no vertex-rate binding).
static VkVertexInputBindingDescription vertexBinding = {
    .binding   = 0,
    .stride    = 16,
    .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE,
};
static VkVertexInputAttributeDescription vertexAttrs[] = {
    {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0},
};

static void recreatePipelines(void) {
    if (simPipe.pipe) vulkanDestroyPipe(&simPipe);
    if (drawPipe.pipe) vulkanDestroyPipe(&drawPipe);
    if (maskPipe.pipe) vulkanDestroyPipe(&maskPipe);

    simPipe = vulkanCreatePipe(
        .name = "weather_update",
        .comp = "shaders/pass/azgaar_weather/spv/weather_update.comp.spv");

    // Colour-only transparent recipe (the water pass verbatim): the scene
    // depth buffer is NOT an attachment — the fragment shader samples it for
    // the soft-particle fade / occlusion, so no depth-test attachment and no
    // layout round-trip is needed.  Premultiplied-style SRC_ALPHA blend,
    // depth write implicitly off.
    drawPipe = vulkanCreatePipe(
        .name                 = "azgaar_weather",
        .vs                   = "shaders/pass/azgaar_weather/spv/azgaar_weather.vert.spv",
        .fs                   = "shaders/pass/azgaar_weather/spv/azgaar_weather.frag.spv",
        .colorFormat1         = VK_FORMAT_R16G16B16A16_SFLOAT,
        .noCull               = 1,
        .blend                = 1,
        .clearColor1          = {0, 0, 0, 0}, .clearColor1Enabled = 0,
        .clearDepth           = {0, 0}, .clearDepthEnabled = 0,
        .vertexAttributes     = vertexAttrs,
        .vertexAttributeCount = 1,
        .vertexBindings       = &vertexBinding,
        .vertexBindingCount   = 1);

    // Coverage mask: same instanced expansion, no blend, cleared R8 out.
    // Runs after the sim dispatch so it uses this frame's positions.
    maskPipe = vulkanCreatePipe(
        .name                 = "azgaar_weather_mask",
        .vs                   = "shaders/pass/azgaar_weather/spv/azgaar_weather.vert.spv",
        .fs                   = "shaders/pass/azgaar_weather/spv/azgaar_weather_mask.frag.spv",
        .colorFormat1         = VK_FORMAT_R8_UNORM,
        .noCull               = 1,
        .clearColor1          = {0, 0, 0, 1}, .clearColor1Enabled = 1,
        .clearDepth           = {0, 0}, .clearDepthEnabled = 0,
        .vertexAttributes     = vertexAttrs,
        .vertexAttributeCount = 1,
        .vertexBindings       = &vertexBinding,
        .vertexBindingCount   = 1);
}

// ── TAA ghost-rejection coverage masks ────────────────────────────────
// Two ping-pong R8 images at render resolution.  Each frame the pass
// clears one (render pass, LOAD_OP_CLEAR) and — when weather is active —
// redraws the particle quads into it as coverage.  TAA runs after this
// pass in the same frame, so it reads "this frame" + "last frame" and
// drops the temporal weight where either covers the pixel.  Without this,
// a static-camera TAA keeps every past leaf position in the accumulator
// (the anti-ghost rejection is gated on camera motion), which reads as a
// meteor streak behind each falling leaf.

static void destroyMaskImages(void) {
    if (maskA.img) vulkanDestroyImage(&maskA, NULL);
    maskA = VulkanImage{};
    if (maskB.img) vulkanDestroyImage(&maskB, NULL);
    maskB = VulkanImage{};
    maskFrame = 0;
}

static void createMaskImages(void) {
    if (window.renderWidth <= 0 || window.renderHeight <= 0) {
        return;
    }
    // TRANSFER_DST is required by the initial clear below (vkCmdClearColorImage
    // and its TRANSFER_DST_OPTIMAL layout barriers).
    VkImageUsageFlags usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    maskA = vulkanCreateImage(.name   = "WeatherMaskA",
                              .format  = VK_FORMAT_R8_UNORM,
                              .usage   = usage,
                              .width   = window.renderWidth,
                              .height  = window.renderHeight);
    maskB = vulkanCreateImage(.name   = "WeatherMaskB",
                              .format  = VK_FORMAT_R8_UNORM,
                              .usage   = usage,
                              .width   = window.renderWidth,
                              .height  = window.renderHeight);

    // Clear both so the first TAA frame never reads undefined content. The
    // masks start in UNDEFINED layout; transition them to their first real
    // layout (color attachment — how the weather pass renders into them)
    // BEFORE clearing, so vulkanClearColorImage' restore-back barrier targets
    // a defined layout (VUID 01198 forbids newLayout = UNDEFINED).
    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanTransition(cmd, &maskA, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, 0, 1);
    vulkanTransition(cmd, &maskB, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, 0, 1);
    VkClearColorValue zero = {};
    vulkanClearColorImage(cmd, &maskA, zero);
    vulkanClearColorImage(cmd, &maskB, zero);
    vulkanTransientEnd(cmd, 1);
}

static void swapchainCreated(void*) {
    recreatePipelines();
    destroyMaskImages();
    createMaskImages();
}

// ── Particle buffer seeding ─────────────────────────────────────────────────

// CPU-side pcg1d (same chain as the shaders) for the boot-time / re-seed
// initial data.
static u32 seedPcg1d(u32 v) {
    u32 state = v * 747796405u + 2891336453u;
    u32 word  = ((state >> ((state >> 28) + 4)) ^ state) * 277803737u;
    return (word >> 22) ^ word;
}
static float seedRnd(u32* state) {
    *state = seedPcg1d(*state);
    return (float)(*state >> 8) * (1.0f / 16777216.0f);
}

// Fill `data` with a fresh particle field for `weather`: positions uniform
// through the wrap volume, spawn types rolled from the weights, density
// roulette applied.  Positions are relative to the world origin — the wrap
// recentres them onto the camera within one simulated frame.
static void buildSeedData(vec4* data, u32 count, const VulkanWeatherData* weather) {
    float halfXZ = weather ? weather->params[0] : 90.0f;
    float halfY  = weather ? weather->params[1] : 30.0f;
    float density = weather ? weather->params[2] : 0.5f;
    float w[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (weather) {
        w[0] = weather->types[0];
        w[1] = weather->types[1];
        w[2] = weather->types[2];
        w[3] = weather->types[3];
    }

    for (u32 i = 0; i < count; i++) {
        u32 state = i * 747796405u + 2891336453u;
        float r0 = seedRnd(&state);
        float r1 = seedRnd(&state);
        float r2 = seedRnd(&state);
        float r3 = seedRnd(&state);
        float r4 = seedRnd(&state);

        // Spawn type from the weight vector (uniform mix when unset).
        u32 type = i % 4;
        float sum = w[0] + w[1] + w[2] + w[3];
        if (sum > 1e-4f) {
            float c = r0 * sum;
            type = W_TYPE_LEAVES;
            if (c < w[0]) type = W_TYPE_SNOW;
            else if (c < w[0] + w[1]) type = W_TYPE_RAIN;
            else if (c < w[0] + w[1] + w[2]) type = W_TYPE_DUST;
        }

        // Precipitating types seed UNIFORM through the whole wrap volume —
        // a top-band seed falls as one synchronized sheet ("wall of flakes")
        // that empties and re-fills in visible waves.  Dust seeds in the
        // fixed ground-hugging band around the camera (mirrors the compute
        // respawn placement).
        float bandY = (type == W_TYPE_DUST)
                          ? (-2.0f + 4.0f * r1)
                          : (r1 * 2.0f - 1.0f) * halfY;

        u32 seed = state & W_META_SEED_MASK;
        u32 meta = type | (seed << W_META_SEED_SHIFT);
        if (r4 >= density) meta |= W_META_DISABLED_BIT;

        data[i][0] = (r2 * 2.0f - 1.0f) * halfXZ;
        data[i][1] = bandY;
        data[i][2] = (r3 * 2.0f - 1.0f) * halfXZ;
        data[i][3] = 0.0f;
        memcpy(&data[i][3], &meta, sizeof(float));
    }
}

static void createParticleBuffer(void) {
    if (particleBuffer.buf) return;

    // Pool-size override (debug lever): ENGINE_AZGAAR_WEATHER_COUNT=N.
    particleCount = MAX_WEATHER_PARTICLES;
    const char* env = getenv("ENGINE_AZGAAR_WEATHER_COUNT");
    if (env && *env) {
        int n = atoi(env);
        if (n < 1) n = 1;
        if ((u32)n > MAX_WEATHER_PARTICLES) n = (int)MAX_WEATHER_PARTICLES;
        particleCount = (u32)n;
    }

    // One persistent in-place buffer (D2): compute-storage + vertex input +
    // transfer-dst for the initial seed upload.
    particleBuffer = vulkanCreateGpuBuffer(
        "weather_particles",
        (u64)MAX_WEATHER_PARTICLES * 16,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    // Pre-seed through the volume with a mixed-type field so the first
    // activation doesn't spawn a "wall" of flakes at the box top.
    alignas(16) std::unique_ptr<vec4[]> seedData(new vec4[MAX_WEATHER_PARTICLES]);
    vec4* seedPtr = static_cast<vec4*>(__builtin_assume_aligned(seedData.get(), 16));
    buildSeedData(seedPtr, MAX_WEATHER_PARTICLES, nullptr);
    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanCopy(.cmd = cmd, .source.data = seedPtr, .target.buf = &particleBuffer,
               .size = (u64)MAX_WEATHER_PARTICLES * sizeof(vec4));
    vulkanTransientEnd(cmd, 1);
}

static void consumePendingReseed(void) {
    utils::threadLock(&uploadLock);
    bool reseed = pendingReseed;
    VulkanWeatherData weather = pendingReseedWeather;
    pendingReseed = false;
    utils::threadUnlock(&uploadLock);
    if (!reseed || !particleBuffer.buf) return;

    alignas(16) std::unique_ptr<vec4[]> seedData(new vec4[MAX_WEATHER_PARTICLES]);
    vec4* seedPtr = static_cast<vec4*>(__builtin_assume_aligned(seedData.get(), 16));
    buildSeedData(seedPtr, MAX_WEATHER_PARTICLES, &weather);
    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanCopy(.cmd = cmd, .source.data = seedPtr, .target.buf = &particleBuffer,
               .size = (u64)MAX_WEATHER_PARTICLES * sizeof(vec4));
    vulkanTransientEnd(cmd, 1);
}

void vulkanAzgaarWeatherReseed(const VulkanWeatherData* weather) {
    utils::threadLock(&uploadLock);
    pendingReseed = true;
    pendingReseedWeather = weather ? *weather
                                   : vulkanResourceGetWeatherData();
    utils::threadUnlock(&uploadLock);
}

// ── Debug: periodic GPU particle y-histogram (ENGINE_AZGAAR_WEATHER_DEBUG) ──

static VulkanBuffer debugReadback;
static double debugNextAt = -1.0;
static int debugMoment = 0;
static bool debugCopied = false;
static float debugCamY = 0.0f;

// Log the vertical distribution of the live particle field (camera-relative
// metres at capture).  A synchronized "sheet" shows as one hot band that
// descends between snapshots; a healthy field is flat through the column.
static void debugLogHistogram(void) {
    if (!debugReadback.vmaInfo.pMappedData) return;
    const vec4* data  = static_cast<const vec4*>(debugReadback.vmaInfo.pMappedData);
    u32 bands[8] = {};
    double ySum = 0.0;
    for (u32 i = 0; i < particleCount; i++) {
        float relY = data[i][1] - debugCamY;
        ySum += (double)data[i][1];
        int b = (int)((relY + 30.0f) / 60.0f * 8.0f);
        if (b < 0) b = 0;
        if (b > 7) b = 7;
        bands[b]++;
    }
    utils::info("azgaar_weather hist[%d] t=%.1f camY=%.1f: relY -30..+30 in 8 bands: "
         "%u %u %u %u %u %u %u %u ySum=%.1f",
         debugMoment, utils::timer.timeSinceStart / BILLION, (double)debugCamY, ySum,
         bands[0], bands[1], bands[2], bands[3],
         bands[4], bands[5], bands[6], bands[7], ySum);
}

static void debugMaybeReadback(VulkanCommand* cmd) {
    if (!getenv("ENGINE_AZGAAR_WEATHER_DEBUG")) return;
    if (debugMoment >= 10) return;
    if (debugNextAt < 0.0) {
        debugNextAt = utils::timer.timeSinceStart / BILLION + 6.0;
        return;
    }
    if (utils::timer.timeSinceStart / BILLION < debugNextAt) return;
    if (!debugReadback.buf) {
        debugReadback = vulkanCreateReadbackBuffer("weather_debug_readback",
                                                    (u64)MAX_WEATHER_PARTICLES * 16, 0);
    }
    if (!debugCopied) {
        Entity* e = cameraGetEntity();
        Camera* c = e ? getComponent(e->scene, Camera, e->id) : NULL;
        if (c) debugCamY = c->cameraUbo.renderLocation[1];
        // The compute wrote the buffer earlier in this command buffer; make
        // the writes visible to the copy (without this the readback can
        // observe stale data — the initial seed — forever).
        VkMemoryBarrier b = {
            .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext         = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        };
        vkCmdPipelineBarrier(cmd->cmd,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &b, 0, NULL, 0, NULL);
        VkBufferCopy region = {.srcOffset = 0, .dstOffset = 0, .size = (u64)MAX_WEATHER_PARTICLES * 16};
        vkCmdCopyBuffer(cmd->cmd, particleBuffer.buf, debugReadback.buf, 1, &region);
        debugCopied = true;
        debugNextAt = utils::timer.timeSinceStart / BILLION + 0.05;  // read next call
        return;
    }
    vkDeviceWaitIdle(vulkan.device);
    debugLogHistogram();
    debugCopied = false;
    debugMoment++;
    debugNextAt = utils::timer.timeSinceStart / BILLION + 5.0;
}

// ── System callbacks ────────────────────────────────────────────────────────

void VulkanAzgaarWeatherPass::added() {
    utils::signalSubscribe("swapchainCreated", swapchainCreated);
    recreatePipelines();
    createParticleBuffer();
    createMaskImages();
}

void VulkanAzgaarWeatherPass::preUpdate() {
    if (vulkan.skipFrame) return;
    vulkanResetProfile(vulkan.currentCmd, &drawPipe.profile, 0);
    consumePendingReseed();
}

void VulkanAzgaarWeatherPass::update() {
    if (vulkan.skipFrame) return;
    if (!particleBuffer.buf || !simPipe.pipe || !drawPipe.pipe) return;

    // D5: weather off → the whole pass (dispatch + draw + depth transition)
    // early-outs.  During cross-fades look.w stays 1 so old-type particles
    // keep finishing their fall invisibly.
    VulkanWeatherData weather = vulkanResourceGetWeatherData();
    bool enabled = weather.look[3] >= 0.5f;

    static double lastDbg = -10.0;
    double nowDbg = utils::timer.timeSinceStart / BILLION;
    if (getenv("ENGINE_AZGAAR_WEATHER_DEBUG") && nowDbg - lastDbg >= 2.0) {
        lastDbg = nowDbg;
        utils::info("azgaar_weather pass: enabled=%d types=(%.2f %.2f %.2f %.2f) dens=%.2f "
             "opac=%.2f count=%u",
             enabled,
             (double)weather.types[0], (double)weather.types[1],
             (double)weather.types[2], (double)weather.types[3],
             (double)weather.params[2], (double)weather.look[0], particleCount);
    }
    VulkanCommand* cmd = vulkan.currentCmd;
    if (!cmd) return;

    VulkanImage* sceneColor  = vulkanFrameResourcesGetSceneColor();
    VulkanImage* depthImage  = vulkanFrameResourcesGetDepth();
    if (!sceneColor || !depthImage) return;

    // The fragment shader linearizes depth with the camera's near/far and
    // projection values (soft fade + occlusion).
    Entity* camEntity = cameraGetEntity();
    Camera* cam       = camEntity ? getComponent(camEntity->scene, Camera, camEntity->id) : NULL;
    if (!cam) return;

    WeatherDrawPushConstants draw = {
        .depthIndex = (u32)depthImage->sampledPoolIndex,
        .width      = depthImage->extent.width,
        .height     = depthImage->extent.height,
        .nearZ      = cam->znear,
        .farZ       = cam->zfar,
        .projM00    = cam->cameraUbo.projection[0][0],
        .projM11    = cam->cameraUbo.projection[1][1],
    };

    if (enabled) {
        vulkanBeginProfile(cmd, &drawPipe.profile, 0);

        // Depth → shader-read for both the compute ground kill and the
        // fragment soft fade (water-pass transition; no-op when already
        // there).
        vulkanTransition(cmd, depthImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

        // 1) Simulate: wrap + integrate + depth kill + respawn (in place).
        vulkanBindPipe(cmd, &simPipe);
        WeatherSimPushConstants sim = {
            .particleAddress = particleBuffer.address,
            .depthIndex      = (u32)depthImage->sampledPoolIndex,
            .maxParticles    = particleCount,
        };
        vulkanPush(cmd, &simPipe, sizeof(sim), &sim);
        int groups = (int)((particleCount + 63) / 64);
        vulkanDispatch(cmd, &simPipe, groups, 1, 1);

        // 2) Compute write → vertex input read (culling-pass barrier
        // pattern).  Covers both the mask draw and the main draw below.
        VkMemoryBarrier particleBarrier = {
            .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext         = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
        };
        vkCmdPipelineBarrier(cmd->cmd,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                             0, 1, &particleBarrier, 0, NULL, 0, NULL);
    }

    // 2b) TAA ghost-rejection mask: ALWAYS cleared (LOAD_OP_CLEAR render
    // pass), drawn only while enabled — a stale mask would make TAA drop
    // history on old-leaf pixels after the weather fades out.  Runs after
    // the sim dispatch so coverage uses this frame's positions (the same
    // ones the main draw renders).
    if (maskA.img && maskB.img) {
        VulkanImage* maskCur = (maskFrame & 1) ? &maskB : &maskA;
        vulkanTransition(cmd, maskCur, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, 0, 1);
        vulkanBeginRender(.cmd = cmd, .pipe = &maskPipe, .color1 = maskCur);
        vulkanViewport(cmd, 0, maskCur->extent.height, maskCur->extent.width,
                       -((i32)maskCur->extent.height));
        vulkanScissor(cmd, 0, 0, maskCur->extent.width, maskCur->extent.height);
        if (enabled) {
            vulkanBindPipe(cmd, &maskPipe);
            vulkanBindVertex(cmd, &particleBuffer, 0, NULL, 0, NULL, 0);
            vulkanPush(cmd, &maskPipe, sizeof(draw), &draw);
            vkCmdDraw(cmd->cmd, 6, particleCount, 0, 0);
        }
        vulkanEndRender(cmd);
        maskFrame++;
    }

    if (!enabled) return;

    // 3) Instanced billboard draw of the particle buffer.
    vulkanBeginRender(.cmd      = cmd,
                      .pipe      = &drawPipe,
                      .color1    = sceneColor);

    vulkanViewport(cmd, 0, sceneColor->extent.height, sceneColor->extent.width,
                   -((i32)sceneColor->extent.height));
    vulkanScissor(cmd, 0, 0, sceneColor->extent.width, sceneColor->extent.height);

    vulkanBindPipe(cmd, &drawPipe);
    vulkanBindVertex(cmd, &particleBuffer, 0, NULL, 0, NULL, 0);

    vulkanPush(cmd, &drawPipe, sizeof(draw), &draw);

    // Always MAX instances (D5): density changes never change the draw —
    // disabled particles collapse to zero-area quads in the vertex shader
    // and are culled before rasterization.
    vkCmdDraw(cmd->cmd, 6, particleCount, 0, 0);
    renderer.drawCalls++;
    renderer.instanceCount += particleCount;
    renderer.triangleCount += particleCount * 2;

    vulkanEndRender(cmd);

    debugMaybeReadback(cmd);

    vulkanEndProfile(cmd, &drawPipe.profile, 0);
    vulkanAzgaarWeatherPass.gpuElapsed = drawPipe.profile.elapsed;
}

void VulkanAzgaarWeatherPass::removed() {
    utils::threadLock(&uploadLock);
    if (particleBuffer.buf) vulkanDestroyBuffer(&particleBuffer, VK_NULL_HANDLE);
    particleBuffer = VulkanBuffer{};
    pendingReseed = false;
    utils::threadUnlock(&uploadLock);
    destroyMaskImages();
    vulkanDestroyPipe(&simPipe);
    vulkanDestroyPipe(&drawPipe);
    vulkanDestroyPipe(&maskPipe);
}

VulkanImage* vulkanAzgaarWeatherPassGetMask(void) {
    if (!maskA.img || !maskB.img) return NULL;
    // maskFrame was already incremented by the update that wrote this
    // frame's mask, so flip the parity to land on it.
    return (maskFrame & 1) ? &maskA : &maskB;
}

VulkanImage* vulkanAzgaarWeatherPassGetPrevMask(void) {
    if (!maskA.img || !maskB.img) return NULL;
    return (maskFrame & 1) ? &maskB : &maskA;
}
}  // namespace engine
