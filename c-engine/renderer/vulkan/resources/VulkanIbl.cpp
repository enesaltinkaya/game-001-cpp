#include "VulkanIbl.h"
#include <algorithm>
#include "VulkanBuffer.h"
#include "VulkanResourceManager.h"
#include "ExrLoader.h"
#include "datamanager/DataManager.h"
#include "renderer/Renderer.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"

static const char* STUDIOLIGHTS_DIR = "images/hdr";
static const char* ENVIRONMENT_PATH = "images/hdr/interior.exr";

// IBL file list for cycling
static Array(String) iblFiles;
static int iblCurrentIndex;
static char iblFileListReady;

enum {
    IRRADIANCE_SIZE = 32,
    PREFILTER_SIZE  = 256,
    PREFILTER_MIPS  = 6,
    BRDF_LUT_SIZE   = 512,
    BLUE_NOISE_SIZE = 128,
};

struct VulkanIblResources {
    VulkanImage environment;
    VulkanImage irradiance;
    VulkanImage prefilter;
    VulkanImage brdfLut;
    VulkanImage blueNoise;
    VulkanImage tonemapLut;
    VulkanImage tonemapLutPunchy;
    float environmentMaxLod;
    float prefilterMaxLod;
    vec4 shL0_M0;
    vec4 shL1_Mn1;
    vec4 shL1_M0;
    vec4 shL1_Mp1;
    bool hasSH;
    IblSunLight extractedSun;
    mat4 envRotation;
    u32 tonemapMode;
    bool enabled;
    float intensity;
    float specularIntensity;
    bool ready;
};

struct IblFacePushConstants {
    u32 environmentMapIndex;
    u32 faceIndex;
    float roughness;
    float pad;
};

static VulkanIblResources ibl;
static VulkanPipe irradiancePipe;
static VulkanPipe prefilterPipe;
static VulkanPipe brdfPipe;
static bool pipesReady;

static const float SUN_THRESHOLD = 10.0f;

static u32 calculateMipLevels(int width, int height);
static void uploadEnvironment(const float* pixels, int width, int height, u32 mipLevels);
static void createEnvironmentDependentImages(void);
static void createBrdfLut(void);
static void createDitherNoiseTexture(void);
static void createTonemapLut(void);
static void createPipelines(void);
static void precomputeIbl(char includeBrdf);
static void destroyPipelines(void);
static void destroyImage(VulkanImage* image);
static VkImageView createAttachmentView(VulkanImage* image, u32 baseMipLevel, u32 baseArrayLayer);
static VulkanImage makeAttachmentProxy(VulkanImage* image, VkImageView view, u32 mipLevel);
static void renderBrdfLut(VulkanCommand* cmd, Array(VkImageView) * tempViews);
static void renderIrradiance(VulkanCommand* cmd,
                             Array(VkImageView) * tempViews,
                             u32 environmentMapIndex);
static void renderPrefilter(VulkanCommand* cmd,
                            Array(VkImageView) * tempViews,
                            u32 environmentMapIndex);
static void generateDitherNoise(u8* pixels, u32 size);
static void extractSHAndSun(const float* pixels, int width, int height, float sunThreshold);
static void pushIblState(void);
static void buildIblFileList(void);
static void loadEnvironmentFromPath(const char* path);

static void pushIblState(void) {
    vulkanResourceSetIbl(
            .environmentMapIndex   = (u32)ibl.environment.sampledPoolIndex,
            .irradianceMapIndex    = (u32)ibl.irradiance.sampledPoolIndex,
            .prefilterMapIndex     = (u32)ibl.prefilter.sampledPoolIndex,
            .brdfLutIndex          = (u32)ibl.brdfLut.sampledPoolIndex,
            .blueNoiseIndex        = (u32)ibl.blueNoise.sampledPoolIndex,
            .tonemapLutIndex       = (u32)ibl.tonemapLut.sampledPoolIndex,
            .tonemapLutPunchyIndex = (u32)ibl.tonemapLutPunchy.sampledPoolIndex,
            .environmentMapMaxLod  = ibl.environmentMaxLod,
            .prefilterMapMaxLod    = ibl.prefilterMaxLod,
            .enabled               = ibl.enabled,
            .intensity             = ibl.intensity,
            .specularIntensity     = ibl.specularIntensity,
            .sunThreshold          = SUN_THRESHOLD,
            .hasSH                 = ibl.hasSH,
            .tonemapMode           = ibl.tonemapMode,
            .shL0_M0     = {ibl.shL0_M0[0], ibl.shL0_M0[1], ibl.shL0_M0[2], ibl.shL0_M0[3]},
            .shL1_Mn1    = {ibl.shL1_Mn1[0], ibl.shL1_Mn1[1], ibl.shL1_Mn1[2], ibl.shL1_Mn1[3]},
            .shL1_M0     = {ibl.shL1_M0[0], ibl.shL1_M0[1], ibl.shL1_M0[2], ibl.shL1_M0[3]},
            .shL1_Mp1    = {ibl.shL1_Mp1[0], ibl.shL1_Mp1[1], ibl.shL1_Mp1[2], ibl.shL1_Mp1[3]},
            .envRotation = {
                ibl.envRotation[0][0],
                ibl.envRotation[0][1],
                ibl.envRotation[0][2],
                ibl.envRotation[0][3],
                ibl.envRotation[1][0],
                ibl.envRotation[1][1],
                ibl.envRotation[1][2],
                ibl.envRotation[1][3],
                ibl.envRotation[2][0],
                ibl.envRotation[2][1],
                ibl.envRotation[2][2],
                ibl.envRotation[2][3],
                ibl.envRotation[3][0],
                ibl.envRotation[3][1],
                ibl.envRotation[3][2],
                ibl.envRotation[3][3],
            }, );
}

static void buildIblFileList(void) {
    if (iblFileListReady) return;

    Array(String) hdrFiles = dataManagerListFiles(".hdr");
    foreach (String s, hdrFiles) {
        if (strStartsWith(s.data, STUDIOLIGHTS_DIR)) {
            String copy = {};
            stringAppend(&copy, s.data);
            arrayPut(iblFiles, copy);
        }
    }
    foreach (String s, hdrFiles) {
        stringDestroy((String*)&s);
    }
    arrayFree(hdrFiles);

    Array(String) exrFiles = dataManagerListFiles(".exr");
    foreach (String s, exrFiles) {
        if (strStartsWith(s.data, STUDIOLIGHTS_DIR)) {
            String copy = {};
            stringAppend(&copy, s.data);
            arrayPut(iblFiles, copy);
        }
    }
    foreach (String s, exrFiles) {
        stringDestroy((String*)&s);
    }
    arrayFree(exrFiles);

    iblCurrentIndex = 0;
    for (u32 i = 0; i < arraySize(iblFiles); i++) {
        if (strcmp(iblFiles[i].data, ENVIRONMENT_PATH) == 0) {
            iblCurrentIndex = (int)i;
            break;
        }
    }

    iblFileListReady = 1;
    info("vulkanIbl: found %d IBL files in %s", arraySize(iblFiles), STUDIOLIGHTS_DIR);
}

static void loadEnvironmentFromPath(const char* path) {
    if (!dataManagerFileExists(path)) {
        warn("vulkanIbl: environment map not found: %s", path);
        if (!ibl.ready) vulkanResourceSetIbl(0);
        return;
    }

    String fileData = dataManagerRead(path);
    if (!fileData.data || fileData.size == 0) {
        warn("vulkanIbl: failed to read %s", path);
        stringDestroy(&fileData);
        if (!ibl.ready) vulkanResourceSetIbl(0);
        return;
    }

    int width     = 0;
    int height    = 0;
    float* pixels = nullptr;

    String probe = {.data = const_cast<char*>(path), .size = static_cast<u32>(strlen(path))};
    if (stringEndsWith(&probe, "exr")) {
        pixels = exrLoadFromMemory(fileData.data, fileData.size, &width, &height);
    } else {
        int channelsInFile = 0;
        pixels             = stbi_loadf_from_memory((const stbi_uc*)fileData.data,
                                                    (int)fileData.size,
                                                    &width,
                                                    &height,
                                                    &channelsInFile,
                                                    4);
    }
    stringDestroy(&fileData);

    if (!pixels || width <= 0 || height <= 0) {
        warn("vulkanIbl: failed to decode HDR image %s", path);
        if (pixels) memoryFree(pixels);
        if (!ibl.ready) vulkanResourceSetIbl(0);
        return;
    }

    if (ibl.ready) {
        rendererWaitIdle("IBL environment reload");
        destroyImage(&ibl.prefilter);
        destroyImage(&ibl.irradiance);
        destroyImage(&ibl.environment);
    }

    extractSHAndSun(pixels, width, height, SUN_THRESHOLD);

    u32 mipLevels = calculateMipLevels(width, height);
    uploadEnvironment(pixels, width, height, mipLevels);
    memoryFree(pixels);

    ibl.environmentMaxLod = (float)(mipLevels - 1);
    ibl.prefilterMaxLod   = (float)(PREFILTER_MIPS - 1);

    char firstTime = !ibl.ready;

    createPipelines();
    createEnvironmentDependentImages();
    if (firstTime) {
        createBrdfLut();
    }
    precomputeIbl(firstTime);
    destroyPipelines();

    if (firstTime) {
        createDitherNoiseTexture();
        createTonemapLut();
        ibl.ready             = 1;
        ibl.enabled           = 1;
        ibl.intensity         = IBL_INTENSITY;
        ibl.specularIntensity = IBL_SPEC_INTENSITY;
    }

    glm_mat4_identity(ibl.envRotation);

    pushIblState();
    info("vulkanIbl: loaded %s (%dx%d envLod=%.0f)", path, width, height, ibl.environmentMaxLod);

    signalEmit("iblChanged", nullptr);
}

void vulkanIblInit(void) {
    if (ibl.ready) return;
    buildIblFileList();
    loadEnvironmentFromPath(ENVIRONMENT_PATH);
}

void vulkanIblDestroy(void) {
    destroyImage(&ibl.tonemapLutPunchy);
    destroyImage(&ibl.tonemapLut);
    destroyImage(&ibl.blueNoise);
    destroyImage(&ibl.brdfLut);
    destroyImage(&ibl.prefilter);
    destroyImage(&ibl.irradiance);
    destroyImage(&ibl.environment);
    destroyPipelines();
    ibl = VulkanIblResources{};

    foreach (String s, iblFiles) {
        stringDestroy((String*)&s);
    }
    arrayFree(iblFiles);
    iblFiles         = nullptr;
    iblFileListReady = 0;
}

void vulkanIblCycleNext(void) {
    if (arraySize(iblFiles) < 2) return;
    iblCurrentIndex = (iblCurrentIndex + 1) % (int)arraySize(iblFiles);
    loadEnvironmentFromPath(iblFiles[iblCurrentIndex].data);
}

void vulkanIblCyclePrev(void) {
    if (arraySize(iblFiles) < 2) return;
    iblCurrentIndex = (iblCurrentIndex + (int)arraySize(iblFiles) - 1) % (int)arraySize(iblFiles);
    loadEnvironmentFromPath(iblFiles[iblCurrentIndex].data);
}

const char* vulkanIblGetCurrentName(void) {
    if (arraySize(iblFiles) == 0) return "(none)";
    const char* path  = iblFiles[iblCurrentIndex].data;
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

VulkanImage* vulkanIblGetEnvironmentImage(void) {
    return ibl.environment.img ? &ibl.environment : nullptr;
}

IblSunLight vulkanIblGetExtractedSun(void) {
    return ibl.extractedSun;
}

void vulkanIblSetDisabled(bool disabled) {
    ibl.enabled = !disabled;
    if (!ibl.ready) return;
    pushIblState();
}

bool vulkanIblIsDisabled(void) {
    return !ibl.enabled;
}

void vulkanIblSetIntensity(float intensity) {
    if (intensity < 0.0f) intensity = 0.0f;
    ibl.intensity = intensity;
    if (!ibl.ready) return;
    pushIblState();
}

float vulkanIblGetIntensity(void) {
    return ibl.intensity;
}

void vulkanIblSetTonemapMode(TonemapMode mode) {
    ibl.tonemapMode = (u32)mode;
    if (!ibl.ready) return;
    pushIblState();
}

void vulkanIblRotateSun(float azimuthDeg, float elevationDeg) {
    vec3 dir;
    glm_vec3_copy(ibl.extractedSun.direction, dir);

    mat4 stepRot;
    glm_mat4_identity(stepRot);

    if (fabsf(azimuthDeg) > 0.001f) {
        float rad = glm_rad(azimuthDeg);
        mat4 rot;
        glm_rotate_make(rot, rad, vec3{0.0f, 1.0f, 0.0f});
        glm_mat4_mul(rot, stepRot, stepRot);
        vec3 tmp;
        glm_mat4_mulv3(rot, dir, 1.0f, tmp);
        glm_vec3_copy(tmp, dir);
    }

    if (fabsf(elevationDeg) > 0.001f) {
        float rad   = glm_rad(elevationDeg);
        vec3 dirXZ  = {dir[0], 0.0f, dir[2]};
        float lenXZ = glm_vec3_norm(dirXZ);
        if (lenXZ > 0.001f) {
            vec3 right;
            glm_vec3_cross(vec3{0.0f, 1.0f, 0.0f}, dirXZ, right);
            glm_vec3_normalize(right);
            mat4 rot;
            glm_rotate_make(rot, rad, right);
            glm_mat4_mul(rot, stepRot, stepRot);
            vec3 tmp;
            glm_mat4_mulv3(rot, dir, 1.0f, tmp);
            if (tmp[1] < 0.05f) tmp[1] = 0.05f;
            glm_vec3_copy(tmp, dir);
        }
    }

    glm_vec3_normalize(dir);
    glm_vec3_copy(dir, ibl.extractedSun.direction);

    mat4 stepRotInv;
    glm_mat4_transpose_to(stepRot, stepRotInv);
    mat4 combined;
    glm_mat4_mul(stepRotInv, ibl.envRotation, combined);
    glm_mat4_copy(combined, ibl.envRotation);

    pushIblState();
    signalEmit("iblChanged", nullptr);
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static u32 calculateMipLevels(int width, int height) {
    int maxDimension = width > height ? width : height;
    u32 levels       = 1;
    while (maxDimension > 1) {
        maxDimension >>= 1;
        levels++;
    }
    return levels;
}

static void uploadEnvironment(const float* pixels, int width, int height, u32 mipLevels) {
    ibl.environment =
        vulkanCreateImage(.name   = "IBL Environment Equirect",
                          .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                          .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          .mipLevels = (int)mipLevels,
                          .width     = width,
                          .height    = height);

    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanTransition(cmd, &ibl.environment, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);
    vulkanCopy(.cmd         = cmd,
               .target.img  = &ibl.environment,
               .source.data = (void*)pixels,
               .size        = (u32)((u64)width * height * 4 * sizeof(float)));

    if (mipLevels > 1) {
        vulkanImgGenerateMips(cmd, &ibl.environment);
    } else {
        vulkanTransition(cmd, &ibl.environment, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    }
    vulkanTransientEnd(cmd, 1);
}

static void createEnvironmentDependentImages(void) {
    ibl.irradiance =
        vulkanCreateImage(.name   = "IBL Irradiance",
                          .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                          .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                          .width = IRRADIANCE_SIZE,
                          .height   = IRRADIANCE_SIZE,
                          .layers   = 6,
                          .flags    = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                          .viewType = VK_IMAGE_VIEW_TYPE_CUBE);

    ibl.prefilter =
        vulkanCreateImage(.name   = "IBL Prefilter",
                          .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                          .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                          .width = PREFILTER_SIZE,
                          .height    = PREFILTER_SIZE,
                          .layers    = 6,
                          .mipLevels = PREFILTER_MIPS,
                          .flags     = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                          .viewType  = VK_IMAGE_VIEW_TYPE_CUBE);
}

static void createBrdfLut(void) {
    ibl.brdfLut =
        vulkanCreateImage(.name   = "IBL BRDF LUT",
                          .format = VK_FORMAT_R16G16_SFLOAT,
                          .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                          .width = BRDF_LUT_SIZE,
                          .height = BRDF_LUT_SIZE);
}

static void createDitherNoiseTexture(void) {
    const u32 pixelCount = BLUE_NOISE_SIZE * BLUE_NOISE_SIZE;
    u8* pixels = static_cast<u8*>(memoryAlloc(pixelCount));
    generateDitherNoise(pixels, BLUE_NOISE_SIZE);

    ibl.blueNoise =
        vulkanCreateImage(.name   = "IBL Blue Noise",
                          .format = VK_FORMAT_R8_UNORM,
                          .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          .width  = BLUE_NOISE_SIZE,
                          .height = BLUE_NOISE_SIZE);

    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanTransition(cmd, &ibl.blueNoise, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);
    vulkanCopy(.cmd = cmd, .target.img = &ibl.blueNoise, .source.data = pixels, .size = pixelCount);
    vulkanTransition(cmd, &ibl.blueNoise, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransientEnd(cmd, 1);

    memoryFree(pixels);
}

static char loadTonemapLutFile(const char* path, const char* name, VulkanImage* outImage) {
    String fileData = dataManagerRead(path);
    if (!fileData.data || fileData.size == 0) {
        warn("vulkanIbl: tonemap LUT not found: %s", path);
        stringDestroy(&fileData);
        return 0;
    }

    if (fileData.size < 16) {
        warn("vulkanIbl: tonemap LUT file too small: %s", path);
        stringDestroy(&fileData);
        return 0;
    }

    const u32* header = (const u32*)fileData.data;
    u32 width         = header[0];
    u32 height        = header[1];

    u32 expectedSize = 16 + width * height * 4 * 2;
    if (fileData.size < expectedSize) {
        warn("vulkanIbl: tonemap LUT size mismatch: %s", path);
        stringDestroy(&fileData);
        return 0;
    }

    void* pixels   = (u8*)fileData.data + 16;
    u32 pixelBytes = width * height * 4 * 2;

    *outImage =
        vulkanCreateImage(.name   = name,
                          .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                          .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          .width  = static_cast<int>(width),
                          .height = static_cast<int>(height));

    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanTransition(cmd, outImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);
    vulkanCopy(.cmd = cmd, .target.img = outImage, .source.data = pixels, .size = pixelBytes);
    vulkanTransition(cmd, outImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransientEnd(cmd, 1);

    stringDestroy(&fileData);
    info("vulkanIbl: loaded tonemap LUT %s (%dx%d)", path, width, height);
    return 1;
}

static void createTonemapLut(void) {
    loadTonemapLutFile("luts/agx_base.bin", "Tonemap LUT AgX", &ibl.tonemapLut);
    loadTonemapLutFile("luts/agx_punchy.bin", "Tonemap LUT AgX Punchy", &ibl.tonemapLutPunchy);
}

static void createPipelines(void) {
    if (pipesReady) return;

    irradiancePipe = vulkanCreatePipe(.name         = "ibl_irradiance",
                                      .vs           = "shaders/pass/ibl/spv/fullscreen.vert.spv",
                                      .fs           = "shaders/pass/ibl/spv/irradiance.frag.spv",
                                      .colorFormat1 = VK_FORMAT_R16G16B16A16_SFLOAT,
                                      .clearColor1Enabled = 1,
                                      .clearColor1        = {0, 0, 0, 0});

    prefilterPipe = vulkanCreatePipe(.name         = "ibl_prefilter",
                                     .vs           = "shaders/pass/ibl/spv/fullscreen.vert.spv",
                                     .fs           = "shaders/pass/ibl/spv/prefilter.frag.spv",
                                     .colorFormat1 = VK_FORMAT_R16G16B16A16_SFLOAT,
                                     .clearColor1Enabled = 1,
                                     .clearColor1        = {0, 0, 0, 0});

    brdfPipe = vulkanCreatePipe(.name               = "ibl_brdf_lut",
                                .vs                 = "shaders/pass/ibl/spv/fullscreen.vert.spv",
                                .fs                 = "shaders/pass/ibl/spv/brdf_lut.frag.spv",
                                .colorFormat1       = VK_FORMAT_R16G16_SFLOAT,
                                .clearColor1Enabled = 1,
                                .clearColor1        = {0, 0, 0, 0});

    pipesReady = 1;
}

static void precomputeIbl(char includeBrdf) {
    Array(VkImageView) tempViews = {};
    VulkanCommand* cmd           = vulkanTransientBegin();

    vulkanTransition(cmd, &ibl.irradiance, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    vulkanTransition(cmd, &ibl.prefilter, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    if (includeBrdf) {
        vulkanTransition(cmd, &ibl.brdfLut, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    }
    vulkanTransition(cmd, &ibl.environment, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    if (includeBrdf) renderBrdfLut(cmd, &tempViews);
    renderIrradiance(cmd, &tempViews, (u32)ibl.environment.sampledPoolIndex);
    renderPrefilter(cmd, &tempViews, (u32)ibl.environment.sampledPoolIndex);

    vulkanTransition(cmd, &ibl.irradiance, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, &ibl.prefilter, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    if (includeBrdf) {
        vulkanTransition(cmd, &ibl.brdfLut, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    }

    vulkanTransientEnd(cmd, 1);

    foreach (VkImageView view, tempViews) {
        vkDestroyImageView(vulkan.device, view, nullptr);
    }
    arrayFree(tempViews);
}

static void destroyPipelines(void) {
    if (!pipesReady) return;
    vulkanDestroyPipe(&brdfPipe);
    vulkanDestroyPipe(&prefilterPipe);
    vulkanDestroyPipe(&irradiancePipe);
    pipesReady = 0;
}

static void destroyImage(VulkanImage* image) {
    if (image->img) {
        vulkanDestroyImage(image, nullptr);
    }
}

static VkImageView createAttachmentView(VulkanImage* image, u32 baseMipLevel, u32 baseArrayLayer) {
    VkImageView view                         = VK_NULL_HANDLE;
    VkImageViewCreateInfo viewInfo           = {};
    viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                           = image->img;
    viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format                          = image->format;
    viewInfo.subresourceRange.aspectMask     = image->aspect;
    viewInfo.subresourceRange.baseMipLevel   = baseMipLevel;
    viewInfo.subresourceRange.levelCount     = 1;
    viewInfo.subresourceRange.baseArrayLayer = baseArrayLayer;
    viewInfo.subresourceRange.layerCount     = 1;
    vkCreateImageView(vulkan.device, &viewInfo, nullptr, &view);
    return view;
}

static VulkanImage makeAttachmentProxy(VulkanImage* image, VkImageView view, u32 mipLevel) {
    VulkanImage proxy   = *image;
    proxy.view          = view;
    proxy.views         = nullptr;
    proxy.layers        = 1;
    proxy.mipLevels     = 1;
    proxy.extent.width  = std::max(1u, image->extent.width >> mipLevel);
    proxy.extent.height = std::max(1u, image->extent.height >> mipLevel);
    proxy.extent.depth  = 1;
    return proxy;
}

static void renderBrdfLut(VulkanCommand* cmd, Array(VkImageView) * tempViews) {
    VkImageView view = createAttachmentView(&ibl.brdfLut, 0, 0);
    arrayPut(*tempViews, view);
    VulkanImage target = makeAttachmentProxy(&ibl.brdfLut, view, 0);
    vulkanBeginRender(.cmd = cmd, .pipe = &brdfPipe, .color1 = &target);
    vulkanViewport(cmd, 0, target.extent.height, target.extent.width, -((i32)target.extent.height));
    vulkanScissor(cmd, 0, 0, target.extent.width, target.extent.height);
    vulkanBindPipe(cmd, &brdfPipe);
    vulkanDraw(cmd, 3, 1);
    vulkanEndRender(cmd);
}

static void renderIrradiance(VulkanCommand* cmd,
                             Array(VkImageView) * tempViews,
                             u32 environmentMapIndex) {
    for (u32 face = 0; face < 6; face++) {
        VkImageView view = createAttachmentView(&ibl.irradiance, 0, face);
        arrayPut(*tempViews, view);
        VulkanImage target = makeAttachmentProxy(&ibl.irradiance, view, 0);
        vulkanBeginRender(.cmd = cmd, .pipe = &irradiancePipe, .color1 = &target);
        vulkanViewport(cmd,
                       0,
                       target.extent.height,
                       target.extent.width,
                       -((i32)target.extent.height));
        vulkanScissor(cmd, 0, 0, target.extent.width, target.extent.height);
        vulkanBindPipe(cmd, &irradiancePipe);
        IblFacePushConstants pc = {.environmentMapIndex = environmentMapIndex, .faceIndex = face};
        vulkanPush(cmd, &irradiancePipe, sizeof(pc), &pc);
        vulkanDraw(cmd, 3, 1);
        vulkanEndRender(cmd);
    }
}

static void renderPrefilter(VulkanCommand* cmd,
                            Array(VkImageView) * tempViews,
                            u32 environmentMapIndex) {
    for (u32 mip = 0; mip < PREFILTER_MIPS; mip++) {
        float roughness = PREFILTER_MIPS > 1 ? (float)mip / (float)(PREFILTER_MIPS - 1) : 0.0f;
        for (u32 face = 0; face < 6; face++) {
            VkImageView view = createAttachmentView(&ibl.prefilter, mip, face);
            arrayPut(*tempViews, view);
            VulkanImage target = makeAttachmentProxy(&ibl.prefilter, view, mip);
            vulkanBeginRender(.cmd = cmd, .pipe = &prefilterPipe, .color1 = &target);
            vulkanViewport(cmd,
                           0,
                           target.extent.height,
                           target.extent.width,
                           -((i32)target.extent.height));
            vulkanScissor(cmd, 0, 0, target.extent.width, target.extent.height);
            vulkanBindPipe(cmd, &prefilterPipe);
            IblFacePushConstants pc = {.environmentMapIndex = environmentMapIndex,
                                       .faceIndex           = face,
                                       .roughness           = roughness};
            vulkanPush(cmd, &prefilterPipe, sizeof(pc), &pc);
            vulkanDraw(cmd, 3, 1);
            vulkanEndRender(cmd);
        }
    }
}

// ---------------------------------------------------------------------------
// SH + Sun extraction
// ---------------------------------------------------------------------------

static float luminance(float r, float g, float b) {
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

/* Equirect direction for a pixel centre, matching the extraction mapping
 * (phi = 2*pi*(u-0.5), theta = pi*(0.5-v)). */
static void equirectPixelDir(int x, int y, int width, int height, vec3 out) {
    float v        = ((float)y + 0.5f) / (float)height;
    float u        = ((float)x + 0.5f) / (float)width;
    float theta    = GLM_PIf * (0.5f - v);
    float phi      = 2.0f * GLM_PIf * (u - 0.5f);
    float cosTheta = cosf(theta);
    out[0]         = cosTheta * cosf(phi);
    out[1]         = sinf(theta);
    out[2]         = cosTheta * sinf(phi);
}

static void extractSHAndSun(const float* pixels, int width, int height, float sunThreshold) {
    const float Y00 = 0.282095f;
    const float Y1x = 0.488603f;

    double shL0[3] = {}, shL1n[3] = {}, shL10[3] = {}, shL1p[3] = {};
    double sunRadiance[3] = {}, sunDirWeighted[3] = {}, sunWeightTotal = 0.0;

    /* Brightest pixel — seed of the dominant-hotspot sun cluster. */
    float bestLum = 0.0f;
    int bestX = -1, bestY = -1;

    for (int y = 0; y < height; y++) {
        float v          = ((float)y + 0.5f) / (float)height;
        float theta      = GLM_PIf * (0.5f - v);
        float cosTheta   = cosf(theta);
        float sinTheta   = sinf(theta);
        float dTheta     = GLM_PIf / (float)height;
        float dPhi       = 2.0f * GLM_PIf / (float)width;
        float solidAngle = cosTheta * dTheta * dPhi;
        if (solidAngle <= 0.0f) continue;

        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;
            float r = pixels[idx + 0], g = pixels[idx + 1], b = pixels[idx + 2];
            float u   = ((float)x + 0.5f) / (float)width;
            float phi = 2.0f * GLM_PIf * (u - 0.5f);
            float dx  = cosTheta * cosf(phi);
            float dz  = cosTheta * sinf(phi);
            float dy  = sinTheta;

            float lum = luminance(r, g, b);
            if (lum > bestLum) {
                bestLum = lum;
                bestX   = x;
                bestY   = y;
            }
            if (lum > sunThreshold) {
                float scale = (lum - sunThreshold) / lum;
                float sr = r * scale, sg = g * scale, sb = b * scale;
                float sunWeight = luminance(sr, sg, sb) * solidAngle;
                sunRadiance[0] += sr * (double)solidAngle;
                sunRadiance[1] += sg * (double)solidAngle;
                sunRadiance[2] += sb * (double)solidAngle;
                sunDirWeighted[0] += dx * (double)sunWeight;
                sunDirWeighted[1] += dy * (double)sunWeight;
                sunDirWeighted[2] += dz * (double)sunWeight;
                sunWeightTotal += sunWeight;
                r -= sr;
                g -= sg;
                b -= sb;
            }

            double w = solidAngle;
            shL0[0] += r * Y00 * w;
            shL0[1] += g * Y00 * w;
            shL0[2] += b * Y00 * w;
            shL1n[0] += r * Y1x * dy * w;
            shL1n[1] += g * Y1x * dy * w;
            shL1n[2] += b * Y1x * dy * w;
            shL10[0] += r * Y1x * dz * w;
            shL10[1] += g * Y1x * dz * w;
            shL10[2] += b * Y1x * dz * w;
            shL1p[0] += r * Y1x * dx * w;
            shL1p[1] += g * Y1x * dx * w;
            shL1p[2] += b * Y1x * dx * w;
        }
    }

    glm_vec4_copy(vec4{static_cast<float>(shL0[0]), static_cast<float>(shL0[1]), static_cast<float>(shL0[2]), 0}, ibl.shL0_M0);
    glm_vec4_copy(vec4{static_cast<float>(shL1n[0]), static_cast<float>(shL1n[1]), static_cast<float>(shL1n[2]), 0}, ibl.shL1_Mn1);
    glm_vec4_copy(vec4{static_cast<float>(shL10[0]), static_cast<float>(shL10[1]), static_cast<float>(shL10[2]), 0}, ibl.shL1_M0);
    glm_vec4_copy(vec4{static_cast<float>(shL1p[0]), static_cast<float>(shL1p[1]), static_cast<float>(shL1p[2]), 0}, ibl.shL1_Mp1);
    ibl.hasSH = true;

    glm_vec3_copy(vec3{0.3f, 0.8f, -0.5f}, ibl.extractedSun.direction);
    glm_vec3_normalize(ibl.extractedSun.direction);
    glm_vec3_copy(vec3{1, 1, 1}, ibl.extractedSun.color);
    ibl.extractedSun.angularRadius = 0.0f;

    /* Dominant-hotspot clustering.
     *
     * The luminance-weighted centroid of EVERY pixel above the sun
     * threshold is only correct for single-sun sky HDRIs. Studio/interior
     * maps contain several bright windows, and the all-pixels centroid
     * then lands on a wall BETWEEN them — a "sun" that matches no actual
     * light, so the direct sun (shadow direction, sun disc, direct
     * specular) disagrees with the IBL reflection of the real hotspots
     * (terrain/character shine appears in a different direction than the
     * cast shadow). Instead: seed the sun at the brightest pixel and
     * integrate only the threshold excess within a small cone around it —
     * the dominant light source. */
    if (bestLum > sunThreshold && bestX >= 0) {
        vec3 seedDir;
        equirectPixelDir(bestX, bestY, width, height, seedDir);
        glm_vec3_normalize(seedDir);

        /* 15° cone: wide enough for a blown-out sun disc, narrow enough to
         * keep a second window (tens of degrees away) out of the cluster. */
        const float minDot = cosf(glm_rad(15.0f));

        double clusterDir[3] = {}, clusterRad[3] = {}, clusterWeight = 0.0;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int idx = (y * width + x) * 4;
                float r = pixels[idx + 0], g = pixels[idx + 1], b = pixels[idx + 2];
                float lum = luminance(r, g, b);
                if (lum <= sunThreshold) continue;

                vec3 d;
                equirectPixelDir(x, y, width, height, d);
                if (glm_vec3_dot(d, seedDir) < minDot) continue;

                float v          = ((float)y + 0.5f) / (float)height;
                float theta      = GLM_PIf * (0.5f - v);
                float cosTheta   = cosf(theta);
                float dTheta     = GLM_PIf / (float)height;
                float dPhi       = 2.0f * GLM_PIf / (float)width;
                float solidAngle = cosTheta * dTheta * dPhi;
                if (solidAngle <= 0.0f) continue;

                float scale = (lum - sunThreshold) / lum;
                float sr = r * scale, sg = g * scale, sb = b * scale;
                float sunWeight = luminance(sr, sg, sb) * solidAngle;
                clusterDir[0] += (double)d[0] * sunWeight;
                clusterDir[1] += (double)d[1] * sunWeight;
                clusterDir[2] += (double)d[2] * sunWeight;
                clusterRad[0] += sr * (double)solidAngle;
                clusterRad[1] += sg * (double)solidAngle;
                clusterRad[2] += sb * (double)solidAngle;
                clusterWeight += sunWeight;
            }
        }

        float dirLen = (float)sqrt(clusterDir[0] * clusterDir[0] + clusterDir[1] * clusterDir[1] +
                                   clusterDir[2] * clusterDir[2]);
        if (clusterWeight > 0.001 && dirLen > 0.001f) {
            ibl.extractedSun.direction[0] = (float)(clusterDir[0] / dirLen);
            ibl.extractedSun.direction[1] = (float)(clusterDir[1] / dirLen);
            ibl.extractedSun.direction[2] = (float)(clusterDir[2] / dirLen);
            ibl.extractedSun.color[0]     = (float)clusterRad[0];
            ibl.extractedSun.color[1]     = (float)clusterRad[1];
            ibl.extractedSun.color[2]     = (float)clusterRad[2];
            float normalizedLen           = (float)(dirLen / clusterWeight);
            float sunAngleCos             = 2.0f * normalizedLen - 1.0f;
            sunAngleCos                   = fmaxf(sunAngleCos, 0.001f);
            ibl.extractedSun.angularRadius = acosf(fminf(sunAngleCos, 1.0f));
            info("vulkanIbl: extracted sun (dominant hotspot, seed u=%.3f v=%.3f) — dir=(%.4f,%.4f,%.4f), color=(%.2f,%.2f,%.2f), radiance=(%.0f,%.0f,%.0f), angularRadius=%.4f",
                 ((double)bestX + 0.5) / width, ((double)bestY + 0.5) / height,
                 (double)ibl.extractedSun.direction[0], (double)ibl.extractedSun.direction[1],
                 (double)ibl.extractedSun.direction[2], (double)ibl.extractedSun.color[0],
                 (double)ibl.extractedSun.color[1], (double)ibl.extractedSun.color[2],
                 clusterRad[0], clusterRad[1], clusterRad[2],
                 (double)ibl.extractedSun.angularRadius);
            return;
        }
    }

    /* Fallback: no dominant hotspot found — use the all-pixels centroid. */
    if (sunWeightTotal > 0.001) {
        float dirLen = (float)sqrt(sunDirWeighted[0] * sunDirWeighted[0] +
                                   sunDirWeighted[1] * sunDirWeighted[1] +
                                   sunDirWeighted[2] * sunDirWeighted[2]);
        if (dirLen > 0.001f) {
            ibl.extractedSun.direction[0]  = (float)(sunDirWeighted[0] / dirLen);
            ibl.extractedSun.direction[1]  = (float)(sunDirWeighted[1] / dirLen);
            ibl.extractedSun.direction[2]  = (float)(sunDirWeighted[2] / dirLen);
            ibl.extractedSun.color[0]      = (float)sunRadiance[0];
            ibl.extractedSun.color[1]      = (float)sunRadiance[1];
            ibl.extractedSun.color[2]      = (float)sunRadiance[2];
            float normalizedLen            = (float)(dirLen / sunWeightTotal);
            float sunAngleCos              = 2.0f * normalizedLen - 1.0f;
            sunAngleCos                    = fmaxf(sunAngleCos, 0.001f);
            ibl.extractedSun.angularRadius = acosf(fminf(sunAngleCos, 1.0f));
            info("vulkanIbl: extracted sun — dir=(%.4f,%.4f,%.4f), color=(%.2f,%.2f,%.2f), radiance=(%.0f,%.0f,%.0f), angularRadius=%.4f",
                 (double)ibl.extractedSun.direction[0], (double)ibl.extractedSun.direction[1], (double)ibl.extractedSun.direction[2],
                 (double)ibl.extractedSun.color[0], (double)ibl.extractedSun.color[1], (double)ibl.extractedSun.color[2],
                 sunRadiance[0], sunRadiance[1], sunRadiance[2],
                 (double)ibl.extractedSun.angularRadius);
        }
    }
}

// ---------------------------------------------------------------------------
// Blue noise (void-and-cluster)
// ---------------------------------------------------------------------------

enum { BN_RADIUS = 4, BN_KSIZE = 2 * 4 + 1 };

static float bnKernel[BN_KSIZE * BN_KSIZE];
static char bnKernelReady;

static void bnInitKernel(void) {
    if (bnKernelReady) return;
    const float sigma = 1.5f, sigma2 = sigma * sigma;
    for (int dy = -BN_RADIUS; dy <= BN_RADIUS; dy++) {
        for (int dx = -BN_RADIUS; dx <= BN_RADIUS; dx++) {
            float v = (dx || dy) ? expf(-(float)(dx * dx + dy * dy) / (2.0f * sigma2)) : 0.0f;
            bnKernel[(dy + BN_RADIUS) * BN_KSIZE + (dx + BN_RADIUS)] = v;
        }
    }
    bnKernelReady = 1;
}

static void bnToggle(float* energy, u8* binary, u32 size, u32 idx, u8 state) {
    binary[idx] = state;
    u32 px = idx % size, py = idx / size;
    float sign = state ? 1.0f : -1.0f;
    for (int dy = -BN_RADIUS; dy <= BN_RADIUS; dy++) {
        for (int dx = -BN_RADIUS; dx <= BN_RADIUS; dx++) {
            float kv = bnKernel[(dy + BN_RADIUS) * BN_KSIZE + (dx + BN_RADIUS)];
            if (kv == 0.0f) continue;
            u32 nx = ((u32)((int)px + dx + (int)size)) % size;
            u32 ny = ((u32)((int)py + dy + (int)size)) % size;
            energy[ny * size + nx] += sign * kv;
        }
    }
}

static void generateDitherNoise(u8* pixels, u32 size) {
    bnInitKernel();
    const u32 n   = size * size;
    u8* binary = static_cast<u8*>(memoryAlloc(n));
    u32* ranking = static_cast<u32*>(memoryAlloc(n * sizeof(u32)));
    float* energy = static_cast<float*>(memoryAlloc(n * sizeof(float)));
    memset(binary, 0, n);
    memset(energy, 0, n * sizeof(float));

    u32 seedCount = n / 10, placed = 0;
    for (u32 i = 0; placed < seedCount && i < n * 4; i++) {
        u32 v = i * 0x8da6b343u ^ 0xcb1ab31fu;
        v ^= v >> 13;
        v *= 0x85ebca6bu;
        v ^= v >> 16;
        u32 idx = v % n;
        if (!binary[idx]) {
            bnToggle(energy, binary, size, idx, 1);
            placed++;
        }
    }

    u32 rank = placed;
    for (u32 step = 0; step < placed; step++) {
        float maxE = -1.0f;
        u32 best   = 0;
        for (u32 i = 0; i < n; i++) {
            if (binary[i] && energy[i] > maxE) {
                maxE = energy[i];
                best = i;
            }
        }
        rank--;
        ranking[best] = rank;
        bnToggle(energy, binary, size, best, 0);
    }

    rank = placed;
    for (u32 step = placed; step < n; step++) {
        float minE = 1e30f;
        u32 best   = 0;
        for (u32 i = 0; i < n; i++) {
            if (!binary[i] && energy[i] < minE) {
                minE = energy[i];
                best = i;
            }
        }
        ranking[best] = rank;
        bnToggle(energy, binary, size, best, 1);
        rank++;
    }

    for (u32 i = 0; i < n; i++) pixels[i] = (u8)((ranking[i] * 255u) / (n - 1));

    memoryFree(energy);
    memoryFree(ranking);
    memoryFree(binary);
}
