#include "Ibl.h"
#include "ExrLoader.h"
#include "VulkanResourceManager.h"
#include "VulkanBuffer.h"
#include "VulkanImage.h"
#include "../Vulkan.h"
#include "../command/VulkanCommand.h"
#include "../pipeline/VulkanPipe.h"
#include "Utils.h"
#include <math.h>

namespace engine {
static const char* STUDIOLIGHTS_DIR = "images/hdr";
static const char* ENVIRONMENT_PATH = "images/hdr/interior.exr";

static const float IBL_INTENSITY      = 1.0f;
static const float IBL_SPEC_INTENSITY = 0.5f;
static const float SUN_THRESHOLD       = 10.0f;

enum {
    IRRADIANCE_SIZE = 32,
    PREFILTER_SIZE  = 256,
    PREFILTER_MIPS  = 6,
    BRDF_LUT_SIZE   = 512,
};

struct VulkanIblResources {
    VulkanImage environment = {};
    VulkanImage irradiance = {};
    VulkanImage prefilter = {};
    VulkanImage brdfLut = {};
    float environmentMaxLod = 0.0f;
    float prefilterMaxLod = 0.0f;
    vec4 shL0_M0 = {};
    vec4 shL1_Mn1 = {};
    vec4 shL1_M0 = {};
    vec4 shL1_Mp1 = {};
    char hasSH = 0;
    RendererSunLight extractedSun = {};
    mat4 envRotation = {};
    char enabled = 0;
    float intensity = 0.0f;
    float specularIntensity = 0.0f;
    char ready = 0;
};

static VulkanIblResources ibl;
static VulkanPipe irradiancePipe = {};
static VulkanPipe prefilterPipe = {};
static VulkanPipe brdfPipe = {};
static char pipesReady = 0;

static std::vector<std::string> iblFiles;
static int iblCurrentIndex = 0;
static char iblFileListReady = 0;

struct IblFacePushConstants {
    u32 environmentMapIndex;
    u32 faceIndex;
    float roughness;
    float pad;
};

static void pushIblState(void);
static void buildIblFileList(void);
static void loadEnvironmentFromPath(const std::string& path);
static void uploadEnvironment(const float* pixels, int width, int height, u32 mipLevels);
static void createEnvironmentDependentImages(void);
static void createBrdfLut(void);
static void createPipelines(void);
static void precomputeIbl(char includeBrdf);
static void destroyPipelines(void);
static void destroyImage(VulkanImage* image);
static VkImageView createAttachmentView(VulkanImage* image, u32 baseMipLevel, u32 baseArrayLayer);
static VulkanImage makeAttachmentProxy(VulkanImage* image, VkImageView view, u32 mipLevel);
static void renderBrdfLut(VulkanCommand* cmd);
static void renderIrradiance(VulkanCommand* cmd, u32 environmentMapIndex);
static void renderPrefilter(VulkanCommand* cmd, u32 environmentMapIndex);
static void extractSHAndSun(const float* pixels, int width, int height, float sunThreshold);
static u32 calculateMipLevels(int width, int height);

static void pushIblState(void) {
    VulkanIblData data = {};
    data.environmentMapIndex  = static_cast<u32>(ibl.environment.sampledPoolIndex);
    data.irradianceMapIndex   = static_cast<u32>(ibl.irradiance.sampledPoolIndex);
    data.prefilterMapIndex    = static_cast<u32>(ibl.prefilter.sampledPoolIndex);
    data.brdfLutIndex         = static_cast<u32>(ibl.brdfLut.sampledPoolIndex);
    data.environmentMapMaxLod = ibl.environmentMaxLod;
    data.prefilterMapMaxLod   = ibl.prefilterMaxLod;
    data.enabled              = static_cast<u32>(ibl.enabled);
    data.intensity            = ibl.intensity;
    data.specularIntensity    = ibl.specularIntensity;
    data.hasSH                = static_cast<u32>(ibl.hasSH);
    glm_vec4_copy(ibl.shL0_M0, data.shL0_M0);
    glm_vec4_copy(ibl.shL1_Mn1, data.shL1_Mn1);
    glm_vec4_copy(ibl.shL1_M0, data.shL1_M0);
    glm_vec4_copy(ibl.shL1_Mp1, data.shL1_Mp1);
    glm_mat4_copy(ibl.envRotation, data.envRotation);
    vulkanResourceSetIblData(&data);
}

static void buildIblFileList(void) {
    if (iblFileListReady) return;

    auto addDir = [&](const char* extension) {
        std::vector<utils::String> files = utils::dataManagerListFiles(extension);
        for (utils::String& s : files) {
            if (utils::strStartsWith(s.data, STUDIOLIGHTS_DIR)) {
                iblFiles.emplace_back(s.data);
            }
        }
    };
    addDir(".hdr");
    addDir(".exr");

    iblCurrentIndex = 0;
    for (size_t i = 0; i < iblFiles.size(); i++) {
        if (iblFiles[i] == ENVIRONMENT_PATH) {
            iblCurrentIndex = static_cast<int>(i);
            break;
        }
    }

    iblFileListReady = 1;
    utils::info("vulkanIbl: found %zu IBL files in %s", iblFiles.size(), STUDIOLIGHTS_DIR);
}

static void loadEnvironmentFromPath(const std::string& path) {
    if (!utils::dataManagerFileExists(path.c_str())) {
        utils::warn("vulkanIbl: environment map not found: %s", path.c_str());
        if (!ibl.ready) vulkanResourceSetIblData(nullptr);
        return;
    }

    utils::String fileData = utils::dataManagerRead(path.c_str());
    if (!fileData.data || fileData.size == 0) {
        utils::warn("vulkanIbl: failed to read %s", path.c_str());
        utils::stringDestroy(&fileData);
        if (!ibl.ready) vulkanResourceSetIblData(nullptr);
        return;
    }

    int width = 0;
    int height = 0;
    std::vector<float> pixels;

    if (utils::strEndsWithC(path.c_str(), "exr")) {
        pixels = exrLoadFromMemory(fileData.data, fileData.size, &width, &height);
    } else {
        int channelsInFile = 0;
        float* stbiPixels = stbi_loadf_from_memory((const unsigned char*)fileData.data,
                                                   (int)fileData.size,
                                                   &width,
                                                   &height,
                                                   &channelsInFile,
                                                   4);
        if (stbiPixels) {
            pixels.assign(stbiPixels, stbiPixels + (size_t)width * height * 4);
            stbi_image_free(stbiPixels);
        }
    }
    utils::stringDestroy(&fileData);

    if (pixels.empty() || width <= 0 || height <= 0) {
        utils::warn("vulkanIbl: failed to decode HDR image %s", path.c_str());
        if (!ibl.ready) vulkanResourceSetIblData(nullptr);
        return;
    }

    if (ibl.ready) {
        rendererWaitIdle("IBL environment reload");
        destroyImage(&ibl.prefilter);
        destroyImage(&ibl.irradiance);
        destroyImage(&ibl.environment);
    }

    extractSHAndSun(pixels.data(), width, height, SUN_THRESHOLD);

    u32 mipLevels = calculateMipLevels(width, height);
    uploadEnvironment(pixels.data(), width, height, mipLevels);

    ibl.environmentMaxLod = static_cast<float>(mipLevels - 1);
    ibl.prefilterMaxLod   = static_cast<float>(PREFILTER_MIPS - 1);

    char firstTime = !ibl.ready;

    createPipelines();
    createEnvironmentDependentImages();
    if (firstTime) {
        createBrdfLut();
    }
    precomputeIbl(firstTime);
    destroyPipelines();

    if (firstTime) {
        ibl.ready             = 1;
        ibl.enabled           = 1;
        ibl.intensity         = IBL_INTENSITY;
        ibl.specularIntensity = IBL_SPEC_INTENSITY;
    }

    glm_mat4_identity(ibl.envRotation);

    pushIblState();
    utils::info("vulkanIbl: loaded %s (%dx%d envLod=%.0f)", path.c_str(), width, height,
                (double)ibl.environmentMaxLod);

    utils::signalEmit("iblChanged", nullptr);
}

void vulkanIblInit(void) {
    if (ibl.ready) return;
    buildIblFileList();
    loadEnvironmentFromPath(ENVIRONMENT_PATH);
}

void vulkanIblDestroy(void) {
    destroyImage(&ibl.brdfLut);
    destroyImage(&ibl.prefilter);
    destroyImage(&ibl.irradiance);
    destroyImage(&ibl.environment);
    destroyPipelines();
    ibl = {};
    iblFiles.clear();
    iblFileListReady = 0;
}

void vulkanIblCycleNext(void) {
    if (iblFiles.size() < 2) return;
    iblCurrentIndex = (iblCurrentIndex + 1) % static_cast<int>(iblFiles.size());
    loadEnvironmentFromPath(iblFiles[static_cast<size_t>(iblCurrentIndex)]);
}

void vulkanIblCyclePrev(void) {
    if (iblFiles.size() < 2) return;
    iblCurrentIndex = (iblCurrentIndex + static_cast<int>(iblFiles.size()) - 1) %
                      static_cast<int>(iblFiles.size());
    loadEnvironmentFromPath(iblFiles[static_cast<size_t>(iblCurrentIndex)]);
}

const char* vulkanIblGetCurrentName(void) {
    if (iblFiles.empty()) return "(none)";
    const std::string& path = iblFiles[static_cast<size_t>(iblCurrentIndex)];
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path.c_str() : path.c_str() + slash + 1;
}

char vulkanIblIsReady(void) {
    return ibl.ready;
}

RendererSunLight vulkanIblGetExtractedSun(void) {
    return ibl.extractedSun;
}

void vulkanIblSetDisabled(char disabled) {
    ibl.enabled = !disabled;
    if (!ibl.ready) return;
    pushIblState();
    utils::signalEmit("iblChanged", nullptr);
}

char vulkanIblIsDisabled(void) {
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

void vulkanIblRotateSun(float azimuthDeg, float elevationDeg) {
    vec3 dir;
    glm_vec3_copy(ibl.extractedSun.direction, dir);

    mat4 stepRot;
    glm_mat4_identity(stepRot);

    if (fabsf(azimuthDeg) > 0.001f) {
        float rad = azimuthDeg * (M_PI / 180.0f);
        vec3 yAxis = {0.0f, 1.0f, 0.0f};
        mat4 rot;
        glm_rotate_make(rot, rad, yAxis);
        glm_mat4_mul(rot, stepRot, stepRot);
        vec3 tmp;
        glm_mat4_mulv3(stepRot, dir, 1.0f, tmp);
        glm_vec3_copy(tmp, dir);
    }

    if (fabsf(elevationDeg) > 0.001f) {
        float rad   = elevationDeg * (M_PI / 180.0f);
        vec3 dirXZ  = {dir[0], 0.0f, dir[2]};
        float lenXZ = glm_vec3_norm(dirXZ);
        if (lenXZ > 0.001f) {
            vec3 yAxis = {0.0f, 1.0f, 0.0f};
            vec3 right;
            glm_vec3_cross(yAxis, dirXZ, right);
            glm_vec3_normalize(right);
            mat4 rot;
            glm_rotate_make(rot, rad, right);
            glm_mat4_mul(rot, stepRot, stepRot);
            vec3 tmp;
            glm_mat4_mulv3(stepRot, dir, 1.0f, tmp);
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
    utils::signalEmit("iblChanged", nullptr);
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
        vulkanCreateImage(.name      = "IBL Environment Equirect",
                          .format    = VK_FORMAT_R32G32B32A32_SFLOAT,
                          .usage     = VK_IMAGE_USAGE_SAMPLED_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          .mipLevels = static_cast<int>(mipLevels),
                          .width     = width,
                          .height    = height);

    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanTransition(cmd, &ibl.environment, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);
    vulkanCopy(.cmd         = cmd,
               .target.img  = &ibl.environment,
               .source.data = const_cast<void*>(static_cast<const void*>(pixels)),
               .size        = static_cast<u32>((u64)width * height * 4 * sizeof(float)));
    if (mipLevels > 1) {
        vulkanImgGenerateMips(cmd, &ibl.environment);
    } else {
        vulkanTransition(cmd, &ibl.environment, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    }
    vulkanTransientEnd(cmd, 1);

    vulkanAddImageToPool(&ibl.environment);
}

static void createEnvironmentDependentImages(void) {
    ibl.irradiance =
        vulkanCreateImage(.name      = "IBL Irradiance",
                          .format    = VK_FORMAT_R16G16B16A16_SFLOAT,
                          .usage     = VK_IMAGE_USAGE_SAMPLED_BIT |
                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                          .width     = IRRADIANCE_SIZE,
                          .height    = IRRADIANCE_SIZE,
                          .layers    = 6,
                          .flags     = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                          .viewType  = VK_IMAGE_VIEW_TYPE_CUBE);

    ibl.prefilter =
        vulkanCreateImage(.name      = "IBL Prefilter",
                          .format    = VK_FORMAT_R16G16B16A16_SFLOAT,
                          .usage     = VK_IMAGE_USAGE_SAMPLED_BIT |
                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                          .width     = PREFILTER_SIZE,
                          .height    = PREFILTER_SIZE,
                          .layers    = 6,
                          .mipLevels = PREFILTER_MIPS,
                          .flags     = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                          .viewType  = VK_IMAGE_VIEW_TYPE_CUBE);

    vulkanAddImageToPool(&ibl.irradiance);
    vulkanAddImageToPool(&ibl.prefilter);
}

static void createBrdfLut(void) {
    ibl.brdfLut =
        vulkanCreateImage(.name   = "IBL BRDF LUT",
                          .format = VK_FORMAT_R16G16_SFLOAT,
                          .usage  = VK_IMAGE_USAGE_SAMPLED_BIT |
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                          .width  = BRDF_LUT_SIZE,
                          .height = BRDF_LUT_SIZE);

    vulkanAddImageToPool(&ibl.brdfLut);
}

static void createPipelines(void) {
    if (pipesReady) return;

    irradiancePipe = vulkanCreatePipe(.name                 = "ibl_irradiance",
                                     .vs                   = "shaders/pass/ibl/spv/fullscreen.vert.spv",
                                     .fs                   = "shaders/pass/ibl/spv/irradiance.frag.spv",
                                     .colorFormat1         = VK_FORMAT_R16G16B16A16_SFLOAT,
                                     .clearColor1Enabled   = 1,
                                     .clearColor1          = {0, 0, 0, 0});

    prefilterPipe = vulkanCreatePipe(.name                 = "ibl_prefilter",
                                     .vs                   = "shaders/pass/ibl/spv/fullscreen.vert.spv",
                                     .fs                   = "shaders/pass/ibl/spv/prefilter.frag.spv",
                                     .colorFormat1         = VK_FORMAT_R16G16B16A16_SFLOAT,
                                     .clearColor1Enabled   = 1,
                                     .clearColor1          = {0, 0, 0, 0});

    brdfPipe = vulkanCreatePipe(.name                 = "ibl_brdf_lut",
                                 .vs                   = "shaders/pass/ibl/spv/fullscreen.vert.spv",
                                 .fs                   = "shaders/pass/ibl/spv/brdf_lut.frag.spv",
                                 .colorFormat1         = VK_FORMAT_R16G16_SFLOAT,
                                 .clearColor1Enabled   = 1,
                                 .clearColor1          = {0, 0, 0, 0});

    pipesReady = 1;
}

static void precomputeIbl(char includeBrdf) {
    VulkanCommand* cmd = vulkanTransientBegin();

    vulkanTransition(cmd, &ibl.irradiance, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    vulkanTransition(cmd, &ibl.prefilter, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    if (includeBrdf) {
        vulkanTransition(cmd, &ibl.brdfLut, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    }
    vulkanTransition(cmd, &ibl.environment, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    if (includeBrdf) renderBrdfLut(cmd);
    renderIrradiance(cmd, static_cast<u32>(ibl.environment.sampledPoolIndex));
    renderPrefilter(cmd, static_cast<u32>(ibl.environment.sampledPoolIndex));

    vulkanTransition(cmd, &ibl.irradiance, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, &ibl.prefilter, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    if (includeBrdf) {
        vulkanTransition(cmd, &ibl.brdfLut, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    }

    vulkanTransientEnd(cmd, 1);
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
    VulkanImage proxy = *image;
    proxy.view        = view;
    proxy.views.clear();
    proxy.layers      = 1;
    proxy.mipLevels   = 1;
    proxy.extent.width  = static_cast<uint32_t>(std::max(1u, image->extent.width >> mipLevel));
    proxy.extent.height = static_cast<uint32_t>(std::max(1u, image->extent.height >> mipLevel));
    proxy.extent.depth  = 1;
    return proxy;
}

static void renderBrdfLut(VulkanCommand* cmd) {
    VkImageView view = createAttachmentView(&ibl.brdfLut, 0, 0);
    VulkanImage target = makeAttachmentProxy(&ibl.brdfLut, view, 0);
    vulkanBeginRender(.cmd   = cmd, .pipe = &brdfPipe, .color1 = &target);
    vulkanViewport(cmd, 0, target.extent.height, target.extent.width,
                   -((i32)target.extent.height));
    vulkanScissor(cmd, 0, 0, target.extent.width, target.extent.height);
    vulkanBindPipe(cmd, &brdfPipe);
    vulkanDraw(cmd, 3, 1);
    vulkanEndRender(cmd);
    vkDestroyImageView(vulkan.device, view, nullptr);
}

static void renderIrradiance(VulkanCommand* cmd, u32 environmentMapIndex) {
    std::vector<VkImageView> tempViews;
    for (u32 face = 0; face < 6; face++) {
        VkImageView view = createAttachmentView(&ibl.irradiance, 0, face);
        tempViews.push_back(view);
        VulkanImage target = makeAttachmentProxy(&ibl.irradiance, view, 0);
        vulkanBeginRender(.cmd   = cmd, .pipe = &irradiancePipe, .color1 = &target);
        vulkanViewport(cmd, 0, target.extent.height, target.extent.width,
                       -((i32)target.extent.height));
        vulkanScissor(cmd, 0, 0, target.extent.width, target.extent.height);
        vulkanBindPipe(cmd, &irradiancePipe);
        IblFacePushConstants pc = {.environmentMapIndex = environmentMapIndex,
                                   .faceIndex          = face,
                                   .roughness          = 0.0f,
                                   .pad                = 0.0f};
        vulkanPush(cmd, &irradiancePipe, sizeof(pc), &pc);
        vulkanDraw(cmd, 3, 1);
        vulkanEndRender(cmd);
    }
    for (VkImageView view : tempViews) {
        vkDestroyImageView(vulkan.device, view, nullptr);
    }
}

static void renderPrefilter(VulkanCommand* cmd, u32 environmentMapIndex) {
    std::vector<VkImageView> tempViews;
    for (u32 mip = 0; mip < PREFILTER_MIPS; mip++) {
        float roughness = PREFILTER_MIPS > 1 ? static_cast<float>(mip) / static_cast<float>(PREFILTER_MIPS - 1)
                                             : 0.0f;
        for (u32 face = 0; face < 6; face++) {
            VkImageView view = createAttachmentView(&ibl.prefilter, mip, face);
            tempViews.push_back(view);
            VulkanImage target = makeAttachmentProxy(&ibl.prefilter, view, mip);
            vulkanBeginRender(.cmd   = cmd, .pipe = &prefilterPipe, .color1 = &target);
            vulkanViewport(cmd, 0, target.extent.height, target.extent.width,
                           -((i32)target.extent.height));
            vulkanScissor(cmd, 0, 0, target.extent.width, target.extent.height);
            vulkanBindPipe(cmd, &prefilterPipe);
            IblFacePushConstants pc = {.environmentMapIndex = environmentMapIndex,
                                       .faceIndex          = face,
                                       .roughness          = roughness,
                                       .pad                = 0.0f};
            vulkanPush(cmd, &prefilterPipe, sizeof(pc), &pc);
            vulkanDraw(cmd, 3, 1);
            vulkanEndRender(cmd);
        }
    }
    for (VkImageView view : tempViews) {
        vkDestroyImageView(vulkan.device, view, nullptr);
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
    float theta    = (float)M_PI * (0.5f - v);
    float phi      = 2.0f * (float)M_PI * (u - 0.5f);
    float cosTheta = cosf(theta);
    out[0]         = cosTheta * cosf(phi);
    out[1]         = sinf(theta);
    out[2]         = cosTheta * sinf(phi);
}

static void extractSHAndSun(const float* pixels, int width, int height, float sunThreshold) {
    const float Y00 = 0.282095f;
    const float Y1x = 0.488603f;

    double shL0[3] = {0}, shL1n[3] = {0}, shL10[3] = {0}, shL1p[3] = {0};
    double sunRadiance[3] = {0}, sunDirWeighted[3] = {0}, sunWeightTotal = 0.0;

    /* Brightest pixel — seed of the dominant-hotspot sun cluster. */
    float bestLum = 0.0f;
    int bestX = -1, bestY = -1;

    for (int y = 0; y < height; y++) {
        float v        = ((float)y + 0.5f) / (float)height;
        float theta    = (float)M_PI * (0.5f - v);
        float cosTheta = cosf(theta);
        float sinTheta = sinf(theta);
        float dTheta   = (float)M_PI / (float)height;
        float dPhi      = 2.0f * (float)M_PI / (float)width;
        float solidAngle = cosTheta * dTheta * dPhi;
        if (solidAngle <= 0.0f) continue;

        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;
            float r = pixels[idx + 0], g = pixels[idx + 1], b = pixels[idx + 2];
            float u   = ((float)x + 0.5f) / (float)width;
            float phi = 2.0f * (float)M_PI * (u - 0.5f);
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

    {
        vec4 v = {(float)shL0[0], (float)shL0[1], (float)shL0[2], 0.0f};
        glm_vec4_copy(v, ibl.shL0_M0);
    }
    {
        vec4 v = {(float)shL1n[0], (float)shL1n[1], (float)shL1n[2], 0.0f};
        glm_vec4_copy(v, ibl.shL1_Mn1);
    }
    {
        vec4 v = {(float)shL10[0], (float)shL10[1], (float)shL10[2], 0.0f};
        glm_vec4_copy(v, ibl.shL1_M0);
    }
    {
        vec4 v = {(float)shL1p[0], (float)shL1p[1], (float)shL1p[2], 0.0f};
        glm_vec4_copy(v, ibl.shL1_Mp1);
    }
    ibl.hasSH = 1;

    vec3 fallbackDir = {0.3f, 0.8f, -0.5f};
    glm_vec3_copy(fallbackDir, ibl.extractedSun.direction);
    glm_vec3_normalize(ibl.extractedSun.direction);
    vec3 white = {1.0f, 1.0f, 1.0f};
    glm_vec3_copy(white, ibl.extractedSun.color);
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
        const float minDot = cosf(15.0f * (M_PI / 180.0f));

        double clusterDir[3] = {0}, clusterRad[3] = {0}, clusterWeight = 0.0;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int idx = (y * width + x) * 4;
                float r = pixels[idx + 0], g = pixels[idx + 1], b = pixels[idx + 2];
                float lum = luminance(r, g, b);
                if (lum <= sunThreshold) continue;

                vec3 d;
                equirectPixelDir(x, y, width, height, d);
                if (glm_vec3_dot(d, seedDir) < minDot) continue;

                float v        = ((float)y + 0.5f) / (float)height;
                float theta    = (float)M_PI * (0.5f - v);
                float cosTheta = cosf(theta);
                float dTheta   = (float)M_PI / (float)height;
                float dPhi      = 2.0f * (float)M_PI / (float)width;
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
            utils::info("vulkanIbl: extracted sun (dominant hotspot, seed u=%.3f v=%.3f) — dir=(%.4f,%.4f,%.4f), color=(%.2f,%.2f,%.2f), angularRadius=%.4f",
                 ((double)bestX + 0.5) / width, ((double)bestY + 0.5) / height,
                 (double)ibl.extractedSun.direction[0], (double)ibl.extractedSun.direction[1],
                 (double)ibl.extractedSun.direction[2], (double)ibl.extractedSun.color[0],
                 (double)ibl.extractedSun.color[1], (double)ibl.extractedSun.color[2],
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
            utils::info("vulkanIbl: extracted sun — dir=(%.4f,%.4f,%.4f), color=(%.2f,%.2f,%.2f), angularRadius=%.4f",
                 (double)ibl.extractedSun.direction[0], (double)ibl.extractedSun.direction[1],
                 (double)ibl.extractedSun.direction[2], (double)ibl.extractedSun.color[0],
                 (double)ibl.extractedSun.color[1], (double)ibl.extractedSun.color[2],
                 (double)ibl.extractedSun.angularRadius);
        }
    }
}
}  // namespace engine
