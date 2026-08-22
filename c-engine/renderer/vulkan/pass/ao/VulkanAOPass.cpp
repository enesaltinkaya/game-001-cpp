#include "VulkanAOPass.h"
#include "ecs/Ecs.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/window/WindowSystem.h"
#include "events/Events.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanProfile.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include <stdlib.h>
#include <string.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <FidelityFX/host/ffx_cacao.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#pragma GCC diagnostic pop

namespace engine {

static void swapchainCreated(void* _);
static void cacaoDestroyContext(void);
static void cacaoDestroyOutput(void);
static char  cacaoEnsureContext(u32 width, u32 height);
static void  cacaoUpdate(VulkanCommand* cmd, VulkanImage* depth, VulkanImage* normals, Camera* camera);
static VkImageCreateInfo makeImageCreateInfo(VulkanImage* image);
static FfxResource wrapImageResource(VulkanImage* image,
                                     FfxResourceUsage usage,
                                     FfxResourceStates state,
                                     const wchar_t* name);

static double elapsedCPU;
static double elapsedGPU;
static char aoDisabled;

/* CACAO (FidelityFX) state.  Own FfxInterface + scratch buffer (same
 * pattern as the FSR pass); the CACAO context's internal GPU resources
 * are freed on swapchain recreation / pass removal. */
static void* cacaoScratchBuffer;
static size_t cacaoScratchBufferSize;
static FfxInterface cacaoBackendInterface;
static char cacaoBackendReady;
static FfxCacaoContext cacaoContext;
static char cacaoContextReady;
static VulkanImage cacaoOutput;
static u32 cacaoWidth;
static u32 cacaoHeight;
static VulkanProfile cacaoProfile;
static char cacaoProfileReady;

VulkanAOPass vulkanAOPass;

VulkanAOPass::VulkanAOPass() : System("ao") {}

void VulkanAOPass::added() {
    const char* env = getenv("ENGINE_AO_DISABLED");
    if (env && *env && atoi(env)) aoDisabled = 1;

    utils::signalSubscribe("swapchainCreated", swapchainCreated);

    cacaoProfile      = vulkanCreateProfile("cacao_ao");
    cacaoProfileReady = 1;
}

void VulkanAOPass::preUpdate() {
    if (vulkan.skipFrame) {
        return;
    }
    if (cacaoProfileReady) {
        vulkanResetProfile(vulkan.currentCmd, &cacaoProfile, 0);
    }
}

static void swapchainCreated(void* _) {
    (void)_;
    cacaoDestroyContext();
    cacaoDestroyOutput();
}

static void cacaoDestroyContext(void) {
    if (cacaoContextReady) {
        ffxCacaoContextDestroy(&cacaoContext);
        cacaoContext      = FfxCacaoContext{};
        cacaoContextReady = 0;
    }
}

static void cacaoDestroyOutput(void) {
    if (cacaoOutput.img) {
        vulkanDestroyImage(&cacaoOutput, NULL);
        cacaoOutput = VulkanImage{};
    }
    cacaoWidth  = 0;
    cacaoHeight = 0;
}

static char cacaoEnsureContext(u32 width, u32 height) {
    if (!cacaoBackendReady) {
        cacaoScratchBufferSize = ffxGetScratchMemorySizeVK(vulkan.physicalDevice, 1);
        cacaoScratchBuffer     = calloc(1, cacaoScratchBufferSize);
        if (!cacaoScratchBuffer) {
            utils::error("vulkanAOPass: failed to allocate %zu bytes of CACAO backend scratch memory",
                         cacaoScratchBufferSize);
            return 0;
        }

        VkDeviceContext deviceContext = {
            .vkDevice         = vulkan.device,
            .vkPhysicalDevice = vulkan.physicalDevice,
            .vkDeviceProcAddr = vkGetDeviceProcAddr,
        };

        FfxDevice device = ffxGetDeviceVK(&deviceContext);
        FfxErrorCode backendResult =
            ffxGetInterfaceVK(&cacaoBackendInterface, device, cacaoScratchBuffer, cacaoScratchBufferSize, 1);
        if (backendResult != FFX_OK) {
            utils::error("vulkanAOPass: ffxGetInterfaceVK failed: %d", backendResult);
            free(cacaoScratchBuffer);
            cacaoScratchBuffer     = NULL;
            cacaoScratchBufferSize = 0;
            return 0;
        }

        cacaoBackendReady = 1;
    }

    if (cacaoContextReady && cacaoOutput.img && cacaoWidth == width && cacaoHeight == height) {
        return 1;
    }

    cacaoDestroyContext();
    cacaoDestroyOutput();

    /* CACAO writes rgba16f: .r = final AO (1 = unoccluded). */
    cacaoOutput = vulkanCreateImage(.name   = "CacaoOutput",
                                    .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                    .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                    .width  = (int)width,
                                    .height = (int)height);
    if (!cacaoOutput.img) {
        return 0;
    }

    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanTransition(cmd, &cacaoOutput, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    vulkanTransientEnd(cmd, 1);

    FfxCacaoContextDescription desc = {};
    desc.width              = width;
    desc.height             = height;
    desc.useDownsampledSsao = true; /* cheaper; bilateral 5x5 upscale reconstructs full-res AO */
    desc.backendInterface   = cacaoBackendInterface;

    FfxErrorCode createResult = ffxCacaoContextCreate(&cacaoContext, &desc);
    if (createResult != FFX_OK) {
        utils::error("vulkanAOPass: ffxCacaoContextCreate failed: %d", createResult);
        cacaoDestroyOutput();
        return 0;
    }

    cacaoContextReady = 1;
    cacaoWidth        = width;
    cacaoHeight       = height;
    utils::info("vulkanAOPass: created CACAO context for %ux%u", width, height);
    return 1;
}

static VkImageCreateInfo makeImageCreateInfo(VulkanImage* image) {
    VkImageCreateInfo info = {};
    info.sType             = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType         = VK_IMAGE_TYPE_2D;
    info.format            = image->format;
    info.extent            = image->extent;
    info.mipLevels         = (u32)image->mipLevels;
    info.arrayLayers       = (u32)image->layers;
    info.samples           = image->samples;
    info.tiling            = VK_IMAGE_TILING_OPTIMAL;
    info.usage             = image->usage;
    info.sharingMode       = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    return info;
}

static FfxResource wrapImageResource(VulkanImage* image,
                                     FfxResourceUsage usage,
                                     FfxResourceStates state,
                                     const wchar_t* name) {
    VkImageCreateInfo createInfo = makeImageCreateInfo(image);
    FfxResourceDescription desc  = ffxGetImageResourceDescriptionVK(image->img, createInfo, usage);
    return ffxGetResourceVK(image->img, desc, name, state);
}

static void cacaoUpdate(VulkanCommand* cmd, VulkanImage* depth, VulkanImage* normals, Camera* camera) {
    if (!cacaoEnsureContext(depth->extent.width, depth->extent.height)) {
        return;
    }

    /* Depth/normals were left SHADER_READ_ONLY by the SSR pass; stage them
     * for the compute reads. */
    vulkanTransition(cmd, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, normals, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, &cacaoOutput, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

    FfxCacaoSettings settings = FFX_CACAO_DEFAULT_SETTINGS;
    /* The engine's normal buffer is oct-encoded (.rg), which CACAO's affine
     * unpack (mul/add) cannot decode — reconstruct normals from depth. */
    settings.generateNormals = true;
    /* Rotate/scale the sampling kernel per frame (AMD's TAA recommendation)
     * so the spatial kernel doesn't alias against the jitter sequence. */
    const u32 phase = camera->frameIndex % 3;
    settings.temporalSupersamplingAngleOffset  = (float)phase / 3.0f * 3.14159265f;
    settings.temporalSupersamplingRadiusOffset = 1.0f + (((float)phase - 1.0f) / 3.0f) * 0.1f;
    ffxCacaoUpdateSettings(&cacaoContext, &settings, true);

    /* CACAO derives the depth linearization from the projection matrix
     * (proj[10]/proj[11]); the depth buffer holds standard [0,1] depth. */
    FfxFloat32x4x4 proj          = {};
    FfxFloat32x4x4 normalsToView = {};
    memcpy(proj, camera->cameraUbo.projection, sizeof(proj));
    memcpy(normalsToView, camera->cameraUbo.view, sizeof(normalsToView));

    FfxCacaoDispatchDescription dispatch = {};
    dispatch.commandList  = ffxGetCommandListVK(cmd->cmd);
    dispatch.depthBuffer  = wrapImageResource(depth,
                                              FFX_RESOURCE_USAGE_READ_ONLY,
                                              FFX_RESOURCE_STATE_COMPUTE_READ,
                                              L"cacao_depth");
    dispatch.normalBuffer = wrapImageResource(normals,
                                              FFX_RESOURCE_USAGE_READ_ONLY,
                                              FFX_RESOURCE_STATE_COMPUTE_READ,
                                              L"cacao_normals");
    dispatch.outputBuffer = wrapImageResource(&cacaoOutput,
                                              FFX_RESOURCE_USAGE_UAV,
                                              FFX_RESOURCE_STATE_UNORDERED_ACCESS,
                                              L"cacao_output");
    dispatch.proj            = &proj;
    dispatch.normalsToView   = &normalsToView;
    dispatch.normalUnpackMul = 1.0f;
    dispatch.normalUnpackAdd = 0.0f;

    vulkanBeginProfile(cmd, &cacaoProfile, 0);
    FfxErrorCode result = ffxCacaoContextDispatch(&cacaoContext, &dispatch);
    vulkanEndProfile(cmd, &cacaoProfile, 0);

    if (result != FFX_OK) {
        utils::error("vulkanAOPass: ffxCacaoContextDispatch failed: %d", result);
        cacaoDestroyContext();
        cacaoDestroyOutput();
        return;
    }

    vulkanTransition(cmd, &cacaoOutput, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
}

void VulkanAOPass::update() {
    elapsedCPU = utils::nanos();

    if (vulkan.skipFrame) {
        elapsedCPU = utils::nanos() - elapsedCPU;
        return;
    }

    VulkanCommand* cmd     = vulkan.currentCmd;
    VulkanImage* depth     = vulkanFrameResourcesGetDepth();
    VulkanImage* normals   = vulkanFrameResourcesGetNormals();
    if (!depth || !normals) {
        elapsedCPU = utils::nanos() - elapsedCPU;
        return;
    }

    if (!aoDisabled) {
        Entity* camEntity = cameraGetEntity();
        Camera* camera    = getComponent(camEntity->scene, Camera, camEntity->id);
        if (camera) {
            cacaoUpdate(cmd, depth, normals, camera);
        }
    }
    /* While disabled the composite skips the AO multiply entirely
     * (absent-sentinel index), so no output needs producing. */

    elapsedGPU = cacaoProfileReady ? cacaoProfile.elapsed : 0.0;
    elapsedCPU = utils::nanos() - elapsedCPU;
}

void VulkanAOPass::postUpdate() {
    vulkanAOPass.cpuElapsed = elapsedCPU;
    vulkanAOPass.gpuElapsed = elapsedGPU;
}

void VulkanAOPass::removed() {
    cacaoDestroyContext();
    cacaoDestroyOutput();
    if (cacaoScratchBuffer) {
        free(cacaoScratchBuffer);
        cacaoScratchBuffer     = NULL;
        cacaoScratchBufferSize = 0;
    }
    cacaoBackendInterface = FfxInterface{};
    cacaoBackendReady     = 0;
    if (cacaoProfileReady) {
        vulkanDestroyProfile(&cacaoProfile);
        cacaoProfile      = VulkanProfile{};
        cacaoProfileReady = 0;
    }
}

void vulkanAOPassSetDisabled(char disabled) {
    aoDisabled = disabled;
    utils::info("AO: %s", aoDisabled ? "disabled" : "enabled");
}

char vulkanAOPassIsDisabled(void) {
    return aoDisabled;
}

VulkanImage* vulkanAOPassGetOutput(void) {
    /* CACAO's rgba16f output (.r = AO).  NULL until the context exists
     * (i.e. before the first enabled frame after swapchain creation). */
    return cacaoOutput.img ? &cacaoOutput : NULL;
}
}  // namespace engine