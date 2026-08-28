#include "renderer/vulkan/resources/VulkanIbl.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "datamanager/DataManager.h"
#include "events/Events.h"
#include "logger/Logger.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/ExrLoader.h"
#include "renderer/vulkan/resources/VulkanBuffer.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"

namespace engine {

    static const char* STUDIOLIGHTS_DIR = "images/studiolights";

    enum {
        IRRADIANCE_SIZE = 32,
        PREFILTER_SIZE  = 256,
        PREFILTER_MIPS  = 6,
        BRDF_LUT_SIZE   = 512,
    };

    static const float SUN_THRESHOLD      = 10.0f;
    static const float IBL_INTENSITY      = 1.0f;
    static const float IBL_SPEC_INTENSITY = 0.5f;

    struct VulkanIblResources {
        VulkanImage environment;
        VulkanImage irradiance;
        VulkanImage prefilter;
        VulkanImage brdfLut;
        float environmentMaxLod;
        float prefilterMaxLod;
        vec4 shL0_M0;
        vec4 shL1_Mn1;
        vec4 shL1_M0;
        vec4 shL1_Mp1;
        bool hasSH;
        RendererSunLight extractedSun;
        mat4 envRotation;
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

    static std::vector<std::string> iblFiles;
    static int iblCurrentIndex;
    static bool iblFileListReady;

    static u32 calculateMipLevels(int width, int height);
    static void uploadEnvironment(const float* pixels, int width, int height, u32 mipLevels);
    static void createEnvironmentDependentImages(void);
    static void createBrdfLut(void);
    static void createPipelines(void);
    static void precomputeIbl(bool includeBrdf);
    static void destroyPipelines(void);
    static void destroyIblImage(VulkanImage* image);
    static VkImageView createAttachmentView(VulkanImage* image,
                                            u32 baseMipLevel,
                                            u32 baseArrayLayer);
    static VulkanImage makeAttachmentProxy(VulkanImage* image, VkImageView view, u32 mipLevel);
    static void renderBrdfLut(VulkanCommand* cmd, std::vector<VkImageView>& tempViews);
    static void renderIrradiance(VulkanCommand* cmd,
                                 std::vector<VkImageView>& tempViews,
                                 u32 environmentMapIndex);
    static void renderPrefilter(VulkanCommand* cmd,
                                std::vector<VkImageView>& tempViews,
                                u32 environmentMapIndex);
    static void extractSHAndSun(const float* pixels, int width, int height, float sunThreshold);
    static void pushIblState(void);
    static void buildIblFileList(void);
    static void loadEnvironmentFromPath(const char* path);

    static void pushIblState(void) {
        VulkanIblData data;
        data.environmentMapIndex   = (u32)ibl.environment.sampledPoolIndex;
        data.irradianceMapIndex    = (u32)ibl.irradiance.sampledPoolIndex;
        data.prefilterMapIndex     = (u32)ibl.prefilter.sampledPoolIndex;
        data.brdfLutIndex          = (u32)ibl.brdfLut.sampledPoolIndex;
        data.blueNoiseIndex        = 0;  // unused: FFX LPM does tone mapping
        data.tonemapLutIndex       = 0;
        data.environmentMapMaxLod  = ibl.environmentMaxLod;
        data.prefilterMapMaxLod    = ibl.prefilterMaxLod;
        data.enabled               = ibl.enabled ? 1 : 0;
        data.intensity             = ibl.intensity;
        data.specularIntensity     = ibl.specularIntensity;
        data.sunThreshold          = SUN_THRESHOLD;
        data.hasSH                 = ibl.hasSH ? 1 : 0;
        data.tonemapMode           = 0;
        data.tonemapLutPunchyIndex = 0;
        data.pad_ibl2              = 0;
        glm_vec4_copy(ibl.shL0_M0, data.shL0_M0);
        glm_vec4_copy(ibl.shL1_Mn1, data.shL1_Mn1);
        glm_vec4_copy(ibl.shL1_M0, data.shL1_M0);
        glm_vec4_copy(ibl.shL1_Mp1, data.shL1_Mp1);
        glm_mat4_copy(ibl.envRotation, data.envRotation);
        vulkanResourceSetIbl(&data);
    }

    static void buildIblFileList(void) {
        if (iblFileListReady) return;

        std::vector<utils::String> hdrFiles = utils::dataManagerListFiles(".hdr");
        for (utils::String& s : hdrFiles) {
            if (utils::strStartsWith(s.data, STUDIOLIGHTS_DIR)) {
                iblFiles.push_back(s.data);
            }
            utils::stringDestroy(&s);
        }

        std::vector<utils::String> exrFiles = utils::dataManagerListFiles(".exr");
        for (utils::String& s : exrFiles) {
            if (utils::strStartsWith(s.data, STUDIOLIGHTS_DIR)) {
                iblFiles.push_back(s.data);
            }
            utils::stringDestroy(&s);
        }

        iblCurrentIndex  = 0;
        iblFileListReady = true;
        utils::info("vulkanIbl: found %d IBL files in %s", (int)iblFiles.size(), STUDIOLIGHTS_DIR);
    }

    static void loadEnvironmentFromPath(const char* path) {
        if (!utils::dataManagerFileExists(path)) {
            utils::warn("vulkanIbl: environment map not found: %s", path);
            return;
        }

        utils::String fileData = utils::dataManagerRead(path);
        if (!fileData.data || fileData.size == 0) {
            utils::warn("vulkanIbl: failed to read %s", path);
            utils::stringDestroy(&fileData);
            return;
        }

        int width  = 0;
        int height = 0;
        std::vector<float> exrPixels;
        float* hdrPixels = nullptr;

        if (utils::strEndsWithC(path, "exr")) {
            exrPixels = exrLoadFromMemory(fileData.data, fileData.size, &width, &height);
        } else {
            int channelsInFile = 0;
            hdrPixels          = stbi_loadf_from_memory((const stbi_uc*)fileData.data,
                                                        (int)fileData.size,
                                                        &width,
                                                        &height,
                                                        &channelsInFile,
                                                        4);
        }
        utils::stringDestroy(&fileData);

        const float* pixels = hdrPixels ? hdrPixels : exrPixels.data();
        if (!pixels || width <= 0 || height <= 0) {
            utils::warn("vulkanIbl: failed to decode HDR image %s", path);
            if (hdrPixels) free(hdrPixels);
            return;
        }

        if (ibl.ready) {
            rendererWaitIdle("IBL environment reload");
            destroyIblImage(&ibl.prefilter);
            destroyIblImage(&ibl.irradiance);
            destroyIblImage(&ibl.environment);
        }

        extractSHAndSun(pixels, width, height, SUN_THRESHOLD);

        u32 mipLevels = calculateMipLevels(width, height);
        uploadEnvironment(pixels, width, height, mipLevels);
        if (hdrPixels) free(hdrPixels);

        ibl.environmentMaxLod = (float)(mipLevels - 1);
        ibl.prefilterMaxLod   = (float)(PREFILTER_MIPS - 1);

        bool firstTime = !ibl.ready;

        createPipelines();
        createEnvironmentDependentImages();
        if (firstTime) {
            createBrdfLut();
        }
        precomputeIbl(firstTime);
        destroyPipelines();

        if (firstTime) {
            ibl.ready             = true;
            ibl.enabled           = true;
            ibl.intensity         = IBL_INTENSITY;
            ibl.specularIntensity = IBL_SPEC_INTENSITY;
        }

        glm_mat4_identity(ibl.envRotation);

        pushIblState();
        utils::info("vulkanIbl: loaded %s (%dx%d envLod=%.0f)",
                    path,
                    width,
                    height,
                    ibl.environmentMaxLod);

        utils::signalEmit("iblChanged", nullptr);
    }

    void vulkanIblInit(void) {
        if (ibl.ready) return;
        buildIblFileList();
        if (iblFiles.empty()) {
            utils::warn("vulkanIbl: no IBL files found in %s", STUDIOLIGHTS_DIR);
            return;
        }
        loadEnvironmentFromPath(iblFiles[iblCurrentIndex].c_str());
    }

    void vulkanIblDestroy(void) {
        destroyIblImage(&ibl.brdfLut);
        destroyIblImage(&ibl.prefilter);
        destroyIblImage(&ibl.irradiance);
        destroyIblImage(&ibl.environment);
        destroyPipelines();

        ibl = VulkanIblResources{};
        iblFiles.clear();
        iblFileListReady = false;
    }

    void vulkanIblCycleNext(void) {
        if (iblFiles.size() < 2) return;
        iblCurrentIndex = (iblCurrentIndex + 1) % (int)iblFiles.size();
        loadEnvironmentFromPath(iblFiles[iblCurrentIndex].c_str());
    }

    void vulkanIblCyclePrev(void) {
        if (iblFiles.size() < 2) return;
        iblCurrentIndex = (iblCurrentIndex + (int)iblFiles.size() - 1) % (int)iblFiles.size();
        loadEnvironmentFromPath(iblFiles[iblCurrentIndex].c_str());
    }

    const char* vulkanIblGetCurrentName(void) {
        if (iblFiles.empty()) return "(none)";
        const std::string& path = iblFiles[iblCurrentIndex];
        const char* slash       = strrchr(path.c_str(), '/');
        return slash ? slash + 1 : path.c_str();
    }

    VulkanImage* vulkanIblGetEnvironmentImage(void) {
        return ibl.environment.img ? &ibl.environment : nullptr;
    }

    RendererSunLight vulkanIblGetExtractedSun(void) {
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

    void vulkanIblRotateSun(float azimuthDeg, float elevationDeg) {
        vec3 dir;
        glm_vec3_copy(ibl.extractedSun.direction, dir);

        mat4 stepRot;
        glm_mat4_identity(stepRot);

        if (fabsf(azimuthDeg) > 0.001f) {
            float rad = glm_rad(azimuthDeg);
            mat4 rot;
            vec3 up = {0.0f, 1.0f, 0.0f};
            glm_rotate_make(rot, rad, up);
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
                vec3 up = {0.0f, 1.0f, 0.0f};
                vec3 right;
                glm_vec3_cross(up, dirXZ, right);
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
        ibl.environment = vulkanCreateImage(.name      = "IBL Environment Equirect",
                                            .format    = VK_FORMAT_R32G32B32A32_SFLOAT,
                                            .usage     = VK_IMAGE_USAGE_SAMPLED_BIT |
                                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT |
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
        ibl.irradiance = vulkanCreateImage(.name     = "IBL Irradiance",
                                           .format   = VK_FORMAT_R16G16B16A16_SFLOAT,
                                           .usage    = VK_IMAGE_USAGE_SAMPLED_BIT |
                                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                           .width    = IRRADIANCE_SIZE,
                                           .height   = IRRADIANCE_SIZE,
                                           .layers   = 6,
                                           .flags    = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                                           .viewType = VK_IMAGE_VIEW_TYPE_CUBE);

        ibl.prefilter = vulkanCreateImage(.name      = "IBL Prefilter",
                                          .format    = VK_FORMAT_R16G16B16A16_SFLOAT,
                                          .usage     = VK_IMAGE_USAGE_SAMPLED_BIT |
                                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                          .width     = PREFILTER_SIZE,
                                          .height    = PREFILTER_SIZE,
                                          .layers    = 6,
                                          .mipLevels = PREFILTER_MIPS,
                                          .flags     = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                                          .viewType  = VK_IMAGE_VIEW_TYPE_CUBE);
    }

    static void createBrdfLut(void) {
        ibl.brdfLut = vulkanCreateImage(.name   = "IBL BRDF LUT",
                                        .format = VK_FORMAT_R16G16_SFLOAT,
                                        .usage  = VK_IMAGE_USAGE_SAMPLED_BIT |
                                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                        .width  = BRDF_LUT_SIZE,
                                        .height = BRDF_LUT_SIZE);
    }

    static void createPipelines(void) {
        if (pipesReady) return;

        irradiancePipe = vulkanCreatePipe(.name = "ibl_irradiance",
                                          .vs   = "shaders/pass/ibl/spv/fullscreen.vert.spv",
                                          .fs   = "shaders/pass/ibl/spv/irradiance.frag.spv",
                                          .colorFormat1       = VK_FORMAT_R16G16B16A16_SFLOAT,
                                          .clearColor1Enabled = true,
                                          .clearColor1        = {0, 0, 0, 0});

        prefilterPipe = vulkanCreatePipe(.name         = "ibl_prefilter",
                                         .vs           = "shaders/pass/ibl/spv/fullscreen.vert.spv",
                                         .fs           = "shaders/pass/ibl/spv/prefilter.frag.spv",
                                         .colorFormat1 = VK_FORMAT_R16G16B16A16_SFLOAT,
                                         .clearColor1Enabled = true,
                                         .clearColor1        = {0, 0, 0, 0});

        brdfPipe = vulkanCreatePipe(.name         = "ibl_brdf_lut",
                                    .vs           = "shaders/pass/ibl/spv/fullscreen.vert.spv",
                                    .fs           = "shaders/pass/ibl/spv/brdf_lut.frag.spv",
                                    .colorFormat1 = VK_FORMAT_R16G16_SFLOAT,
                                    .clearColor1Enabled = true,
                                    .clearColor1        = {0, 0, 0, 0});

        pipesReady = true;
    }

    static void precomputeIbl(bool includeBrdf) {
        std::vector<VkImageView> tempViews;
        VulkanCommand* cmd = vulkanTransientBegin();

        vulkanTransition(cmd, &ibl.irradiance, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
        vulkanTransition(cmd, &ibl.prefilter, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
        if (includeBrdf) {
            vulkanTransition(cmd, &ibl.brdfLut, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
        }
        vulkanTransition(cmd, &ibl.environment, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

        if (includeBrdf) renderBrdfLut(cmd, tempViews);
        renderIrradiance(cmd, tempViews, (u32)ibl.environment.sampledPoolIndex);
        renderPrefilter(cmd, tempViews, (u32)ibl.environment.sampledPoolIndex);

        vulkanTransition(cmd, &ibl.irradiance, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        vulkanTransition(cmd, &ibl.prefilter, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        if (includeBrdf) {
            vulkanTransition(cmd, &ibl.brdfLut, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
        }

        vulkanTransientEnd(cmd, 1);

        for (VkImageView view : tempViews) {
            vkDestroyImageView(vulkan.device, view, nullptr);
        }
    }

    static void destroyPipelines(void) {
        if (!pipesReady) return;
        vulkanDestroyPipe(&brdfPipe);
        vulkanDestroyPipe(&prefilterPipe);
        vulkanDestroyPipe(&irradiancePipe);
        pipesReady = false;
    }

    /* Destroys an IBL image and re-points its bindless pool slot to the dummy
     * image first, so in-flight and future frames never see a destroyed view
     * in a bound descriptor.  inPool is cleared manually: the retire helpers
     * already re-added the slot to the free list, and vulkanDestroyImage would
     * otherwise free the same slot a second time. */
    static void destroyIblImage(VulkanImage* image) {
        if (!image->img) return;
        if (image->inPool) {
            if (image->viewType == VK_IMAGE_VIEW_TYPE_CUBE) {
                vulkanRetireCubePoolEntry(image->sampledPoolIndex, &vulkanResources.dummyImage);
            } else {
                vulkanRetireSampledPoolEntry(image->sampledPoolIndex, &vulkanResources.dummyImage);
            }
            image->inPool = false;
        }
        vulkanDestroyImage(image, nullptr);
    }

    static VkImageView createAttachmentView(VulkanImage* image,
                                            u32 baseMipLevel,
                                            u32 baseArrayLayer) {
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
        VkImageView view                         = VK_NULL_HANDLE;
        vkCreateImageView(vulkan.device, &viewInfo, nullptr, &view);
        return view;
    }

    static VulkanImage makeAttachmentProxy(VulkanImage* image, VkImageView view, u32 mipLevel) {
        VulkanImage proxy = *image;
        proxy.view        = view;
        proxy.views.clear();
        proxy.layers        = 1;
        proxy.mipLevels     = 1;
        proxy.extent.width  = std::max(1u, image->extent.width >> mipLevel);
        proxy.extent.height = std::max(1u, image->extent.height >> mipLevel);
        proxy.extent.depth  = 1;
        return proxy;
    }

    static void renderBrdfLut(VulkanCommand* cmd, std::vector<VkImageView>& tempViews) {
        VkImageView view = createAttachmentView(&ibl.brdfLut, 0, 0);
        tempViews.push_back(view);
        VulkanImage target = makeAttachmentProxy(&ibl.brdfLut, view, 0);
        vulkanBeginRender(.cmd = cmd, .pipe = &brdfPipe, .color1 = &target);
        vulkanViewport(cmd, 0, target.extent.height, target.extent.width, -((i32)target.extent.height));
        vulkanScissor(cmd, 0, 0, target.extent.width, target.extent.height);
        vulkanBindPipe(cmd, &brdfPipe);
        vulkanDraw(cmd, 3, 1);
        vulkanEndRender(cmd);
    }

    static void renderIrradiance(VulkanCommand* cmd,
                                 std::vector<VkImageView>& tempViews,
                                 u32 environmentMapIndex) {
        for (u32 face = 0; face < 6; face++) {
            VkImageView view = createAttachmentView(&ibl.irradiance, 0, face);
            tempViews.push_back(view);
            VulkanImage target = makeAttachmentProxy(&ibl.irradiance, view, 0);
            vulkanBeginRender(.cmd = cmd, .pipe = &irradiancePipe, .color1 = &target);
            vulkanViewport(cmd, 0, target.extent.height, target.extent.width, -((i32)target.extent.height));
            vulkanScissor(cmd, 0, 0, target.extent.width, target.extent.height);
            vulkanBindPipe(cmd, &irradiancePipe);
            IblFacePushConstants pc = {.environmentMapIndex = environmentMapIndex,
                                       .faceIndex           = face,
                                       .roughness           = 0.0f,
                                       .pad                 = 0.0f};
            vulkanPush(cmd, &irradiancePipe, sizeof(pc), &pc);
            vulkanDraw(cmd, 3, 1);
            vulkanEndRender(cmd);
        }
    }

    static void renderPrefilter(VulkanCommand* cmd,
                                std::vector<VkImageView>& tempViews,
                                u32 environmentMapIndex) {
        for (u32 mip = 0; mip < PREFILTER_MIPS; mip++) {
            float roughness = PREFILTER_MIPS > 1 ? (float)mip / (float)(PREFILTER_MIPS - 1) : 0.0f;
            for (u32 face = 0; face < 6; face++) {
                VkImageView view = createAttachmentView(&ibl.prefilter, mip, face);
                tempViews.push_back(view);
                VulkanImage target = makeAttachmentProxy(&ibl.prefilter, view, mip);
                vulkanBeginRender(.cmd = cmd, .pipe = &prefilterPipe, .color1 = &target);
                vulkanViewport(cmd, 0, target.extent.height, target.extent.width, -((i32)target.extent.height));
                vulkanScissor(cmd, 0, 0, target.extent.width, target.extent.height);
                vulkanBindPipe(cmd, &prefilterPipe);
                IblFacePushConstants pc = {.environmentMapIndex = environmentMapIndex,
                                           .faceIndex           = face,
                                           .roughness           = roughness,
                                           .pad                 = 0.0f};
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

        double shL0[3] = {0}, shL1n[3] = {0}, shL10[3] = {0}, shL1p[3] = {0};
        double sunRadiance[3] = {0}, sunDirWeighted[3] = {0}, sunWeightTotal = 0.0;

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

        vec4 shL0Vec  = {(float)shL0[0], (float)shL0[1], (float)shL0[2], 0};
        vec4 shL1nVec = {(float)shL1n[0], (float)shL1n[1], (float)shL1n[2], 0};
        vec4 shL10Vec = {(float)shL10[0], (float)shL10[1], (float)shL10[2], 0};
        vec4 shL1pVec = {(float)shL1p[0], (float)shL1p[1], (float)shL1p[2], 0};
        glm_vec4_copy(shL0Vec, ibl.shL0_M0);
        glm_vec4_copy(shL1nVec, ibl.shL1_Mn1);
        glm_vec4_copy(shL10Vec, ibl.shL1_M0);
        glm_vec4_copy(shL1pVec, ibl.shL1_Mp1);
        ibl.hasSH = true;

        vec3 defaultSunDir = {0.3f, 0.8f, -0.5f};
        glm_vec3_copy(defaultSunDir, ibl.extractedSun.direction);
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
            const float minDot = cosf(glm_rad(15.0f));

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

            float dirLen =
                (float)sqrt(clusterDir[0] * clusterDir[0] + clusterDir[1] * clusterDir[1] +
                            clusterDir[2] * clusterDir[2]);
            if (clusterWeight > 0.001 && dirLen > 0.001f) {
                ibl.extractedSun.direction[0]  = (float)(clusterDir[0] / dirLen);
                ibl.extractedSun.direction[1]  = (float)(clusterDir[1] / dirLen);
                ibl.extractedSun.direction[2]  = (float)(clusterDir[2] / dirLen);
                ibl.extractedSun.color[0]      = (float)clusterRad[0];
                ibl.extractedSun.color[1]      = (float)clusterRad[1];
                ibl.extractedSun.color[2]      = (float)clusterRad[2];
                float normalizedLen            = (float)(dirLen / clusterWeight);
                float sunAngleCos              = 2.0f * normalizedLen - 1.0f;
                sunAngleCos                    = fmaxf(sunAngleCos, 0.001f);
                ibl.extractedSun.angularRadius = acosf(fminf(sunAngleCos, 1.0f));
                utils::info(
                    "vulkanIbl: extracted sun (dominant hotspot, seed u=%.3f v=%.3f) — "
                    "dir=(%.4f,%.4f,%.4f), color=(%.2f,%.2f,%.2f), radiance=(%.0f,%.0f,%.0f), "
                    "angularRadius=%.4f",
                    ((double)bestX + 0.5) / width,
                    ((double)bestY + 0.5) / height,
                    (double)ibl.extractedSun.direction[0],
                    (double)ibl.extractedSun.direction[1],
                    (double)ibl.extractedSun.direction[2],
                    (double)ibl.extractedSun.color[0],
                    (double)ibl.extractedSun.color[1],
                    (double)ibl.extractedSun.color[2],
                    clusterRad[0],
                    clusterRad[1],
                    clusterRad[2],
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
                utils::info(
                    "vulkanIbl: extracted sun — dir=(%.4f,%.4f,%.4f), color=(%.2f,%.2f,%.2f), "
                    "radiance=(%.0f,%.0f,%.0f), angularRadius=%.4f",
                    (double)ibl.extractedSun.direction[0],
                    (double)ibl.extractedSun.direction[1],
                    (double)ibl.extractedSun.direction[2],
                    (double)ibl.extractedSun.color[0],
                    (double)ibl.extractedSun.color[1],
                    (double)ibl.extractedSun.color[2],
                    sunRadiance[0],
                    sunRadiance[1],
                    sunRadiance[2],
                    (double)ibl.extractedSun.angularRadius);
            }
        }
    }

}  // namespace engine
