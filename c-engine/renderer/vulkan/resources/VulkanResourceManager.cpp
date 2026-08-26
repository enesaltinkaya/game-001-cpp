#include "VulkanResourceManager.h"
#include "../Vulkan.h"
#include "../utils/VulkanUtils.h"
#include "ecs/components/Skin.h"
#include "ecs/system/window/WindowSystem.h"
#include "renderer/material/Material.h"
#include "renderer/texture/TextureManager.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "Utils.h"
#include "VulkanBuffer.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/light/LightComponent.h"
#include "ecs/Ecs.h"
#include "ecs/system/scene/Scene.h"
#include "renderer/vulkan/scene/VulkanScene.h"
#include "timer/Timer.h"
#include <math.h>

namespace engine {
struct ImageArrayData {
    struct utils::Thread lock = {};
    u32 textureArrayCounter  = 0;
    std::vector<u32> emptySlots = {};
    enum VulkanDescType descType;
    int slot;
};

static ImageArrayData sampledImageArrayData     = {.slot     = SLOT_IMAGE,  //
                                                   .descType = VULKAN_BINDING_SAMPLED_IMAGE,
                                                   .lock     = {.mutex = PTHREAD_MUTEX_INITIALIZER}};
static ImageArrayData sampledCubeImageArrayData = {.slot     = SLOT_CUBE_IMAGE,
                                                   .descType = VULKAN_BINDING_SAMPLED_IMAGE,
                                                   .lock = {.mutex = PTHREAD_MUTEX_INITIALIZER}};
static ImageArrayData storageImageArrayData     = {.slot     = SLOT_STORAGE_IMAGE,
                                                   .descType = VULKAN_BINDING_STORAGE_IMAGE,
                                                   .lock     = {.mutex = PTHREAD_MUTEX_INITIALIZER}};

struct BufferGarbage {
    VkBuffer buffer;
    VmaVirtualBlock virtualBlock;
    VkFence fence;
    std::atomic<bool>* submitted;
    VmaAllocation vma;
};

struct ImageGarbage {
    std::vector<VkImageView> views;
    VkImageView view;
    VkFence fence;
    std::atomic<bool>* submitted;
    VmaAllocation vma;
    VkImage img;
};

struct CommandGarbage {
    VkFence fence;
    std::atomic<bool>* submitted;
    VkCommandPool pool;
    VulkanCommand* cmd;
};

static std::vector<VkFence> fencesToDestroy;
static std::vector<VulkanCommand*> cmdsToFree;
static std::vector<BufferGarbage> buffersToClean;
static std::vector<ImageGarbage> imagesToClean;
static std::vector<CommandGarbage> commandsToClean;
static utils::Thread garbageLock = {.mutex = PTHREAD_MUTEX_INITIALIZER};

struct VulkanPostProcessData {
    u32 depthTextureIndex;
    u32 pad[3];
};

struct VulkanSplatGroup {
    u32 weightTextures[SPLAT_UDIM_TILES];  // 100 UDIM tile texture IDs (0 = empty)
};

struct VulkanTerrainData {
    u32 grassAlbedoIndex;
    u32 grassNormalIndex;
    u32 cliffAlbedoIndex;
    u32 cliffNormalIndex;
    vec4 worldMin;  // terrain AABB min (xyz), w unused
    vec4 worldMax;  // terrain AABB max (xyz), w unused
    // Mirrors TerrainData.climateParams in globalset.shader (std430):
    // x = snowLo (deg C), y = snowHi (deg C), z = beachHeight (m), w = enable.
    float climateParams[4];
    u32 splatGroupCount;
    u32 pomEnabled;
    u32 biomeColorIndex;   // RGBA8 world biome tint
    u32 climateIndex;      // RGBA8 temp/prec/coast/biome field
    u32 snowAlbedoIndex;
    u32 sandAlbedoIndex;
    VulkanSplatGroup splatGroups[MAX_SPLAT_GROUPS];  // UDIM weight textures per group
    u32 _pad[2];
};

struct VulkanCameraOccluderData {
    u32   entityIds[VULKAN_MAX_CAMERA_OCCLUDERS];
    float alphas[VULKAN_MAX_CAMERA_OCCLUDERS];
    u32   count;
    u32   pad[3];
};

/* VulkanWaterData / VulkanAzgaarPropsData are defined in VulkanResourceManager.h */

struct VulkanSceneBuffer {
    CameraUbo cameras[4];
    DirectionalLightUbo directionalLight;
    ShadowUbo shadow;
    int time;
    int prevTime;
    int pad[2];
    ivec4 lightCounts;  // x=directional, y=point, z=spot, w=total
    GpuLight lights[MAX_GPU_LIGHTS];
    VulkanPostProcessData post;
    VulkanTerrainData terrain;
    VulkanFogData     fog;
    VulkanCameraOccluderData cameraOccluders;
    VulkanWaterData water;
    VulkanAzgaarPropsData props;
    VulkanWeatherData weather;
};

struct VulkanAddressBuffer {
    u64 sceneBufferAddress = 0;
    u64 materialBufferAddress = 0;
    u64 lightGridAddress = 0;
    u64 lightIndexAddress = 0;
    u64 jointMatrixBufferAddress = 0;
    u64 entitySkinMapBufferAddress = 0;
    u64 prevJointMatrixBufferAddress = 0;
};

VulkanResources vulkanResources;
static VkSampler samplers[MAX_SAMPLERS];

VkSampler vulkanGetLinearSampler(void) {
    return samplers[SAMPLER_LINEAR];
}

static VulkanBuffer addressBuffer[FRAMES_IN_FLIGHT];
static VulkanBuffer sceneBuffer[FRAMES_IN_FLIGHT];

static VulkanBuffer globalMaterialBuffer;

static u64 lightGridAddress;
static u64 lightIndexAddress;

static void initDummyImage(void);
static void initDummyCubeImage(void);
static void initSamplers(void);
static void updateSamplerMipBias(void);
static void writeSamplers(int i);
static void writeAddressBuffer(int i);
void vulkanCleanupGarbage();
static float currentMipBias;

struct UploadQueue {
    CameraUbo camera;
    DirectionalLightUbo directionalLight;
    ShadowUbo shadow;
    ivec4 lightCounts;
    GpuLight lights[MAX_GPU_LIGHTS];
    u32 lightCount;
    bool hasCamera;
    bool hasDirectionalLight;
    bool hasShadow;
    bool hasLightCounts;
    bool hasLights;
};

static UploadQueue uploadQueue[FRAMES_IN_FLIGHT];

static VulkanFogData fogData = {
    .fogColor            = {0.7f, 0.75f, 0.8f, 0.0f},
    .fogDensity          = 0.04f,
    .fogStartDistance     = 5.0f,
    .fogMaxDistance       = 300.0f,
    .fogType              = 2,
    .fogHeightBase        = 0.0f,
    .fogHeightFalloff     = 0.02f,
    .fogHeightDensity     = 0.05f,
    .fogHeightEnabled     = 1,
    .fogTime              = 0.0f,
    .fogNoiseScale        = 0.015f,
    .fogNoiseStrength     = 0.4f,
    .fogPad               = 0.0f,
};

static VulkanWaterData waterData = {
    .surfaceY              = {0.0f, 1024.0f, 128.0f, 0.0f},  // sea level Y, grid size, grid divs
    .shallowColor          = {0.05f, 0.25f, 0.45f, 5.0f},     // shallow tint, shallow depth (m)
    .deepColor             = {0.01f, 0.05f, 0.15f, 40.0f},    // deep tint, max absorption depth
    .foamColor             = {0.95f, 0.95f, 1.0f, 0.3f},      // foam color, foam threshold
    .waveDirAmp            = {
        {0.8f, 0.6f, 0.4f, 40.0f},
        {0.5f, -0.5f, 0.2f, 20.0f},
        {0.3f, 0.7f, 0.1f, 15.0f},
        {0.9f, 0.1f, 0.15f, 10.0f},
    },
    .waveSpeedSteep       = {
        {1.2f, 0.8f, 0.0f, 0.0f},
        {0.8f, 0.5f, 0.0f, 0.0f},
        {1.5f, 0.3f, 0.0f, 0.0f},
        {2.0f, 0.2f, 0.0f, 0.0f},
    },
    .fresnelPower         = 5.0f,
    .fresnelScale         = 0.6f,
    .normalStrength       = 1.5f,
    .rippleScale          = 0.02f,
    .windAngle            = 0.5f,
    .sunSpecularPower     = 64.0f,
    .sunSpecularIntensity = 1.5f,
    .enabled              = 1.0f,
};

// Azgaar props (instanced vegetation).  Disabled by default (enabled=0) so
// non-Azgaar scenes draw nothing; the game pushes wind/density each frame
// once a world is loaded (workstream B).
static VulkanAzgaarPropsData azgaarPropsData = {
    .wind    = {0.7071f, 0.7071f, 1.0f, 0.15f},  // default wind dir / speed / strength
    .density = {1.0f, 1.0f, 1.0f, 0.0f},         // enabled = 0 (off)
};

// GPU weather particles.  Disabled by default (look.w = 0) so the pass
// early-outs for non-Azgaar scenes; the game (AzgaarWeather) owns and
// cross-fades every field once a world is loaded.
static VulkanWeatherData weatherData = {
    .wind   = {1.0f, 0.0f, 3.5f, 0.5f},                        // dir / m/s / turbulence
    .types  = {0.0f, 0.0f, 0.0f, 0.0f},                        // no spawn types yet
    .params = {90.0f, 30.0f, 0.0f, 1.0f},                      // box / density / size scale
    .look   = {0.0f, 1.0f, 60.0f, 0.0f},                       // opacity / fall / fade / OFF
    .tint   = {1.0f, 1.0f, 1.0f, 0.0f},
};

static void swapchainCreated(void*) {
    updateSamplerMipBias();
}

void vulkanResourceInit(void) {
    initSamplers();
    utils::signalSubscribe("swapchainCreated", swapchainCreated);

    globalMaterialBuffer = vulkanCreateGpuBuffer(
        "globalMaterialBuffer",  //
        (u64)MAX_MATERIALS * sizeof(Material),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    for (i32 i = 0, si = FRAMES_IN_FLIGHT; i < si; i++) {
        vulkanResources.globalSet0[i] = vulkanCreateDesc(.name          = "globalDescriptor",  //
                                                         .samplers      = MAX_SAMPLERS,
                                                         .sampledImages = MAX_IMAGES,
                                                         .sampledCubeImages = MAX_IMAGES,
                                                         .storageImages     = MAX_IMAGES,
                                                         .ubos = 1 /* address buffer */);

        writeSamplers(i);
        writeAddressBuffer(i);
    }

    initDummyImage();
    initDummyCubeImage();
}

void vulkanResourceDestroy(void) {
    for (i32 i = 0, si = MAX_SAMPLERS; i < si; i++) {
        vkDestroySampler(vulkan.device, samplers[i], nullptr);
    }

    vulkanDestroyBuffer(&globalMaterialBuffer, nullptr);

    for (i32 i = 0, si = FRAMES_IN_FLIGHT; i < si; i++) {
        vulkanDestroyBuffer(&addressBuffer[i], nullptr);
        vulkanDestroyBuffer(&sceneBuffer[i], nullptr);
        vulkanDestroyDesc(&vulkanResources.globalSet0[i]);
    }
    vulkanDestroyImage(&vulkanResources.dummyImage, nullptr);
    vulkanDestroyImage(&vulkanResources.dummyCubeImage, nullptr);


    vulkanCleanupGarbage();
}

void writeSamplers(int i) {
    struct VulkanDesc* desc = &vulkanResources.globalSet0[i];
    vulkanUpdateDesc(desc,
                     VULKAN_BINDING_SAMPLER,
                     samplers[SAMPLER_NEAREST],
                     SLOT_SAMPLER,
                     SAMPLER_NEAREST);
    vulkanUpdateDesc(desc,
                     VULKAN_BINDING_SAMPLER,
                     samplers[SAMPLER_LINEAR],
                     SLOT_SAMPLER,
                     SAMPLER_LINEAR);
    vulkanUpdateDesc(desc,
                     VULKAN_BINDING_SAMPLER,
                     samplers[SAMPLER_NORMALMAP],
                     SLOT_SAMPLER,
                     SAMPLER_NORMALMAP);
    vulkanUpdateDesc(desc,
                     VULKAN_BINDING_SAMPLER,
                     samplers[SAMPLER_SPLATMAP],
                     SLOT_SAMPLER,
                     SAMPLER_SPLATMAP);
    vulkanUpdateDesc(desc,
                     VULKAN_BINDING_SAMPLER,
                     samplers[SAMPLER_BORDER],
                     SLOT_SAMPLER,
                     SAMPLER_BORDER);
    vulkanUpdateDesc(desc,
                     VULKAN_BINDING_SAMPLER,
                     samplers[SAMPLER_BORDER_NEAREST],
                     SLOT_SAMPLER,
                     SAMPLER_BORDER_NEAREST);
    vulkanUpdateDesc(desc,
                     VULKAN_BINDING_SAMPLER,
                     samplers[SAMPLER_SHADOW_CMP],
                     SLOT_SAMPLER,
                     SAMPLER_SHADOW_CMP);
    vulkanUpdateDesc(desc,
                     VULKAN_BINDING_SAMPLER,
                     samplers[SAMPLER_CLAMP_LINEAR],
                     SLOT_SAMPLER,
                     SAMPLER_CLAMP_LINEAR);
    vulkanUpdateDesc(desc,
                     VULKAN_BINDING_SAMPLER,
                     samplers[SAMPLER_CLAMP_NEAREST],
                     SLOT_SAMPLER,
                     SAMPLER_CLAMP_NEAREST);
    vulkanUpdateDesc(desc,
                     VULKAN_BINDING_SAMPLER,
                     samplers[SAMPLER_CLAMP_LINEAR_MIPMAP],
                     SLOT_SAMPLER,
                     SAMPLER_CLAMP_LINEAR_MIPMAP);
}

void writeAddressBuffer(int i) {
    sceneBuffer[i] = vulkanCreateCpuBuffer(
        utils::strtmp("sceneBuffer %i", i),
        sizeof(VulkanSceneBuffer),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    // Zero-initialize to ensure terrain splatGroups and other optional fields
    // start as clean zeros rather than VMA allocator garbage.
    memset(sceneBuffer[i].vmaInfo.pMappedData, 0, sizeof(VulkanSceneBuffer));

    VulkanDesc* desc = &vulkanResources.globalSet0[i];
    char* name       = utils::strtmp("addressBuffer %i", i);
    addressBuffer[i] = vulkanCreateCpuBuffer(name,
                                             sizeof(VulkanAddressBuffer),
                                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

    VulkanAddressBuffer bufferAddresses = {
        .sceneBufferAddress    = sceneBuffer[i].address,
        .materialBufferAddress = globalMaterialBuffer.address,
        .lightGridAddress      = lightGridAddress,
        .lightIndexAddress     = lightIndexAddress,
    };

    // Wire skinning addresses from the first scene that has skinning buffers
    for (size_t si = 0; si < ecs.scenes.size(); si++) {
        Scene* s = ecs.scenes[si];
        if (!s || !s->backendScene) continue;
        VulkanScene* vs  = static_cast<VulkanScene*>(s->backendScene);
        if (vs->totalSkinJointCount > 0) {
            bufferAddresses.jointMatrixBufferAddress = vs->jointMatrixBuffer[i].address;
            bufferAddresses.entitySkinMapBufferAddress = vs->entitySkinMapBuffer[i].address;
            bufferAddresses.prevJointMatrixBufferAddress = vs->prevJointMatrixBuffer[i].address;
            break;
        }
    }

    vulkanCopy(.target.buf  = &addressBuffer[i],
               .source.data = &bufferAddresses,
               .size        = sizeof(bufferAddresses));

    vulkanUpdateDesc(desc, VULKAN_BINDING_UBO, &addressBuffer[i], SLOT_ADDRESS_BUFFER, 0);
}

void vulkanResourceUpdate(void) {
    vulkanCleanupGarbage();

    int flightIndex          = renderer.flightIndex;
    VulkanSceneBuffer* scene  = static_cast<VulkanSceneBuffer*>(sceneBuffer[flightIndex].vmaInfo.pMappedData);
    static int lastTimeMs    = 0;
    int currentTimeMs        = (int)(utils::timer.timeSinceStart / 1000000.0);  // nanos -> milliseconds
    scene->prevTime          = lastTimeMs;
    scene->time              = currentTimeMs;
    lastTimeMs               = currentTimeMs;

    // Update fog time and upload fog data every frame
    fogData.fogTime          = (float)currentTimeMs / 1000.0f;
    scene->fog               = fogData;

    // Upload water params every frame
    scene->water             = waterData;

    // Upload Azgaar props params every frame
    scene->props = azgaarPropsData;

    // Upload weather params every frame
    scene->weather = weatherData;

    UploadQueue* queue = &uploadQueue[flightIndex];
    if (queue->hasCamera) {
        memcpy(&scene->cameras[0], &queue->camera, sizeof(CameraUbo));
        // scene->cameras[0] = uq->camera;
        queue->hasCamera = 0;
    }
    if (queue->hasDirectionalLight) {
        scene->directionalLight    = queue->directionalLight;
        queue->hasDirectionalLight = 0;
    }
    if (queue->hasShadow) {
        scene->shadow    = queue->shadow;
        queue->hasShadow = 0;
    }
    if (queue->hasLightCounts) {
        glm_ivec4_copy(queue->lightCounts, scene->lightCounts);
        queue->hasLightCounts = 0;
    }
    if (queue->hasLights) {
        memcpy(scene->lights, queue->lights, sizeof(GpuLight) * queue->lightCount);
        queue->hasLights = 0;
    }
    // Flush transforms for ALL scenes (keeps upload queues drained even
    // when a scene is frustum-culled, preventing unbounded memory growth).
    for (size_t si = 0; si < ecs.scenes.size(); si++) {
        Scene* s = ecs.scenes[si];
        if (!s->backendScene) continue;
        VulkanScene* vs  = static_cast<VulkanScene*>(s->backendScene);
        if (!vs->totalDraws) continue;
        vulkanSceneFlushTransforms(vs);
        vulkanSceneFlushJoints(s);
    }
}

void vulkanResourceUploadCamera(Camera* camera) {
    if (!camera) return;
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        uploadQueue[i].camera    = camera->cameraUbo;
        uploadQueue[i].hasCamera = 1;
    }
}

void vulkanResourceUploadDirectionalLight(DirectionalLightUbo* directionalLight) {
    if (!directionalLight) return;
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        uploadQueue[i].directionalLight    = *directionalLight;
        uploadQueue[i].hasDirectionalLight = 1;
    }
}

void vulkanResourceUploadShadow(ShadowUbo* shadow) {
    if (!shadow) return;
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        uploadQueue[i].shadow    = *shadow;
        uploadQueue[i].hasShadow = 1;
    }
    /* Also patch the current frame directly so that changes made during
     * rendering (after vulkanResourceUpdate) are visible immediately. */
    int fi                 = renderer.flightIndex;
    VulkanSceneBuffer* buf  = static_cast<VulkanSceneBuffer*>(sceneBuffer[fi].vmaInfo.pMappedData);
    if (buf) {
        buf->shadow = *shadow;
    }
}


void vulkanResourceSetContactShadowImageIndex(u32 index) {
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VulkanSceneBuffer* buf  = static_cast<VulkanSceneBuffer*>(sceneBuffer[i].vmaInfo.pMappedData);
        if (buf) {
            buf->shadow.contactShadowImageIndex = index;
        }
    }
}

void vulkanResourceSetShadowMaskImageIndex(u32 index) {
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VulkanSceneBuffer* buf  = static_cast<VulkanSceneBuffer*>(sceneBuffer[i].vmaInfo.pMappedData);
        if (buf) {
            buf->shadow.shadowMaskImageIndex = index;
        }
    }
}

void vulkanResourceUploadLight(GpuLight* light, u32 index) {
    (void)light;
    (void)index;
}

void vulkanResourceUploadLights(const GpuLight* lights, u32 count) {
    if (!lights || count == 0) return;
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (count > MAX_GPU_LIGHTS) count = MAX_GPU_LIGHTS;
        memcpy(uploadQueue[i].lights, lights, count * sizeof(GpuLight));
        uploadQueue[i].lightCount = count;
        uploadQueue[i].hasLights = 1;
    }
}

void vulkanResourceUploadLightUbo(const LightUbo* lightUbo) {
    if (!lightUbo) return;
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        uploadQueue[i].lightCounts[0] = lightUbo->counts[0];
        uploadQueue[i].lightCounts[1] = lightUbo->counts[1];
        uploadQueue[i].lightCounts[2] = lightUbo->counts[2];
        uploadQueue[i].lightCounts[3] = lightUbo->counts[3];
        uploadQueue[i].hasLightCounts = 1;
        u32 count = (u32)lightUbo->counts[3];
        if (count > MAX_GPU_LIGHTS) count = MAX_GPU_LIGHTS;
        memcpy(uploadQueue[i].lights, lightUbo->lights, count * sizeof(GpuLight));
        uploadQueue[i].lightCount = count;
        uploadQueue[i].hasLights = 1;
    }
}

void vulkanResourceUploadLightCounts(ivec4 counts) {
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        glm_ivec4_copy(counts, uploadQueue[i].lightCounts);
        uploadQueue[i].hasLightCounts = 1;
    }
}

void vulkanResourceUploadMaterial(Material* material) {
    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanCopy(.cmd                 = cmd,
               .target.buf          = &globalMaterialBuffer,
               .target.bufferOffset = static_cast<u32>(sizeof(Material) * material->id),
               .source.data         = material,
               .size                = sizeof(Material));
    vulkanTransientEnd(cmd, 0);
}

void initSamplers(void) {
    VkSamplerCreateInfo samplerInfo     = {};
    samplerInfo.sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.flags                   = 0;
    samplerInfo.pNext                   = 0;
    samplerInfo.minFilter               = VK_FILTER_NEAREST;
    samplerInfo.magFilter               = VK_FILTER_NEAREST;
    samplerInfo.addressModeU            = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV            = samplerInfo.addressModeU;
    samplerInfo.addressModeW            = samplerInfo.addressModeU;
    samplerInfo.borderColor             = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    samplerInfo.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.minLod                  = 0;
    samplerInfo.maxLod                  = VK_LOD_CLAMP_NONE;
    samplerInfo.compareOp               = VK_COMPARE_OP_NEVER;
    samplerInfo.compareEnable           = 0;
    samplerInfo.anisotropyEnable        = 1;
    samplerInfo.maxAnisotropy           = 16;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.mipLodBias              = 0.0F;
    vkCreateSampler(vulkan.device, &samplerInfo, NULL, &samplers[SAMPLER_NEAREST]);
    if (utils::isDebug()) {
        vulkanUtilsSetName((u64)samplers[SAMPLER_NEAREST],
                           VK_OBJECT_TYPE_SAMPLER,
                           "sampler nearest");
    }
    samplerInfo.minFilter               = VK_FILTER_LINEAR;
    samplerInfo.magFilter               = VK_FILTER_LINEAR;
    samplerInfo.addressModeU            = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV            = samplerInfo.addressModeU;
    samplerInfo.addressModeW            = samplerInfo.addressModeU;
    samplerInfo.borderColor             = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    samplerInfo.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.minLod                  = 0;
    samplerInfo.maxLod                  = VK_LOD_CLAMP_NONE;
    samplerInfo.compareOp               = VK_COMPARE_OP_NEVER;
    samplerInfo.compareEnable           = 0;
    samplerInfo.anisotropyEnable        = 1;
    samplerInfo.maxAnisotropy           = 16;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.mipLodBias              = 0.0F;
    vkCreateSampler(vulkan.device, &samplerInfo, NULL, &samplers[SAMPLER_LINEAR]);
    if (utils::isDebug()) {
        vulkanUtilsSetName((u64)samplers[SAMPLER_LINEAR], VK_OBJECT_TYPE_SAMPLER, "sampler linear");
    }

    samplerInfo.minFilter    = VK_FILTER_LINEAR;
    samplerInfo.magFilter    = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = samplerInfo.addressModeU;
    samplerInfo.addressModeW = samplerInfo.addressModeU;
    // samplerInfo.mipLodBias   = 0.25F;
    vkCreateSampler(vulkan.device, &samplerInfo, NULL, &samplers[SAMPLER_NORMALMAP]);
    if (utils::isDebug()) {
        vulkanUtilsSetName((u64)samplers[SAMPLER_NORMALMAP],
                           VK_OBJECT_TYPE_SAMPLER,
                           "sampler normalmap");
    }

    samplerInfo.minFilter               = VK_FILTER_LINEAR;
    samplerInfo.magFilter               = VK_FILTER_LINEAR;
    samplerInfo.addressModeU            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.borderColor             = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    samplerInfo.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.minLod                  = 0;
    samplerInfo.maxLod                  = 0;
    samplerInfo.compareOp               = VK_COMPARE_OP_NEVER;
    samplerInfo.compareEnable           = 0;
    samplerInfo.anisotropyEnable        = 0;
    samplerInfo.maxAnisotropy           = 1;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.mipLodBias              = 0.0F;
    vkCreateSampler(vulkan.device, &samplerInfo, NULL, &samplers[SAMPLER_BORDER]);
    if (utils::isDebug()) {
        vulkanUtilsSetName((u64)samplers[SAMPLER_BORDER], VK_OBJECT_TYPE_SAMPLER, "sampler border");
    }

    samplerInfo.minFilter               = VK_FILTER_NEAREST;
    samplerInfo.magFilter               = VK_FILTER_NEAREST;
    samplerInfo.addressModeU            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.borderColor             = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    samplerInfo.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.minLod                  = 0;
    samplerInfo.maxLod                  = 0;
    samplerInfo.compareOp               = VK_COMPARE_OP_NEVER;
    samplerInfo.compareEnable           = 0;
    samplerInfo.anisotropyEnable        = 0;
    samplerInfo.maxAnisotropy           = 1;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.mipLodBias              = 0.0F;
    vkCreateSampler(vulkan.device, &samplerInfo, NULL, &samplers[SAMPLER_BORDER_NEAREST]);
    if (utils::isDebug()) {
        vulkanUtilsSetName((u64)samplers[SAMPLER_BORDER_NEAREST],
                           VK_OBJECT_TYPE_SAMPLER,
                           "sampler border nearest");
    }

    /* Shadow comparison sampler: bilinear filtering with depth compare.
     * When used with sampler2DShadow, the GPU performs 4 depth comparisons
     * and bilinearly interpolates the results — free hardware PCF. */
    samplerInfo.minFilter               = VK_FILTER_LINEAR;
    samplerInfo.magFilter               = VK_FILTER_LINEAR;
    samplerInfo.addressModeU            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.borderColor             = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    samplerInfo.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.minLod                  = 0;
    samplerInfo.maxLod                  = 0;
    samplerInfo.compareEnable           = VK_TRUE;
    samplerInfo.compareOp               = VK_COMPARE_OP_LESS_OR_EQUAL;
    samplerInfo.anisotropyEnable        = 0;
    samplerInfo.maxAnisotropy           = 1;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.mipLodBias              = 0.0F;
    vkCreateSampler(vulkan.device, &samplerInfo, NULL, &samplers[SAMPLER_SHADOW_CMP]);
    if (utils::isDebug()) {
        vulkanUtilsSetName((u64)samplers[SAMPLER_SHADOW_CMP],
                           VK_OBJECT_TYPE_SAMPLER,
                           "sampler shadow compare");
    }

    samplerInfo.compareEnable = 0;
    samplerInfo.compareOp     = VK_COMPARE_OP_NEVER;

    samplerInfo.minFilter               = VK_FILTER_LINEAR;
    samplerInfo.magFilter               = VK_FILTER_LINEAR;
    samplerInfo.addressModeU            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.borderColor             = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    samplerInfo.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.minLod                  = 0;
    samplerInfo.maxLod                  = VK_LOD_CLAMP_NONE;
    samplerInfo.anisotropyEnable        = VK_TRUE;
    samplerInfo.maxAnisotropy           = 16;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.mipLodBias              = 0;

    vkCreateSampler(vulkan.device, &samplerInfo, NULL, &samplers[SAMPLER_SPLATMAP]);
    if (utils::isDebug()) {
        vulkanUtilsSetName((u64)samplers[SAMPLER_SPLATMAP],
                           VK_OBJECT_TYPE_SAMPLER,
                           "sampler splatmap");
    }

    samplerInfo.minFilter               = VK_FILTER_LINEAR;
    samplerInfo.magFilter               = VK_FILTER_LINEAR;
    samplerInfo.addressModeU            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.borderColor             = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    samplerInfo.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.minLod                  = 0;
    samplerInfo.maxLod                  = 0;
    samplerInfo.compareOp               = VK_COMPARE_OP_NEVER;
    samplerInfo.compareEnable           = 0;
    samplerInfo.anisotropyEnable        = 0;
    samplerInfo.maxAnisotropy           = 1;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.mipLodBias              = 0.0F;
    vkCreateSampler(vulkan.device, &samplerInfo, NULL, &samplers[SAMPLER_CLAMP_LINEAR]);
    if (utils::isDebug()) {
        vulkanUtilsSetName((u64)samplers[SAMPLER_CLAMP_LINEAR],
                           VK_OBJECT_TYPE_SAMPLER,
                           "sampler clamp linear");
    }

    samplerInfo.minFilter               = VK_FILTER_NEAREST;
    samplerInfo.magFilter               = VK_FILTER_NEAREST;
    samplerInfo.addressModeU            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.borderColor             = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    samplerInfo.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.minLod                  = 0;
    samplerInfo.maxLod                  = VK_LOD_CLAMP_NONE;
    samplerInfo.compareOp               = VK_COMPARE_OP_NEVER;
    samplerInfo.compareEnable           = 0;
    samplerInfo.anisotropyEnable        = 1;
    samplerInfo.maxAnisotropy           = 16;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.mipLodBias              = 0.0F;
    vkCreateSampler(vulkan.device, &samplerInfo, NULL, &samplers[SAMPLER_CLAMP_NEAREST]);
    if (utils::isDebug()) {
        vulkanUtilsSetName((u64)samplers[SAMPLER_CLAMP_NEAREST],
                           VK_OBJECT_TYPE_SAMPLER,
                           "sampler clamp nearest");
    }

    /* SAMPLER_CLAMP_LINEAR_MIPMAP — clamp-to-edge, linear, trilinear mipmaps */
    samplerInfo.minFilter               = VK_FILTER_LINEAR;
    samplerInfo.magFilter               = VK_FILTER_LINEAR;
    samplerInfo.addressModeU            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.borderColor             = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    samplerInfo.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.minLod                  = 0;
    samplerInfo.maxLod                  = VK_LOD_CLAMP_NONE;
    samplerInfo.compareOp               = VK_COMPARE_OP_NEVER;
    samplerInfo.compareEnable           = 0;
    samplerInfo.anisotropyEnable        = 0;
    samplerInfo.maxAnisotropy           = 1;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.mipLodBias              = 0.0F;
    vkCreateSampler(vulkan.device, &samplerInfo, NULL, &samplers[SAMPLER_CLAMP_LINEAR_MIPMAP]);
    if (utils::isDebug()) {
        vulkanUtilsSetName((u64)samplers[SAMPLER_CLAMP_LINEAR_MIPMAP],
                           VK_OBJECT_TYPE_SAMPLER,
                           "sampler clamp linear mipmap");
    }
}

/* ── FSR mip LOD bias ─────────────────────────────────────────────────
 *
 * When FSR renders at a lower resolution the GPU's implicit LOD picks
 * higher (blurrier) mip levels than needed for the display resolution.
 * Each sub-pixel jitter offset shifts the LOD slightly, so the texture
 * content flickers between mip levels frame-to-frame.  FSR can't
 * converge on unstable input → visible ghosting, especially on
 * high-frequency terrain textures (512× tiling).
 *
 * Fix: apply a negative bias  log2(renderWidth / displayWidth)  to
 * every sampler that touches material / terrain textures.  This forces
 * the GPU to sample from the mip level appropriate for the display
 * resolution, giving FSR temporally stable input to accumulate. */

static void recreateSamplerWithBias(u32 index,
                                    float bias,
                                    VkFilter minFilter,
                                    VkFilter magFilter,
                                    VkSamplerAddressMode addressMode,
                                    VkSamplerMipmapMode mipmapMode,
                                    VkBool32 anisotropy,
                                    float maxAniso,
                                    const char* debugName) {
    vkDestroySampler(vulkan.device, samplers[index], nullptr);

    VkSamplerCreateInfo ci = {};
    ci.sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter        = magFilter;
    ci.minFilter        = minFilter;
    ci.mipmapMode       = mipmapMode;
    ci.addressModeU     = addressMode;
    ci.addressModeV     = addressMode;
    ci.addressModeW     = addressMode;
    ci.mipLodBias       = bias;
    ci.anisotropyEnable = anisotropy;
    ci.maxAnisotropy    = maxAniso;
    ci.minLod           = 0.0f;
    ci.maxLod           = VK_LOD_CLAMP_NONE;
    ci.borderColor      = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    vkCreateSampler(vulkan.device, &ci, NULL, &samplers[index]);
    if (utils::isDebug()) {
        vulkanUtilsSetName((u64)samplers[index], VK_OBJECT_TYPE_SAMPLER, debugName);
    }
}

static void updateSamplerMipBias(void) {
    float bias = 0.0f;
    if (rendererIsUpscalerEnabled() && window.renderWidth > 0 && window.width > 0 &&
        window.renderWidth < window.width) {
        bias = log2f((float)window.renderWidth / (float)window.width);
    }

    /* Skip if nothing changed (avoid GPU stall from sampler recreation). */
    if (fabsf(bias - currentMipBias) < 0.001f) {
        return;
    }
    currentMipBias = bias;

    vkDeviceWaitIdle(vulkan.device);

    /* SAMPLER_LINEAR — main material sampler (repeat, trilinear, 16× AF) */
    recreateSamplerWithBias(SAMPLER_LINEAR,
                            bias,
                            VK_FILTER_LINEAR,
                            VK_FILTER_LINEAR,
                            VK_SAMPLER_ADDRESS_MODE_REPEAT,
                            VK_SAMPLER_MIPMAP_MODE_LINEAR,
                            VK_TRUE,
                            16.0f,
                            "sampler linear");

    /* SAMPLER_NORMALMAP — normal map sampler (repeat, trilinear, 16× AF) */
    recreateSamplerWithBias(SAMPLER_NORMALMAP,
                            bias,
                            VK_FILTER_LINEAR,
                            VK_FILTER_LINEAR,
                            VK_SAMPLER_ADDRESS_MODE_REPEAT,
                            VK_SAMPLER_MIPMAP_MODE_LINEAR,
                            VK_TRUE,
                            16.0f,
                            "sampler normalmap");

    /* SAMPLER_SPLATMAP — terrain splat weights (clamp-to-edge, trilinear, 16× AF) */
    recreateSamplerWithBias(SAMPLER_SPLATMAP,
                            bias,
                            VK_FILTER_LINEAR,
                            VK_FILTER_LINEAR,
                            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                            VK_SAMPLER_MIPMAP_MODE_LINEAR,
                            VK_TRUE,
                            16.0f,
                            "sampler splatmap");

    /* Re-write sampler descriptors for all in-flight frames. */
    for (i32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        writeSamplers(i);
    }

    utils::info("vulkanResources: FSR mip LOD bias updated to %.3f", (double)bias);
}

void initDummyImage(void) {
    vulkanResources.dummyImage = vulkanCreateImage(.name   = "dummy",
                                                   .format = VK_FORMAT_R8G8B8A8_SRGB,
                                                   .width  = 1,
                                                   .height = 1);
    unsigned char data[4]      = {150, 150, 150, 255};

    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanTransition(cmd, &vulkanResources.dummyImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);
    vulkanCopy(.cmd         = cmd,
               .target.img  = &vulkanResources.dummyImage,
               .source.data = data,
               .size        = sizeof(data));
    vulkanTransition(cmd,
                     &vulkanResources.dummyImage,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     0,
                     1);

    vulkanTransientEnd(cmd, 1);
}

void initDummyCubeImage(void) {
    vulkanResources.dummyCubeImage =
        vulkanCreateImage(.name     = "dummy_cube",
                          .format   = VK_FORMAT_R8G8B8A8_UNORM,
                          .usage    = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          .width    = 1,
                          .height   = 1,
                          .layers   = 6,
                          .flags    = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                          .viewType = VK_IMAGE_VIEW_TYPE_CUBE);

    unsigned char data[4] = {150, 150, 150, 255};

    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanTransition(cmd,
                     &vulkanResources.dummyCubeImage,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     0,
                     1);
    for (u32 face = 0; face < 6; face++) {
        vulkanCopy(.cmd          = cmd,
                   .target.img   = &vulkanResources.dummyCubeImage,
                   .target.layer = face,
                   .source.data  = data,
                   .size         = sizeof(data));
    }
    vulkanTransition(cmd,
                     &vulkanResources.dummyCubeImage,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     0,
                     1);

    vulkanTransientEnd(cmd, 1);
}

VulkanCommand* vulkanTransientBegin(void) {
    VulkanCommand* cmd = new VulkanCommand{};
    cmd->transient     = 1;

    // create a new pool
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType                   = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags                   = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex        = vulkan.graphicsFamilyIndex;
    vkCreateCommandPool(vulkan.device, &poolInfo, NULL, &cmd->pool);

    // allocate one command buffer
    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType                       = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool                 = cmd->pool;
    allocInfo.level                       = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount          = 1;
    vkAllocateCommandBuffers(vulkan.device, &allocInfo, &cmd->cmd);

    // crate a fence to track "job finish"
    VkFenceCreateInfo fenceCreateInfo = {};
    fenceCreateInfo.sType             = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(vulkan.device, &fenceCreateInfo, NULL, &cmd->fence);

    if (utils::isDebug()) {
        vulkanUtilsSetName((u64)cmd->cmd, VK_OBJECT_TYPE_COMMAND_BUFFER, "transient cmd buffer");
        vulkanUtilsSetName((u64)cmd->pool, VK_OBJECT_TYPE_COMMAND_POOL, "transient cmd pool");
        vulkanUtilsSetName((u64)cmd->fence, VK_OBJECT_TYPE_FENCE, "transient fence");
    }

    vulkanBegin(cmd);
    return cmd;
}

void vulkanTransientEnd(VulkanCommand* cmd, bool wait) {
    vulkanEnd(cmd);
    vulkanSubmit(.cmd = cmd);
    if (wait) vulkanFenceWait(cmd);
    addCommandGarbage(cmd);
}

void vulkanTransientEndAsync(VulkanCommand* cmd) {
    assert(cmd && cmd->transient);
    vulkanEnd(cmd);
    vulkanSubmit(.cmd = cmd);
    // Ownership of pool/fence/struct stays with the caller: it polls
    // vkGetFenceStatus(cmd->fence) and calls vulkanTransientFinish() once
    // the copy is done (or at teardown).
}

void vulkanTransientFinish(VulkanCommand* cmd) {
    if (!cmd) return;
    vulkanFenceWait(cmd);  // no-op when the fence is already signaled
    // Ownership (pool, fence, struct — and any staging buffers registered
    // against this fence) passes to the garbage collector, which reaps them
    // in its usual fence-checked order.
    addCommandGarbage(cmd);
}

static void addToPool(ImageArrayData* imageArrayData, VulkanImage* image) {
    utils::threadLock(&imageArrayData->lock);

    u32 newIndex = 0;
    if (!imageArrayData->emptySlots.empty()) {
        newIndex = imageArrayData->emptySlots.back();
        imageArrayData->emptySlots.pop_back();
    } else {
        newIndex = imageArrayData->textureArrayCounter;
        imageArrayData->textureArrayCounter++;
    }

    if (imageArrayData->descType == VULKAN_BINDING_SAMPLED_IMAGE) {
        image->sampledPoolIndex = newIndex;
    } else if (imageArrayData->descType == VULKAN_BINDING_STORAGE_IMAGE) {
        image->storagePoolIndex = newIndex;
    }

    for (i32 i = 0, si = FRAMES_IN_FLIGHT; i < si; i++) {
        VulkanDesc* desc = &vulkanResources.globalSet0[i];
        vulkanUpdateDesc(desc, imageArrayData->descType, image, imageArrayData->slot, newIndex);
    }

    utils::threadUnlock(&imageArrayData->lock);
}

void vulkanAddImageToPool(VulkanImage* image) {
    bool hasSampled = (image->usage & VK_IMAGE_USAGE_SAMPLED_BIT);
    bool hasStorage = (image->usage & VK_IMAGE_USAGE_STORAGE_BIT);

    if (hasSampled && hasStorage) {
        // Image has both SAMPLED and STORAGE usage. Register at the same
        // index in both pools so compute shaders (imageStore/storage) and
        // fragment shaders (texture/sampled) use matching descriptor slots.
        ImageArrayData* sampledArray = image->viewType == VK_IMAGE_VIEW_TYPE_CUBE
                                            ? &sampledCubeImageArrayData
                                            : &sampledImageArrayData;

        utils::threadLock(&sampledArray->lock);
        utils::threadLock(&storageImageArrayData.lock);

        // Ensure both counters are at the same value. Use the higher one
        // so we don't overwrite existing indices in either pool.
        u32 sampledCounter = sampledArray->textureArrayCounter;
        u32 storageCounter = storageImageArrayData.textureArrayCounter;
        u32 aligned = (sampledCounter > storageCounter) ? sampledCounter : storageCounter;

        // Advance both counters, filling empty slots for each skipped index
        while (sampledCounter < aligned) {
            sampledArray->emptySlots.push_back(sampledCounter);
            sampledCounter++;
        }
        while (storageCounter < aligned) {
            storageImageArrayData.emptySlots.push_back(storageCounter);
            storageCounter++;
        }

        // Register at the aligned index in both pools
        image->sampledPoolIndex = aligned;
        image->storagePoolIndex = aligned;

        sampledArray->textureArrayCounter = aligned + 1;
        storageImageArrayData.textureArrayCounter = aligned + 1;

        for (i32 i = 0, si = FRAMES_IN_FLIGHT; i < si; i++) {
            VulkanDesc* desc = &vulkanResources.globalSet0[i];
            vulkanUpdateDesc(desc, sampledArray->descType, image,
                             sampledArray->slot, image->sampledPoolIndex);
            vulkanUpdateDesc(desc, storageImageArrayData.descType, image,
                             storageImageArrayData.slot, image->storagePoolIndex);
        }

        utils::threadUnlock(&storageImageArrayData.lock);
        utils::threadUnlock(&sampledArray->lock);

        image->inPool = 1;
        return;
    }

    if (hasSampled) {
        ImageArrayData* sampledArray = image->viewType == VK_IMAGE_VIEW_TYPE_CUBE
                                            ? &sampledCubeImageArrayData
                                            : &sampledImageArrayData;
        addToPool(sampledArray, image);
    }

    if (hasStorage) {
        addToPool(&storageImageArrayData, image);
    }

    image->inPool = 1;
}

void vulkanRemoveImageFromPool(VulkanImage* image) {
    if (!image->inPool) {
        return;
    }

    if (image->usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
        ImageArrayData* sampledArray = image->viewType == VK_IMAGE_VIEW_TYPE_CUBE
                                           ? &sampledCubeImageArrayData
                                           : &sampledImageArrayData;
        utils::threadLock(&sampledArray->lock);
        sampledArray->emptySlots.push_back(image->sampledPoolIndex);
        utils::threadUnlock(&sampledArray->lock);
    }

    if (image->usage & VK_IMAGE_USAGE_STORAGE_BIT) {
        utils::threadLock(&storageImageArrayData.lock);
        storageImageArrayData.emptySlots.push_back(image->storagePoolIndex);
        utils::threadUnlock(&storageImageArrayData.lock);
    }

    image->inPool = 0;
}

int vulkanAddImageViewToPool(VkImageView view) {
    utils::threadLock(&sampledImageArrayData.lock);

    u32 newIndex = 0;
    if (!sampledImageArrayData.emptySlots.empty()) {
        newIndex = sampledImageArrayData.emptySlots.back();
        sampledImageArrayData.emptySlots.pop_back();
    } else {
        newIndex = sampledImageArrayData.textureArrayCounter;
        sampledImageArrayData.textureArrayCounter++;
    }

    for (i32 i = 0, si = FRAMES_IN_FLIGHT; i < si; i++) {
        VulkanDesc* desc = &vulkanResources.globalSet0[i];

        utils::threadLock(&desc->lock);
        VkDescriptorImageInfo descriptorImageInfo = {};
        descriptorImageInfo.imageView             = view;
        descriptorImageInfo.imageLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writeDescriptor = {};
        writeDescriptor.sType                = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDescriptor.descriptorCount      = 1;
        writeDescriptor.pImageInfo           = &descriptorImageInfo;
        writeDescriptor.dstSet               = desc->set;
        writeDescriptor.descriptorType       = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writeDescriptor.dstBinding           = SLOT_IMAGE;
        writeDescriptor.dstArrayElement      = newIndex;
        vkUpdateDescriptorSets(vulkan.device, 1, &writeDescriptor, 0, 0);
        utils::threadUnlock(&desc->lock);
    }

    utils::threadUnlock(&sampledImageArrayData.lock);
    return newIndex;
}

void vulkanRemoveImageViewFromPool(int poolIndex) {
    utils::threadLock(&sampledImageArrayData.lock);
    sampledImageArrayData.emptySlots.push_back((u32)poolIndex);
    utils::threadUnlock(&sampledImageArrayData.lock);
}

int vulkanAddStorageImageViewToPool(VkImageView view) {
    utils::threadLock(&storageImageArrayData.lock);

    u32 newIndex = 0;
    if (!storageImageArrayData.emptySlots.empty()) {
        newIndex = storageImageArrayData.emptySlots.back();
        storageImageArrayData.emptySlots.pop_back();
    } else {
        newIndex = storageImageArrayData.textureArrayCounter;
        storageImageArrayData.textureArrayCounter++;
    }

    for (i32 i = 0, si = FRAMES_IN_FLIGHT; i < si; i++) {
        VulkanDesc* desc = &vulkanResources.globalSet0[i];

        utils::threadLock(&desc->lock);
        VkDescriptorImageInfo descriptorImageInfo = {};
        descriptorImageInfo.imageView             = view;
        descriptorImageInfo.imageLayout           = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writeDescriptor = {};
        writeDescriptor.sType                = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDescriptor.descriptorCount      = 1;
        writeDescriptor.pImageInfo           = &descriptorImageInfo;
        writeDescriptor.dstSet               = desc->set;
        writeDescriptor.descriptorType       = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writeDescriptor.dstBinding           = SLOT_STORAGE_IMAGE;
        writeDescriptor.dstArrayElement      = newIndex;
        vkUpdateDescriptorSets(vulkan.device, 1, &writeDescriptor, 0, 0);
        utils::threadUnlock(&desc->lock);
    }

    utils::threadUnlock(&storageImageArrayData.lock);
    return newIndex;
}

void vulkanRemoveStorageImageViewFromPool(int poolIndex) {
    utils::threadLock(&storageImageArrayData.lock);
    storageImageArrayData.emptySlots.push_back((u32)poolIndex);
    utils::threadUnlock(&storageImageArrayData.lock);
}

void addBufferGarbage(VulkanBuffer* buffer, VkFence fence, std::atomic<bool>* submitted) {
    utils::threadLock(&garbageLock);
    BufferGarbage garbage = {buffer->buf, buffer->virtualBlock, fence, submitted, buffer->vma};
    buffersToClean.push_back(garbage);
    utils::threadUnlock(&garbageLock);
}

void addImageGarbage(VulkanImage* image, VkFence fence, std::atomic<bool>* submitted) {
    utils::threadLock(&garbageLock);
    ImageGarbage garbage = {.views     = image->views,
                            .view      = image->view,
                            .fence     = fence,
                            .submitted = submitted,
                            .vma       = image->vma,
                            .img       = image->img};
    imagesToClean.push_back(garbage);
    utils::threadUnlock(&garbageLock);
}

void addCommandGarbage(VulkanCommand* command) {
    assert(command->cmd && command->pool && command->fence);
    utils::threadLock(&garbageLock);
    CommandGarbage garbage = {.fence     = command->fence,
                              .submitted = &command->submitted,
                              .pool      = command->pool,
                              .cmd       = command};
    commandsToClean.push_back(garbage);
    utils::threadUnlock(&garbageLock);
}

// Check if a garbage entry's fence is complete. If submitted is non-NULL,
// we must wait until the fence has actually been passed to vkQueueSubmit
// before we can safely call vkGetFenceStatus on it.
static bool isFenceComplete(VkFence fence, std::atomic<bool>* submitted) {
    if (!fence) return true;
    if (submitted && !*submitted) return false;
    return vkGetFenceStatus(vulkan.device, fence) == VK_SUCCESS;
}

void vulkanCleanupGarbage() {
    double gT0     = utils::nanos();
    static int gHitchOn = -1;
    if (gHitchOn < 0) gHitchOn = getenv("ENGINE_HITCH_DEBUG") != nullptr;
    u32 cleaned     = 0;
    utils::threadLock(&garbageLock);

    // 1. Evaluate commandsToClean first. If the fence is signaled,
    // we extract the fence to destroy it AFTER cleaning up images and buffers
    // that might share the same fence.
    for (u32 i = 0; i < commandsToClean.size();) {
        CommandGarbage* garbage = &commandsToClean[i];
        if (isFenceComplete(garbage->fence, garbage->submitted)) {
            if (garbage->fence) {
                fencesToDestroy.push_back(garbage->fence);
            }
            vkDestroyCommandPool(vulkan.device, garbage->pool, nullptr);
            cmdsToFree.push_back(garbage->cmd);
            cleaned++;
            commandsToClean[i] = commandsToClean.back();
            commandsToClean.pop_back();
        } else {
            i++;
        }
    }

    // 2. Clean up buffers (VMA destroys are serialized against pool-thread
    //    allocations, see vulkanVmaLock in VulkanBuffer.c).
    for (u32 i = 0; i < buffersToClean.size();) {
        BufferGarbage* garbage = &buffersToClean[i];
        if (isFenceComplete(garbage->fence, garbage->submitted)) {
            vulkanVmaLock();
            vmaDestroyVirtualBlock(garbage->virtualBlock);
            vmaDestroyBuffer(vulkan.vmaAllocator, garbage->buffer, garbage->vma);
            vulkanVmaUnlock();
            cleaned++;
            buffersToClean[i] = buffersToClean.back();
            buffersToClean.pop_back();
        } else {
            i++;
        }
    }

    // 3. Clean up images
    for (u32 i = 0; i < imagesToClean.size();) {
        ImageGarbage* garbage = &imagesToClean[i];
        if (isFenceComplete(garbage->fence, garbage->submitted)) {
            vkDestroyImageView(vulkan.device, garbage->view, nullptr);
            for (i32 j = 0, s = static_cast<i32>(garbage->views.size()); j < s; j++) {
                vkDestroyImageView(vulkan.device, garbage->views[j], nullptr);
            }
            vulkanVmaLock();
            vmaDestroyImage(vulkan.vmaAllocator, garbage->img, garbage->vma);
            vulkanVmaUnlock();
            cleaned++;
            imagesToClean[i] = imagesToClean.back();
            imagesToClean.pop_back();
        } else {
            i++;
        }
    }

    // 4. Safely destroy fences now that all resources sharing them have been
    // removed
    for (u32 i = 0; i < fencesToDestroy.size(); i++) {
        vkDestroyFence(vulkan.device, fencesToDestroy[i], nullptr);
    }
    fencesToDestroy.clear();

    // 5. Free transient VulkanCommand structs now that no garbage entry
    // references their submitted field.
    for (u32 i = 0; i < cmdsToFree.size(); i++) {
        delete cmdsToFree[i];
    }
    cmdsToFree.clear();

    utils::threadUnlock(&garbageLock);
    if (gHitchOn) {
        double ms = (utils::nanos() - gT0) / 1e6;
        if (ms > 3.0 || cleaned > 8)
            utils::info("HITCH: cleanupGarbage cleaned=%u in %.1f ms", (unsigned)cleaned, ms);
    }
}

void vulkanResourceSetLightBuffers(u64 gridAddress, u64 indexAddress) {
    lightGridAddress  = gridAddress;
    lightIndexAddress = indexAddress;
    for (i32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VulkanAddressBuffer* ab  = static_cast<VulkanAddressBuffer*>(addressBuffer[i].vmaInfo.pMappedData);
        ab->lightGridAddress    = gridAddress;
        ab->lightIndexAddress   = indexAddress;
    }
}

void vulkanResourceSetTerrainDefaults(u32 grassAlbedo, u32 grassNormal,
                                       u32 cliffAlbedo, u32 cliffNormal) {
    for (i32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VulkanSceneBuffer* buf  = static_cast<VulkanSceneBuffer*>(sceneBuffer[i].vmaInfo.pMappedData);
        buf->terrain.grassAlbedoIndex = grassAlbedo;
        buf->terrain.grassNormalIndex = grassNormal;
        buf->terrain.cliffAlbedoIndex = cliffAlbedo;
        buf->terrain.cliffNormalIndex = cliffNormal;
        buf->terrain.pomEnabled = 1;  // POM on by default
    }
}

void vulkanResourceSetTerrainBounds(float minX, float minY, float minZ,
                                     float maxX, float maxY, float maxZ) {
    for (i32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VulkanSceneBuffer* buf  = static_cast<VulkanSceneBuffer*>(sceneBuffer[i].vmaInfo.pMappedData);
        buf->terrain.worldMin[0] = minX;
        buf->terrain.worldMin[1] = minY;
        buf->terrain.worldMin[2] = minZ;
        buf->terrain.worldMin[3] = 0.0f;
        buf->terrain.worldMax[0] = maxX;
        buf->terrain.worldMax[1] = maxY;
        buf->terrain.worldMax[2] = maxZ;
        buf->terrain.worldMax[3] = 0.0f;
    }
}

void vulkanResourceSetTerrainClimateTextures(u32 biomeColorIndex, u32 climateIndex) {
    if (!vulkan.device) return;
    for (i32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VulkanSceneBuffer* buf  = static_cast<VulkanSceneBuffer*>(sceneBuffer[i].vmaInfo.pMappedData);
        buf->terrain.biomeColorIndex = biomeColorIndex;
        buf->terrain.climateIndex    = climateIndex;
    }
}

void vulkanResourceSetTerrainSnowSand(u32 snowAlbedoIndex, u32 sandAlbedoIndex) {
    if (!vulkan.device) return;
    for (i32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VulkanSceneBuffer* buf  = static_cast<VulkanSceneBuffer*>(sceneBuffer[i].vmaInfo.pMappedData);
        buf->terrain.snowAlbedoIndex = snowAlbedoIndex;
        buf->terrain.sandAlbedoIndex = sandAlbedoIndex;
    }
}

void vulkanResourceSetTerrainClimateParams(float snowLo,
                                           float snowHi,
                                           float beachHeight,
                                           float enabled) {
    if (!vulkan.device) return;
    for (i32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VulkanSceneBuffer* buf   = static_cast<VulkanSceneBuffer*>(sceneBuffer[i].vmaInfo.pMappedData);
        buf->terrain.climateParams[0] = snowLo;
        buf->terrain.climateParams[1] = snowHi;
        buf->terrain.climateParams[2] = beachHeight;
        buf->terrain.climateParams[3] = enabled;
    }
}

static u32 splatUdimCallCount = 0;

void vulkanResourceSetTerrainSplatUdim(u32 groupIndex, u32 udimTileIndex, u32 textureId) {
    splatUdimCallCount++;
    if (splatUdimCallCount == 1) {
        utils::info("vulkanResource: FIRST splatUdim call: group=%u tile=%u texId=%u", groupIndex, udimTileIndex, textureId);
    }
    if (groupIndex >= MAX_SPLAT_GROUPS || udimTileIndex >= SPLAT_UDIM_TILES) return;
    for (i32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VulkanSceneBuffer* buf = (VulkanSceneBuffer*)sceneBuffer[i].vmaInfo.pMappedData;
        buf->terrain.splatGroups[groupIndex].weightTextures[udimTileIndex] = textureId;
    }
    if (splatUdimCallCount <= 3) {
        utils::info("vulkanResource: SetTerrainSplatUdim group=%u tile=%u texId=%u (total calls=%u)",
             groupIndex, udimTileIndex, textureId, splatUdimCallCount);
    }
    // After first call, verify frame[0] was written
    if (splatUdimCallCount == 1) {
        VulkanSceneBuffer* buf0 = (VulkanSceneBuffer*)sceneBuffer[0].vmaInfo.pMappedData;
        utils::info("vulkanResource: After first write, frame[0] group[%u].tile[%u]=%u",
             groupIndex, udimTileIndex, buf0->terrain.splatGroups[groupIndex].weightTextures[udimTileIndex]);
    }
}

void vulkanResourceDebugTerrainSplat(void) {
    VulkanSceneBuffer* buf  = static_cast<VulkanSceneBuffer*>(sceneBuffer[0].vmaInfo.pMappedData);
    utils::info("vulkanResource: splatGroupCount=%u, buf=%p, addr=%p", buf->terrain.splatGroupCount, (void*)buf, (void*)buf->terrain.splatGroups[0].weightTextures);
    for (u32 g = 0; g < buf->terrain.splatGroupCount && g < MAX_SPLAT_GROUPS; g++) {
        bool anyNonzero = false;
        for (u32 i = 0; i < SPLAT_UDIM_TILES; i++) {
            if (buf->terrain.splatGroups[g].weightTextures[i] != 0) {
                if (!anyNonzero) utils::info("vulkanResource:   splatGroup[%u] nonzero weights:", g);
                utils::info("    [idx=%u]=%u", i, buf->terrain.splatGroups[g].weightTextures[i]);
                anyNonzero = true;
            }
        }
        if (!anyNonzero) utils::info("vulkanResource:   splatGroup[%u] all weights are 0", g);
    }
    utils::info("vulkanResource: total vulkanResourceSetTerrainSplatUdim calls: %u", splatUdimCallCount);
}

u32 vulkanResourceGetTerrainSplatGroupCount(void) {
    VulkanSceneBuffer* buf = (VulkanSceneBuffer*)sceneBuffer[0].vmaInfo.pMappedData;
    return buf->terrain.splatGroupCount;
}

void vulkanResourceSetTerrainSplatGroupCount(u32 count) {
    if (count > MAX_SPLAT_GROUPS) count = MAX_SPLAT_GROUPS;
    for (i32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VulkanSceneBuffer* buf  = static_cast<VulkanSceneBuffer*>(sceneBuffer[i].vmaInfo.pMappedData);
        buf->terrain.splatGroupCount = count;
    }
}

u32 vulkanResourceGetTerrainPomEnabled(void) {
    VulkanSceneBuffer* buf = (VulkanSceneBuffer*)sceneBuffer[0].vmaInfo.pMappedData;
    return buf->terrain.pomEnabled;
}

void vulkanResourceSetTerrainPomEnabled(u32 enabled) {
    for (i32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VulkanSceneBuffer* buf  = static_cast<VulkanSceneBuffer*>(sceneBuffer[i].vmaInfo.pMappedData);
        buf->terrain.pomEnabled = enabled;
    }
}

void vulkanResourceGetTerrainState(u32* grassAlbedoIndex, u32* cliffAlbedoIndex,
                                    u32* splatGroupCount, float worldMin[3], float worldMax[3]) {
    VulkanSceneBuffer* buf  = static_cast<VulkanSceneBuffer*>(sceneBuffer[0].vmaInfo.pMappedData);
    if (grassAlbedoIndex) *grassAlbedoIndex = buf->terrain.grassAlbedoIndex;
    if (cliffAlbedoIndex) *cliffAlbedoIndex = buf->terrain.cliffAlbedoIndex;
    if (splatGroupCount) *splatGroupCount = buf->terrain.splatGroupCount;
    if (worldMin) {
        worldMin[0] = buf->terrain.worldMin[0];
        worldMin[1] = buf->terrain.worldMin[1];
        worldMin[2] = buf->terrain.worldMin[2];
    }
    if (worldMax) {
        worldMax[0] = buf->terrain.worldMax[0];
        worldMax[1] = buf->terrain.worldMax[1];
        worldMax[2] = buf->terrain.worldMax[2];
    }
}

// ── Fog data ────────────────────────────────────────────────────────

void vulkanResourceSetFogData(VulkanFogData fog) {
    fogData = fog;
}

VulkanFogData vulkanResourceGetFogData(void) {
    return fogData;
}

// ── Water data ────────────────────────────────────────────────────────

void vulkanResourceSetWaterParams(const VulkanWaterData* params) {
    if (params) waterData = *params;
}

VulkanWaterData vulkanResourceGetWaterData(void) {
    return waterData;
}

// ── Azgaar props data (workstream B) ──────────────────────────────

void vulkanResourceSetAzgaarPropsData(const VulkanAzgaarPropsData* params) {
    if (params) azgaarPropsData = *params;
}

VulkanAzgaarPropsData vulkanResourceGetAzgaarPropsData(void) {
    return azgaarPropsData;
}

// ── Weather data (GPU particle weather) ───────────────────────────

void vulkanResourceSetWeather(const VulkanWeatherData* data) {
    if (data) weatherData = *data;
}

VulkanWeatherData vulkanResourceGetWeatherData(void) {
    return weatherData;
}

void vulkanResourceSetCameraOccluders(const u32* entities, const float* alphas, u32 count) {
    if (count > VULKAN_MAX_CAMERA_OCCLUDERS) count = VULKAN_MAX_CAMERA_OCCLUDERS;

    for (i32 f = 0; f < FRAMES_IN_FLIGHT; f++) {
        if (!sceneBuffer[f].vmaInfo.pMappedData) continue;

        VulkanSceneBuffer* buf  = static_cast<VulkanSceneBuffer*>(sceneBuffer[f].vmaInfo.pMappedData);
        memset(&buf->cameraOccluders, 0, sizeof(buf->cameraOccluders));
        buf->cameraOccluders.count = count;

        for (u32 i = 0; i < count; i++) {
            buf->cameraOccluders.entityIds[i] = entities ? entities[i] : 0;
            buf->cameraOccluders.alphas[i]    = alphas ? alphas[i] : 0.35f;
        }
    }
}


}  // namespace engine
