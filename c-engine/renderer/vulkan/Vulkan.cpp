#include "Vulkan.h"
#include "Engine.h"
#include "Utils.h"
#include "ecs/Ecs.h"
#include "ecs/system/System.h"
#include "ecs/system/window/WindowSystem.h"
#include "events/Events.h"
#include "futuretask/FutureTask.h"
#include "logger/Logger.h"
#include "renderer/Renderer.h"
#include "renderer/vulkan/resources/VulkanSpd.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanProfile.h"
#include "renderer/vulkan/utils/VulkanBlur.h"
#include "renderer/vulkan/utils/VulkanError.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanIbl.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/utils/VulkanUtils.h"
#include "renderer/vulkan/swapchain/VulkanSwapchain.h"
#include "renderer/vulkan/pass/shadow/VulkanShadowPass.h"
#include "renderer/vulkan/pass/culling/VulkanCullingPass.h"
#include "renderer/vulkan/pass/depth/VulkanDepthPass.h"
#include "renderer/vulkan/pass/occlusion/VulkanOcclusionPass.h"
#include "renderer/vulkan/pass/hiz/VulkanHiZPass.h"
#include "renderer/vulkan/pass/heightmap_terrain/VulkanHeightmapTerrainPass.h"
#include "renderer/vulkan/pass/scene/VulkanScenePass.h"
#include "renderer/vulkan/pass/decal/VulkanDecalPass.h"
#include "renderer/vulkan/pass/contact_shadow/VulkanContactShadowPass.h"
#include "renderer/vulkan/pass/ssr/VulkanSsrPass.h"
#include "renderer/vulkan/pass/volumetric/VulkanVolumetricPass.h"
#include "timer/Timer.h"
#include "renderer/vulkan/pass/composite/VulkanCompositePass.h"
#include "renderer/vulkan/pass/light_culling/VulkanLightCullingPass.h"
#include "renderer/vulkan/pass/skybox/VulkanSkyboxPass.h"
#include "renderer/vulkan/pass/azgaar_water/VulkanAzgaarWaterPass.h"
#include "renderer/vulkan/pass/azgaar_river/VulkanAzgaarRiverPass.h"
#include "renderer/vulkan/pass/ao/VulkanAOPass.h"
#include "renderer/vulkan/pass/azgaar_props/VulkanAzgaarPropsPass.h"
#include "renderer/vulkan/pass/azgaar_weather/VulkanAzgaarWeatherPass.h"
#include "renderer/vulkan/pass/oit/VulkanOitAccumulatePass.h"
#include "renderer/vulkan/pass/oit/VulkanOitCompositePass.h"
#include "renderer/vulkan/pass/fsr/VulkanFsrPass.h"
#include "renderer/vulkan/pass/taa/VulkanTaaPass.h"
#include "renderer/vulkan/pass/dof/VulkanDofPass.h"
#include "renderer/vulkan/pass/bloom/VulkanBloomPass.h"
#include "renderer/vulkan/pass/final/VulkanFinalPass.h"
#include "renderer/vulkan/pass/lpm/VulkanLpmPass.h"
#include "renderer/vulkan/pass/lens/VulkanLensPass.h"
#include "renderer/vulkan/pass/rmlui/VulkanRmluiPass.h"
#include "renderer/vulkan/pass/debug_physics/VulkanDebugPhysicsPass.h"
#include "renderer/vulkan/pass/debug_navmesh/VulkanDebugNavMeshPass.h"

namespace engine {
struct Vulkan vulkan;
const uint32_t VULKAN_VERSION = VK_API_VERSION_1_3;

static void initInstance(void);
static void initPhysicalDevice(void);
static void initLogicalDevice(void);
static void initVma(void);
static char checkDiscreteGpus(const std::vector<VkPhysicalDevice>&);
static char checkIntegratedGpus(const std::vector<VkPhysicalDevice>&);
static char checkOtherGpus(const std::vector<VkPhysicalDevice>&);
static void addPass(System* pass);
static struct VulkanProfile overallProfile;
static std::vector<struct VulkanProfile> passProfiles;

// Screenshot-on-game-loaded state
static char screenshotPathBuf[1024];
static char screenshotPending;
static int screenshotCount;
static int screenshotIndex;
static char screenshotArmed;

// Both single-shot and consecutive screenshots only ARM here; the actual
// capture happens pre-present in vulkanPostUpdate (the readback copy must be
// recorded into the frame's flight command buffer before present releases the
// swapchain image — a separate submit touching the released image is a spec
// violation). Stopping after the final shot is handled there.
/* VRAM accounting (ENGINE_VRAM_REPORT). VK_EXT_memory_budget is the only way
 * to see the whole device footprint: the FidelityFX backend allocates its
 * internal SDF / GI resources through raw vkAllocateMemory, so VMA's own
 * statistics do not include them. `tag` marks when the snapshot was taken. */
static void vulkanMemoryReport(const char* tag) {
    if (!vulkan.memoryBudgetAvailable) {
        utils::warn("vulkanCore: ENGINE_VRAM_REPORT unavailable (VK_EXT_memory_budget missing)");
        return;
    }
    double totalUsed = 0.0;
    double totalSize = 0.0;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT memBudget = {};
    memBudget.sType                                     = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 props2            = {};
    props2.sType                                        = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    props2.pNext                                        = &memBudget;
    vkGetPhysicalDeviceMemoryProperties2(vulkan.physicalDevice, &props2);

    for (u32 i = 0; i < props2.memoryProperties.memoryHeapCount; i++) {
        VkDeviceSize usage = memBudget.heapUsage[i];
        double sizeM       = (double)props2.memoryProperties.memoryHeaps[i].size / 1048576.0;
        if (sizeM < 1.0) {
            continue; /* ignore the tiny 8 MiB device-local tier */
        }
        totalUsed += (double)usage;
        totalSize += (double)props2.memoryProperties.memoryHeaps[i].size;
        utils::info("vulkanMemory[%s]: heap %u used=%.0f MiB budget=%.0f MiB size=%.0f MiB%s",
                    tag,
                    i,
                    (double)usage / 1048576.0,
                    (double)memBudget.heapBudget[i] / 1048576.0,
                    sizeM,
                    (usage > memBudget.heapBudget[i]) ? " OVER-BUDGET" : "");
    }
    utils::info("vulkanMemory[%s]: TOTAL used=%.2f GiB of %.2f GiB",
                tag,
                totalUsed / 1073741824.0,
                totalSize / 1073741824.0);
}

static void vulkanScreenshotArm(void* _path) {
    (void)_path;
    screenshotArmed = 1;
}

static void vulkanScreenshotPath(char* out, int outSize, int index) {
    if (screenshotCount == 1) {
        snprintf(out, outSize, "%s", screenshotPathBuf);
        return;
    }

    // Multi-shot: use the path as a base name, strip a .jpg/.jpeg suffix if
    // present, and append the shot index.
    char base[1024];
    snprintf(base, sizeof(base), "%s", screenshotPathBuf);
    int baseLen = static_cast<int>(strlen(base));
    if (baseLen > 5 && !strncmp(base + baseLen - 5, ".jpeg", 5)) {
        base[baseLen - 5] = '\0';
    } else if (baseLen > 4 && !strncmp(base + baseLen - 4, ".jpg", 4)) {
        base[baseLen - 4] = '\0';
    }
    snprintf(out, outSize, "%s_%d.jpg", base, index);
}

static void onGameLoadedForScreenshot(void* _) {
    (void)_;
    if (!screenshotPending) {
        screenshotPending = 1;
        /* Initial delay (ms) after game load.  Default 5000.  For
         * multi-shot shimmer captures a longer delay lets the TAA
         * accumulator fully converge and the camera settle before the
         * capture window (ENGINE_SCREENSHOT_DELAY_MS). */
        int delayMs = 3000;
        const char* delayEnv = getenv("ENGINE_SCREENSHOT_DELAY_MS");
        if (delayEnv && *delayEnv) delayMs = atoi(delayEnv);
        if (delayMs < 0) delayMs = 0;
        utils::futureTaskAdd(delayMs, vulkanScreenshotArm, screenshotPathBuf);
    }
}

/* Debug: dump named frame images alongside each screenshot capture.
 * ENGINE_DEBUG_DUMP_IMAGES=velocity,depth,color,taa
 * Writes <shotBase>_<name>.jpg next to the screenshot (no-op when unset).
 * Float formats are auto-normalised per channel (see vulkanSaveImage). */
static void vulkanDebugDumpFrameImages(const char* shotPath) {
    static char dumpNames[256];
    static char dumpInit = 0;
    if (!dumpInit) {
        dumpInit = 1;
        const char* env = getenv("ENGINE_DEBUG_DUMP_IMAGES");
        if (env && *env) {
            snprintf(dumpNames, sizeof(dumpNames), "%s", env);
        }
    }
    if (dumpNames[0] == '\0') {
        return;
    }

    char base[1024];
    snprintf(base, sizeof(base), "%s", shotPath);
    int baseLen = static_cast<int>(strlen(base));
    if (baseLen > 4 && !strncmp(base + baseLen - 4, ".jpg", 4)) {
        baseLen -= 4;
    }
    char path[1100];

    char* saveptr = nullptr;
    for (char* tok = strtok_r(dumpNames, ",", &saveptr); tok;
         tok = strtok_r(nullptr, ",", &saveptr)) {
        VulkanImage* img = nullptr;
        if (!strcmp(tok, "velocity")) {
            img = vulkanFrameResourcesGetVelocity();
        } else if (!strcmp(tok, "depth")) {
            img = vulkanFrameResourcesGetDepth();
        } else if (!strcmp(tok, "normals")) {
            img = vulkanFrameResourcesGetNormals();
        } else if (!strcmp(tok, "color")) {
            VulkanImage* c = vulkanFrameResourcesGetCompositeColor();
            img           = c ? c : vulkanFrameResourcesGetSceneColor();
        } else if (!strcmp(tok, "taa")) {
            img = vulkanTaaPassGetOutput();
        } else if (!strcmp(tok, "ao")) {
            img = vulkanAOPassGetOutput();
        } else if (!strcmp(tok, "scene")) {
            img = vulkanFrameResourcesGetSceneColor();
        } else if (!strcmp(tok, "oitReveal")) {
            img = vulkanFrameResourcesGetOitReveal();
        } else if (!strcmp(tok, "oitAccum")) {
            img = vulkanFrameResourcesGetOitAccum();
        } else if (!strcmp(tok, "lensIn")) {
            img = vulkanLensPassGetInput();
        } else if (!strcmp(tok, "lensOut")) {
            img = vulkanDofPassGetOutput() ? vulkanDofPassGetOutput() : nullptr;
        } else if (!strcmp(tok, "dof")) {
            img = vulkanDofPassGetOutput();
        } else if (!strcmp(tok, "bloom")) {
            img = vulkanBloomPassGetBloomImage();
        } else if (strstr(tok, "Raw")) {
            /* <name>Raw: raw byte dump of the image named <name> (dispatches
             * through the regular token table by stripping the suffix). */
            char sub[128];
            snprintf(sub, sizeof(sub), "%s", tok);
            /* strip the trailing "Raw" suffix (branch guarantees it ends
             * with one) so names like "aoRawRaw" keep their inner part */
            sub[strlen(sub) - 3] = 0;
            VulkanImage* rawImg = nullptr;
            if (!strcmp(sub, "velocity")) {
                rawImg = vulkanFrameResourcesGetVelocity();
            } else if (!strcmp(sub, "depth")) {
                rawImg = vulkanFrameResourcesGetDepth();
            } else if (!strcmp(sub, "normals")) {
                rawImg = vulkanFrameResourcesGetNormals();
            } else if (!strcmp(sub, "color")) {
                VulkanImage* c = vulkanFrameResourcesGetCompositeColor();
                rawImg         = c ? c : vulkanFrameResourcesGetSceneColor();
            } else if (!strcmp(sub, "taa")) {
                rawImg = vulkanTaaPassGetOutput();
            } else if (!strcmp(sub, "ao")) {
                rawImg = vulkanAOPassGetOutput();
            } else if (!strcmp(sub, "scene")) {
                rawImg = vulkanFrameResourcesGetSceneColor();
            } else if (!strcmp(sub, "lensIn")) {
                rawImg = vulkanLensPassGetInput();
            } else if (!strcmp(sub, "dof")) {
                rawImg = vulkanDofPassGetOutput();
            } else if (!strcmp(sub, "bloom")) {
                rawImg = vulkanBloomPassGetBloomImage();
            }
            if (!rawImg) {
                continue;
            }
            snprintf(path, sizeof(path), "%.*s_%s.bin", baseLen, base, sub);
            vulkanSaveImageRaw(rawImg, path);
            continue;
        }
        if (!img) {
            continue;
        }
        snprintf(path, sizeof(path), "%.*s_%s.jpg", baseLen, base, tok);
        vulkanSaveImage(img, path);
    }
}

static void onDeviceLost(void) {
    utils::error("device lost: waiting idle before cleanup");
    vkDeviceWaitIdle(vulkan.device);
}

void vulkanInit(void) {
    initInstance();
    initPhysicalDevice();
    initLogicalDevice();
    initVma();

    // Register device lost handler
    vulkanSetDeviceLostHandler(onDeviceLost);
    vulkanResourceInit();
    vulkanIblInit();
    vulkanSwapchainInit();
    vulkanFrameResourcesInit();
    vulkanSpdRunSelfTest();
    overallProfile = vulkanCreateProfile("vulkan");

    addPass(&vulkanCullingPass);
    addPass(&vulkanDepthPass);
    addPass(&vulkanOcclusionPass);
    addPass(&vulkanHiZPass);
    addPass(&vulkanShadowPass);
    addPass(&vulkanContactShadowPass);
    addPass(&vulkanLightCullingPass);
    addPass(&vulkanHeightmapTerrainPass);
    addPass(&vulkanAzgaarPropsPass);
    addPass(&vulkanDebugNavMeshPass);
    addPass(&vulkanScenePass);
    addPass(&vulkanSkyboxPass);
    addPass(&vulkanAzgaarRiverPass);
    addPass(&vulkanAzgaarWaterPass);
    addPass(&vulkanAzgaarWeatherPass);
    addPass(&vulkanOitAccumulatePass);
    addPass(&vulkanOitCompositePass);
    addPass(&vulkanSsrPass);
    addPass(&vulkanAOPass);
    addPass(&vulkanVolumetricPass);
    addPass(&vulkanDecalPass);
    addPass(&vulkanCompositePass);
    addPass(&vulkanTaaPass);
    addPass(&vulkanDofPass);
    addPass(&vulkanFsrPass);
    addPass(&vulkanBloomPass);
    addPass(&vulkanFinalPass);
    addPass(&vulkanLpmPass);
    addPass(&vulkanLensPass);
    addPass(&vulkanDebugPhysicsPass);
    addPass(&vulkanRmluiPass);

#ifndef NDEBUG
    {
        const char* screenshotPath = getenv("ENGINE_SCREENSHOT");
        if (screenshotPath) {
            snprintf(screenshotPathBuf, sizeof(screenshotPathBuf), "%s", screenshotPath);
            const char* countEnv = getenv("ENGINE_SCREENSHOT_COUNT");
            screenshotCount = countEnv ? atoi(countEnv) : 1;
            if (screenshotCount < 1) screenshotCount = 1;
            utils::signalSubscribe("gameLoaded", onGameLoadedForScreenshot);
        }
    }
#endif

#if !defined(NDEBUG) && defined(__linux__)
    if (getenv("ENGINE_RENDERDOC_CAPTURE")) {
        captureFrameRenderDoc();
    }
#endif
}

static void vulkanDestroyDelayed(void* _) {
    vulkanWaitIdle("wait before cleaning up");

    for (System* pass : renderer.passes) {
        pass->removed();
    }

    vulkanSpdDestroy();
    vulkanFrameResourcesDestroy();
    vulkanIblDestroy();
    vulkanResourceDestroy();
    vulkanBlurCleanup();

    // Clean up transient commands (e.g. screenshot readback) before swapchain
    // destruction to avoid driver-side heap corruption on AMD.
    vulkanCleanupGarbage();
    vulkanWaitIdle("wait before swapchain destroy");
    vulkanSwapchainDestroy();

    for (size_t i = 0; i < passProfiles.size(); i++) {
        vulkanDestroyProfile(&passProfiles[i]);
    }
    vulkanDestroyProfile(&overallProfile);
    vkDestroySurfaceKHR(vulkan.instance, vulkan.surface, nullptr);
    vmaDestroyAllocator(vulkan.vmaAllocator);
    vkDestroyDevice(vulkan.device, nullptr);
    if (utils::isDebug()) {
        vkDestroyDebugUtilsMessengerEXT(vulkan.instance, vulkan.debugMessenger, 0);
    }
    vkDestroyInstance(vulkan.instance, nullptr);
}

void vulkanDestroy(void) {
    utils::futureTaskAdd(0, vulkanDestroyDelayed, 0);
}

void addPass(System* pass) {
    utils::info("vulkanCore: adding pass %s", pass->name);
    renderer.passes.push_back(pass);
    struct VulkanProfile prof = vulkanCreateProfile(utils::strtmp("pass_%s", pass->name));
    passProfiles.push_back(prof);
    pass->added();
}

void vulkanPostUpdate(void) {
    vulkanSwapchainBegin();
    vulkanFrameResourcesUpdate();
    vulkanResourceUpdate();

    // Reset per-frame render stats (passes accumulate into these)
    renderer.drawCalls     = 0;
    renderer.instanceCount = 0;
    renderer.triangleCount = 0;

    {
        vulkanResetProfile(vulkan.currentCmd, &overallProfile, 1);
        vulkanBeginProfile(vulkan.currentCmd, &overallProfile, 1);

        for (System* pass : renderer.passes) {
            systemPreUpdate(pass);
        }

        for (size_t i = 0; i < renderer.passes.size(); i++) {
            vulkanResetProfile(vulkan.currentCmd, &passProfiles[i], 1);
            vulkanResetProfileStats(vulkan.currentCmd, &passProfiles[i], 1);
        }

        /* ENGINE_VRAM_REPORT=<frame>: dump the device memory before the
         * passes of that frame. Reported once, then disabled. */
        static char vramReportInit  = 0;
        static u32  vramReportFrame = 0xFFFFFFFFu;
        static char vramReportDone  = 0;
        if (!vramReportInit) {
            vramReportInit = 1;
            const char* env = getenv("ENGINE_VRAM_REPORT");
            if (env && *env) vramReportFrame = (u32)atoi(env);
        }
        if (!vramReportDone && vramReportFrame != 0xFFFFFFFFu &&
            (u64)vramReportFrame == utils::timer.frameCounter) {
            vramReportDone = 1;
            vulkanMemoryReport("pre-passes");
        }

        for (size_t i = 0; i < renderer.passes.size(); i++) {
            vulkanBeginProfile(vulkan.currentCmd, &passProfiles[i], 1);
            vulkanBeginProfileStats(vulkan.currentCmd, &passProfiles[i], 1);
            systemUpdate(renderer.passes[i]);
            vulkanEndProfileStats(vulkan.currentCmd, &passProfiles[i], 1);
            vulkanEndProfile(vulkan.currentCmd, &passProfiles[i], 1);
        }

        for (size_t i = 0; i < renderer.passes.size(); i++) {
            systemPostUpdate(renderer.passes[i]);
            renderer.passes[i]->gpuElapsed = passProfiles[i].elapsed;
        }

        vulkanEndProfile(vulkan.currentCmd, &overallProfile, 1);

        renderer.rendererElapsedGpu = overallProfile.elapsed;

        /* Optional per-pass GPU time log: ENGINE_LOG_PASS_GPU=1 prints an
         * averaged per-pass breakdown every 600 frames. */
        static char passGpuLogInit = 0;
        static char passGpuLogOn   = 0;
        static u32 passGpuLogFrame = 0;
        static double passGpuAccum[64];
        static double passGpuAccumTotal;
        static u32 passGpuAccumCount;
        if (!passGpuLogInit) {
            passGpuLogInit = 1;
            const char* env = getenv("ENGINE_LOG_PASS_GPU");
            if (env && *env && atoi(env)) passGpuLogOn = 1;
        }
        if (passGpuLogOn) {
            size_t n = static_cast<i32>(renderer.passes.size());
            if (n > 64) n = 64;
            passGpuLogFrame++;
            /* Skip the first 600 frames (asset streaming, warmup), then
             * print an average of the next 600 frames. */
            if (passGpuLogFrame > 600 && passGpuLogFrame <= 1200) {
                passGpuAccumTotal += overallProfile.elapsed;
                for (size_t i = 0; i < n; i++) {
                    passGpuAccum[i] += passProfiles[i].elapsed;
                }
                passGpuAccumCount++;
            }
            if (passGpuLogFrame == 1200) {
                utils::info("pass-gpu: total=%.2f ms (avg of %u frames) fps=%.1f", passGpuAccumTotal / passGpuAccumCount / MILLION, static_cast<u32>(passGpuAccumCount), utils::timer.fps);
                for (size_t i = 0; i < n; i++) {
                    utils::info("pass-gpu:   %-20s %6.2f", renderer.passes[i]->name, passGpuAccum[i] / passGpuAccumCount / MILLION);
                }
                passGpuAccumTotal = 0;
                for (size_t i = 0; i < n; i++) passGpuAccum[i] = 0;
                passGpuAccumCount = 0;
            }
        }
    }
    // Screenshot capture (single-shot or consecutive): while armed, record a
    // readback copy of the just-rendered swapchain image into THIS frame's
    // flight command buffer — it must happen before vulkanSwapchainEnd, since
    // present releases the image and any transition/copy from a separate
    // submit would use a not-acquired presentable image (spec violation).
    // Consecutive mode captures one image per frame until all shots are taken
    // so temporal artifacts (e.g. TAA shimmer) are visible across the
    // sequence.
    VulkanBuffer screenshotReadback = {};
    VulkanImage* screenshotImg      = nullptr;
    char         shotPath[1024]     = {};
    char         screenshotRecorded = 0;
    if (screenshotArmed && !vulkan.skipFrame && screenshotIndex < screenshotCount) {
        screenshotImg = vulkanSwapchain.currentSwapchainImage;
        vulkanScreenshotPath(shotPath, static_cast<int>(sizeof(shotPath)), screenshotIndex + 1);
        vulkanScreenshotRecord(screenshotImg, &screenshotReadback);
        screenshotRecorded = screenshotReadback.buf != 0;
    }

    vulkanSwapchainEnd();

    if (screenshotRecorded) {
        vulkanSwapchainWaitCurrentFlight();
        vulkanScreenshotWriteJpg(&screenshotReadback, screenshotImg, shotPath);
        vulkanDestroyBuffer(&screenshotReadback, nullptr);
        vulkanDebugDumpFrameImages(shotPath);
        screenshotIndex++;
        if (screenshotIndex >= screenshotCount) {
            utils::info("vulkanScreenshot: captured all %d screenshots — stopping", screenshotCount);
            engineStop();
        }
    }
}

void initInstance(void) {
    if (volkInitialize() != VK_SUCCESS) {
        utils::terminate("failed initialize volk!\n");
    }

    VkApplicationInfo appInfo  = {};
    appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.applicationVersion = VULKAN_VERSION;
    appInfo.engineVersion      = VULKAN_VERSION;
    appInfo.apiVersion         = VULKAN_VERSION;

    std::vector<const char*> extensions        = {};
    u32 extensionCount                   = 0;
    char const* const* backendExtensions = windowSystemGetRequiredVulkanExtensions(&extensionCount);

    extensions.resize(extensionCount);
    for (i32 i = 0, si = extensionCount; i < si; i++) {
        extensions[i] = backendExtensions[i];
    }
    if (utils::isDebug()) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkInstanceCreateInfo instanceCreateInfo    = {};
    instanceCreateInfo.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceCreateInfo.pApplicationInfo        = &appInfo;
    instanceCreateInfo.enabledExtensionCount   = static_cast<i32>(extensions.size());
    instanceCreateInfo.ppEnabledExtensionNames = extensions.data();

    const char* validationLayers[] = {"VK_LAYER_KHRONOS_validation"};
    bool hasValidationLayers       = false;
    if (utils::isDebug()) {
        u32 layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> layerProps = {};
        layerProps.resize(layerCount);
        if (layerCount > 0) {
            vkEnumerateInstanceLayerProperties(&layerCount, layerProps.data());
        }
        for (u32 i = 0; i < layerCount; i++) {
            if (strcmp(layerProps[i].layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                hasValidationLayers = true;
                break;
            }
        }
        if (hasValidationLayers) {
            instanceCreateInfo.enabledLayerCount   = 1;
            instanceCreateInfo.ppEnabledLayerNames = validationLayers;
        }
    }

    if (vkCreateInstance(&instanceCreateInfo, nullptr, &vulkan.instance) != VK_SUCCESS) {
        utils::terminate("vulkanCore: failed to initialize vulkan instance!");
    }

    volkLoadInstanceOnly(vulkan.instance);

    if (utils::isDebug()) {
        VkDebugUtilsMessengerCreateInfoEXT messengerInfo = {};
        messengerInfo.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messengerInfo.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messengerInfo.pfnUserCallback = vulkanValidationLog;
        vkCreateDebugUtilsMessengerEXT(vulkan.instance,
                                       &messengerInfo,
                                       nullptr,
                                       &vulkan.debugMessenger);
    }
}

void initPhysicalDevice(void) {
    // (ill do it later)™
    // vulkanSelectDeviceGui();

    u32 deviceCount = 0;
    vkEnumeratePhysicalDevices(vulkan.instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        utils::terminate("vulkanCore: could not find any GPUs with vulkan support!");
    }
    std::vector<VkPhysicalDevice> physicalDevices = {};
    physicalDevices.resize(deviceCount);
    vkEnumeratePhysicalDevices(vulkan.instance, &deviceCount, physicalDevices.data());

    char found = checkDiscreteGpus(physicalDevices);
    if (!found) {
        found = checkIntegratedGpus(physicalDevices);
    }
    if (!found) {
        found = checkOtherGpus(physicalDevices);
    }
    if (!found) {
        utils::warn(
            "------------------------------------------------------------------"
            "--------------------------");
        utils::warn(
            "vulkanCore: this application needs a vulkan 1.3 device that "
            "supports the following features;");
        utils::warn("  - bufferDeviceAddress");
        utils::warn("  - descriptorIndexing");
        utils::warn("  - synchronization2");
        utils::warn("  - multiDrawIndirect");
        utils::warn("  - drawIndirectCount");
        utils::warn("  - samplerAnisotropy");
        utils::warn("  - sampleRateShading");
        utils::warn("  - tessellationShader");
        utils::warn("  - fillModeNonSolid");
        utils::error("vulkanCore: minimum supported GPU: GTX 1080 Ti or equivalent");
        utils::terminate("vulkanCore: could not find suitable gpu");
    }

}

void initLogicalDevice(void) {
    if (!windowSystemCreateVulkanSurface(vulkan.instance, &vulkan.surface)) {
        utils::terminate("vulkanCore: failed to create window surface!");
    }
    std::vector<const char*> extensions = {};
    extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    // Conditionally enable VK_AMD_device_coherent_memory if available.
    // Required by FSR 3.1 SDK: it queries this extension at runtime and,
    // when found, prefers VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD memory
    // types. If the feature is not enabled, VMA allocates from those memory
    // types and triggers VUID-vkAllocateMemory-deviceCoherentMemory-02790.
    // Safe to do unconditionally — the extension is only present on AMD GPUs,
    // and we only request it when the physical device reports availability.
    u32 extCount = 0;
    vkEnumerateDeviceExtensionProperties(vulkan.physicalDevice, NULL, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts = {};
    if (extCount > 0) {
        exts.resize(extCount);
        vkEnumerateDeviceExtensionProperties(vulkan.physicalDevice, nullptr, &extCount, exts.data());
        for (u32 i = 0; i < extCount; i++) {
            if (strcmp(exts[i].extensionName, VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME) == 0) {
                extensions.push_back(VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME);
                break;
            }
        }
        /* VRAM accounting (ENGINE_VRAM_REPORT): VK_EXT_memory_budget reports
         * bytes allocated / budget / limit per memory heap, including the
         * allocations the FidelityFX backend makes directly through
         * vkAllocateMemory (VMA's own statistics cannot see those). */
        for (u32 i = 0; i < extCount; i++) {
            if (strcmp(exts[i].extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0) {
                vulkan.memoryBudgetAvailable = 1;
                extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
                break;
            }
        }
    }

    u32 queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vulkan.physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilyProperties = {};
    queueFamilyProperties.resize(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(vulkan.physicalDevice,
                                             &queueFamilyCount,
                                             queueFamilyProperties.data());

    int graphicsFamily = -1;
    int computeFamily  = -1;
    int transferFamily = -1;
    int presentFamily  = -1;

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        VkQueueFamilyProperties props = queueFamilyProperties[i];

        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(vulkan.physicalDevice,
                                             i,
                                             vulkan.surface,
                                             &presentSupport);

        if (props.queueFlags & VK_QUEUE_GRAPHICS_BIT && graphicsFamily == -1) {
            graphicsFamily = i;
        }

        if ((props.queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            (props.queueFlags & VK_QUEUE_COMPUTE_BIT) && computeFamily == -1) {
            computeFamily = i;
        }

        if (!(props.queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            (props.queueFlags & VK_QUEUE_TRANSFER_BIT) && transferFamily == -1) {
            transferFamily = i;
        }

        if (presentSupport && presentFamily == -1) {
            presentFamily = i;
        }
    }

    if (computeFamily == -1) {
        computeFamily = graphicsFamily;
    }
    if (presentFamily == -1) {
        presentFamily = graphicsFamily;
    }
    if (transferFamily == -1) {
        transferFamily = graphicsFamily;
    }

    // dedicated transfer family requires some more work
    // (ill do it later)™
    transferFamily = graphicsFamily;

    float queuePriority = 1.0F;
    VkDeviceQueueCreateInfo queueCreateInfos[4];
    uint32_t queueCreateInfoCount = 0;
    bool uniqueIndices[32]        = {};

    uint32_t families[] = {static_cast<uint32_t>(graphicsFamily), static_cast<uint32_t>(computeFamily), static_cast<uint32_t>(transferFamily), static_cast<uint32_t>(presentFamily)};
    for (int i = 0; i < 4; i++) {
        uint32_t family = families[i];
        if (!uniqueIndices[family]) {
            uniqueIndices[family] = true;
            queueCreateInfos[queueCreateInfoCount].sType =
                VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueCreateInfos[queueCreateInfoCount].pNext            = 0;
            queueCreateInfos[queueCreateInfoCount].flags            = 0;
            queueCreateInfos[queueCreateInfoCount].queueFamilyIndex = family;
            queueCreateInfos[queueCreateInfoCount].queueCount       = 1;
            queueCreateInfos[queueCreateInfoCount].pQueuePriorities = &queuePriority;
            queueCreateInfoCount++;
        }
    }

    VkPhysicalDeviceFeatures deviceFeatures             = {};
    deviceFeatures.samplerAnisotropy                    = VK_TRUE;
    deviceFeatures.sampleRateShading                    = VK_TRUE;
    deviceFeatures.tessellationShader                   = VK_TRUE;
    deviceFeatures.shaderInt64                          = VK_TRUE;
    deviceFeatures.multiDrawIndirect                    = VK_TRUE;
    deviceFeatures.fillModeNonSolid                     = VK_TRUE;
    deviceFeatures.shaderImageGatherExtended            = VK_TRUE;
    deviceFeatures.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    deviceFeatures.shaderInt16                          = VK_TRUE;
    deviceFeatures.fragmentStoresAndAtomics             = VK_TRUE;
    deviceFeatures.wideLines                            = vulkan.deviceFeatures2.features.wideLines;
    deviceFeatures.independentBlend                     = VK_TRUE;
    deviceFeatures.depthClamp                           = VK_TRUE;
    deviceFeatures.depthBiasClamp                       = VK_TRUE;
    deviceFeatures.pipelineStatisticsQuery              = VK_TRUE;

    VkPhysicalDeviceSynchronization2Features sync2Features = {};
    sync2Features.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
    sync2Features.synchronization2 = VK_TRUE;

    VkPhysicalDeviceCoherentMemoryFeaturesAMD coherentMemoryFeatures = {};
    coherentMemoryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COHERENT_MEMORY_FEATURES_AMD;
    coherentMemoryFeatures.deviceCoherentMemory = VK_TRUE;

    VkPhysicalDeviceVulkan11Features vulkan11Features = {};
    vulkan11Features.sType                = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    vulkan11Features.shaderDrawParameters = VK_TRUE;
    vulkan11Features.pNext                = &coherentMemoryFeatures;
    coherentMemoryFeatures.pNext          = &sync2Features;

    VkPhysicalDeviceVulkan12Features vulkan12Features = {};
    vulkan12Features.sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vulkan12Features.drawIndirectCount = VK_TRUE;
    /* FidelityFX CACAO / Lens shaders declare the SPIR-V Float16
     * capability; supported by the device (RADV), just needs enabling. */
    vulkan12Features.shaderFloat16    = VK_TRUE;
    // Descriptor indexing features (moved from
    // VkPhysicalDeviceDescriptorIndexingFeatures)
    vulkan12Features.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    vulkan12Features.descriptorBindingStorageImageUpdateAfterBind  = VK_TRUE;
    vulkan12Features.descriptorBindingSampledImageUpdateAfterBind  = VK_TRUE;
    vulkan12Features.descriptorBindingUpdateUnusedWhilePending     = VK_TRUE;
    vulkan12Features.runtimeDescriptorArray                        = VK_TRUE;
    vulkan12Features.shaderStorageBufferArrayNonUniformIndexing    = VK_TRUE;
    vulkan12Features.shaderUniformBufferArrayNonUniformIndexing    = VK_TRUE;
    vulkan12Features.shaderStorageImageArrayNonUniformIndexing     = VK_TRUE;
    vulkan12Features.shaderSampledImageArrayNonUniformIndexing     = VK_TRUE;
    vulkan12Features.descriptorBindingPartiallyBound               = VK_TRUE;
    // Buffer device address feature (moved from
    // VkPhysicalDeviceBufferDeviceAddressFeatures)
    vulkan12Features.bufferDeviceAddress = VK_TRUE;
    // 8-bit storage for general use
    vulkan12Features.storageBuffer8BitAccess           = VK_TRUE;
    vulkan12Features.uniformAndStorageBuffer8BitAccess = VK_TRUE;
    // 16-bit float/int — optional; FSR 3.1 uses FP16 shader permutations
    // when available but falls back to FP32 automatically.
    // Not supported on older GPUs (e.g. Pascal / GTX 10xx).

    vulkan12Features.pNext = &vulkan11Features;

    VkPhysicalDeviceDynamicRenderingFeatures dynamicRenderingFeatures = {};
    dynamicRenderingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    dynamicRenderingFeatures.dynamicRendering = VK_TRUE;
    dynamicRenderingFeatures.pNext            = &vulkan12Features;

    if (utils::isDebug()) {
        vulkan12Features.bufferDeviceAddressCaptureReplay = VK_TRUE;
    }

    VkDeviceCreateInfo deviceCreateInfo      = {};
    deviceCreateInfo.pNext                   = &dynamicRenderingFeatures;
    deviceCreateInfo.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.queueCreateInfoCount    = queueCreateInfoCount;
    deviceCreateInfo.pQueueCreateInfos       = queueCreateInfos;
    deviceCreateInfo.pEnabledFeatures        = &deviceFeatures;
    deviceCreateInfo.enabledExtensionCount   = static_cast<i32>(extensions.size());
    deviceCreateInfo.ppEnabledExtensionNames = extensions.data();
    VkResult result =
        vkCreateDevice(vulkan.physicalDevice, &deviceCreateInfo, nullptr, &vulkan.device);
    if (result != VK_SUCCESS) {
        utils::terminate("vulkanCore: failed to create logical device! code: %d", result);
    }

    volkLoadDevice(vulkan.device);

    vkGetDeviceQueue(vulkan.device, graphicsFamily, 0, &vulkan.graphicsQueue);
    vulkan.graphicsFamilyIndex = graphicsFamily;
    if (utils::isDebug()) {
        vulkanUtilsSetName(reinterpret_cast<u64>(vulkan.graphicsQueue), VK_OBJECT_TYPE_QUEUE, "queue graphics");
    }

}

void initVma(void) {
    VmaVulkanFunctions vmaVulkanFunctions    = {};
    vmaVulkanFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vmaVulkanFunctions.vkGetDeviceProcAddr   = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo allocatorCreateInfo = {};
    allocatorCreateInfo.vulkanApiVersion       = VULKAN_VERSION;
    allocatorCreateInfo.physicalDevice         = vulkan.physicalDevice;
    allocatorCreateInfo.device                 = vulkan.device;
    allocatorCreateInfo.instance               = vulkan.instance;
    allocatorCreateInfo.pVulkanFunctions       = &vmaVulkanFunctions;
    allocatorCreateInfo.flags                  = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;

    VkPhysicalDeviceProperties deviceProperties = {};
    vkGetPhysicalDeviceProperties(vulkan.physicalDevice, &deviceProperties);

    if (vmaCreateAllocator(&allocatorCreateInfo, &vulkan.vmaAllocator) != VK_SUCCESS) {
        utils::terminate("vulkanCore: failed to initialize vma!");
    }
}

static char checkGpu(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties deviceProperties) {
    VkPhysicalDeviceBufferDeviceAddressFeatures deviceAddressFeatures = {};
    deviceAddressFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    deviceAddressFeatures.pNext = 0;
    VkPhysicalDeviceDescriptorIndexingFeatures indexingFeatures = {};
    indexingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    indexingFeatures.pNext = &deviceAddressFeatures;
    VkPhysicalDeviceSynchronization2Features sync2Features = {};
    sync2Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
    sync2Features.pNext = &indexingFeatures;
    VkPhysicalDeviceVulkan12Features vulkan12Features = {};
    vulkan12Features.sType              = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    vulkan12Features.pNext              = &sync2Features;
    VkPhysicalDeviceFeatures2 features2 = {};
    features2.sType                     = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext                     = &vulkan12Features;
    vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);

    utils::debug("vulkanCore: found gpu %s", deviceProperties.deviceName);

    if (!deviceAddressFeatures.bufferDeviceAddress) {
        utils::warn(
            "vulkanCore: this gpu does not support bufferDeviceAddress, "
            "skipping");
        return 0;
    }
    if (!indexingFeatures.descriptorBindingStorageBufferUpdateAfterBind) {
        utils::warn(
            "vulkanCore: this gpu does not support "
            "descriptorBindingStorageBufferUpdateAfterBind, skipping");
        return 0;
    }
    if (!indexingFeatures.descriptorBindingStorageImageUpdateAfterBind) {
        utils::warn(
            "vulkanCore: this gpu does not support "
            "descriptorBindingStorageImageUpdateAfterBind, skipping");
        return 0;
    }
    if (!indexingFeatures.descriptorBindingSampledImageUpdateAfterBind) {
        utils::warn(
            "vulkanCore: this gpu does not support "
            "descriptorBindingSampledImageUpdateAfterBind, skipping");
        return 0;
    }
    if (!indexingFeatures.descriptorBindingUpdateUnusedWhilePending) {
        utils::warn(
            "vulkanCore: this gpu does not support "
            "descriptorBindingUpdateUnusedWhilePending, skipping");
        return 0;
    }
    if (!indexingFeatures.runtimeDescriptorArray) {
        utils::warn(
            "vulkanCore: this gpu does not support runtimeDescriptorArray, "
            "skipping");
        return 0;
    }
    if (!indexingFeatures.descriptorBindingPartiallyBound) {
        utils::warn(
            "vulkanCore: this gpu does not support "
            "descriptorBindingPartiallyBound, skipping");
        return 0;
    }
    if (!indexingFeatures.shaderStorageBufferArrayNonUniformIndexing) {
        utils::warn(
            "vulkanCore: this gpu does not support "
            "shaderStorageBufferArrayNonUniformIndexing, skipping");
        return 0;
    }
    if (!indexingFeatures.shaderUniformBufferArrayNonUniformIndexing) {
        utils::warn(
            "vulkanCore: this gpu does not support "
            "shaderUniformBufferArrayNonUniformIndexing, skipping");
        return 0;
    }
    if (!indexingFeatures.shaderStorageImageArrayNonUniformIndexing) {
        utils::warn(
            "vulkanCore: this gpu does not support "
            "shaderStorageImageArrayNonUniformIndexing, skipping");
        return 0;
    }
    if (!indexingFeatures.shaderSampledImageArrayNonUniformIndexing) {
        utils::warn(
            "vulkanCore: this gpu does not support "
            "shaderSampledImageArrayNonUniformIndexing, skipping");
        return 0;
    }

    if (!sync2Features.synchronization2) {
        utils::warn("vulkanCore: this gpu does not support synchronization2, skipping");
        return 0;
    }

    if (!features2.features.multiDrawIndirect) {
        utils::warn(
            "vulkanCore: this gpu does not support multiDrawIndirect, "
            "skipping");
        return 0;
    }

    if (!features2.features.shaderInt64) {
        utils::warn("vulkanCore: this gpu does not support shaderInt64, skipping");
        return 0;
    }

    if (!features2.features.samplerAnisotropy) {
        utils::warn(
            "vulkanCore: this gpu does not support samplerAnisotropy, "
            "skipping");
        return 0;
    }

    if (!features2.features.sampleRateShading) {
        utils::warn(
            "vulkanCore: this gpu does not support sampleRateShading, "
            "skipping");
        return 0;
    }

    if (!features2.features.tessellationShader) {
        utils::warn(
            "vulkanCore: this gpu does not support tessellationShader, "
            "skipping");
        return 0;
    }

    if (!features2.features.fillModeNonSolid) {
        utils::warn("vulkanCore: this gpu does not support fillModeNonSolid, skipping");
        return 0;
    }

    if (!vulkan12Features.drawIndirectCount) {
        utils::warn(
            "vulkanCore: this gpu does not support drawIndirectCount, "
            "skipping");
        return 0;
    }

    if (!features2.features.shaderImageGatherExtended) {
        utils::warn(
            "vulkanCore: this gpu does not support shaderImageGatherExtended, "
            "skipping");
        return 0;
    }

    if (!features2.features.shaderStorageImageWriteWithoutFormat) {
        utils::warn(
            "vulkanCore: this gpu does not support "
            "shaderStorageImageWriteWithoutFormat, skipping");
        return 0;
    }

    if (!features2.features.shaderInt16) {
        utils::warn("vulkanCore: this gpu does not support shaderInt16, skipping");
        return 0;
    }

    if (!vulkan12Features.shaderFloat16) {
        utils::info("vulkanCore: shaderFloat16 not supported, FSR will use FP32 permutations");
    }

    vulkan.physicalDevice   = physicalDevice;
    vulkan.deviceProperties = deviceProperties;
    vulkan.deviceFeatures2  = features2;
    utils::debug("vulkanCore: selected gpu %s", deviceProperties.deviceName);
    return 1;
}

char checkDiscreteGpus(const std::vector<VkPhysicalDevice>& devices) {
    for (VkPhysicalDevice physicalDevice : devices) {
        VkPhysicalDeviceProperties deviceProperties = {};
        vkGetPhysicalDeviceProperties(physicalDevice, &deviceProperties);
        if (deviceProperties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            continue;
        }

        if (checkGpu(physicalDevice, deviceProperties)) {
            return 1;
        }
    }
    return 0;
}

char checkIntegratedGpus(const std::vector<VkPhysicalDevice>& devices) {
    for (VkPhysicalDevice physicalDevice : devices) {
        VkPhysicalDeviceProperties deviceProperties = {};
        vkGetPhysicalDeviceProperties(physicalDevice, &deviceProperties);
        if (deviceProperties.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            continue;
        }

        if (checkGpu(physicalDevice, deviceProperties)) {
            return 1;
        }
    }
    return 0;
}

char checkOtherGpus(const std::vector<VkPhysicalDevice>& devices) {
    for (VkPhysicalDevice physicalDevice : devices) {
        VkPhysicalDeviceProperties deviceProperties = {};
        vkGetPhysicalDeviceProperties(physicalDevice, &deviceProperties);
        if (deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
            deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            continue;
        }

        if (checkGpu(physicalDevice, deviceProperties)) {
            return 1;
        }
    }
    return 0;
}

void vulkanSetVsync(bool vsync) {
    (void)vsync;
    vulkanSwapchainRecreate();
}

const struct VulkanProfile* vulkanGetPassProfiles(void) {
    return passProfiles.data();
}

size_t vulkanGetPassProfileCount(void) {
    return static_cast<i32>(passProfiles.size());
}
}  // namespace engine
