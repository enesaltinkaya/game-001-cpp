#include "VulkanCompositePass.h"
#include "renderer/vulkan/pass/ao/VulkanAOPass.h"
#include "renderer/vulkan/pass/azgaar_weather/VulkanAzgaarWeatherPass.h"
#include "renderer/vulkan/pass/brixelizer/VulkanBrixelizerPass.h"
#include "renderer/vulkan/pass/volumetric/VulkanVolumetricPass.h"
#include "ecs/system/System.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include <stdlib.h>

namespace engine {

static VulkanPipe pipeline;
static double     elapsedCPU;
static double     elapsedGPU;

/* Brixelizer GI strength knobs (read once from the environment): the FFX
 * output is already scaled by its environmentMapIntensity, so 1.0 is the
 * "trust the tracer" default and the env vars exist for tuning/A-B. */
static float giDiffuseFactor;
static float giSpecularFactor;
static char  giFactorInited;
static void  giReadFactors(void) {
    if (giFactorInited) {
        return;
    }
    const char* df = getenv("ENGINE_BRIX_GI_DIFFUSE_FACTOR");
    const char* sf = getenv("ENGINE_BRIX_GI_SPECULAR_FACTOR");
    giDiffuseFactor  = df ? (float)atof(df) : 1.0f;
    giSpecularFactor = sf ? (float)atof(sf) : 1.0f;
    giFactorInited   = 1;
    utils::info("vulkanCompositePass: brixelizer GI factors diffuse=%.3f specular=%.3f",
                giDiffuseFactor, giSpecularFactor);
}

VulkanCompositePass vulkanCompositePass;

VulkanCompositePass::VulkanCompositePass() : System("composite") {}

typedef struct CompositePushConstants {
    u32 sceneColorIndex;
    u32 ssrColorIndex;
    u32 depthIndex;
    u32 normalsIndex;
    u32 materialIndex;
    u32 outputImageIndex;
    u32 volumetricColorIndex;
    u32 weatherMaskIndex;
    u32 aoIndex;
    u32 giDiffuseIndex;
    u32 giSpecularIndex;
    float giDiffuseFactor;
    float giSpecularFactor;
    u32 width;
    u32 height;
} CompositePushConstants;

void VulkanCompositePass::added() {
    pipeline = vulkanCreatePipe(
        .name = "composite",
        .comp = "shaders/pass/composite/spv/composite.comp.spv");
}

void VulkanCompositePass::preUpdate() {
    vulkanResetProfile(vulkan.currentCmd, &pipeline.profile, 0);
}

void VulkanCompositePass::update() {
    elapsedCPU = utils::nanos();
    giReadFactors();

    if (vulkan.skipFrame) {
        elapsedCPU = utils::nanos() - elapsedCPU;
        return;
    }

    VulkanCommand *cmd        = vulkan.currentCmd;
    VulkanImage   *sceneColor = vulkanFrameResourcesGetSceneColor();
    VulkanImage   *depth      = vulkanFrameResourcesGetDepth();
    VulkanImage   *reflColor  = vulkanFrameResourcesGetReflectionColor();
    VulkanImage   *normals    = vulkanFrameResourcesGetNormals();
    VulkanImage   *material   = vulkanFrameResourcesGetMaterial();
    VulkanImage   *composite  = vulkanFrameResourcesGetCompositeColor();
    VulkanImage   *volumetric  = vulkanVolumetricPassGetOutput();
    /* Weather particle coverage mask — the weather pass runs earlier in the
     * frame, so this frame's mask is ready.  Used to keep the screen-space
     * fog from erasing particles that float in front of fogged geometry. */
    VulkanImage   *weatherMask = vulkanAzgaarWeatherPassGetMask();
    /* Brixelizer GI outputs — the brixelizer pass ran earlier in the frame,
     * so this frame's diffuse/specular GI is already written.  Absent while
     * GI is not up (context not created) or at a mismatched resolution;
     * the composite then uses the 0xFFFFFFFF sentinel and stays bit-identical
     * to the no-GI output. */
    VulkanImage   *giDiffuse  = (VulkanImage*)vulkanBrixelizerPassGetDiffuseGI();
    VulkanImage   *giSpecular = (VulkanImage*)vulkanBrixelizerPassGetSpecularGI();
    /* One pair: either both GI outputs are present and sized for this GBuffer
     * or neither is consumed. */
    if ((giDiffuse && !vulkanBrixelizerPassGIResolutionMatches(giDiffuse)) ||
        (giSpecular && !vulkanBrixelizerPassGIResolutionMatches(giSpecular))) {
        giDiffuse  = NULL;
        giSpecular = NULL;
    }
    if (!sceneColor || !depth || !reflColor || !normals || !material || !composite) {
        elapsedCPU = utils::nanos() - elapsedCPU;
        return;
    }

    vulkanTransition(cmd, sceneColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, normals, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, material, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    if (volumetric) {
        vulkanTransition(cmd, volumetric, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    }
    if (weatherMask) {
        vulkanTransition(cmd, weatherMask, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    }
    /* The GI outputs sit in GENERAL after the brixelizer's UAV writes; the
     * FFX backend inserts no barriers on the first re-wrap per frame, so the
     * engine leaves them in exactly the sampled layout.  They go back to
     * GENERAL after the dispatch (see below) for the next GI UAV write. */
    if (giDiffuse) {
        vulkanTransition(cmd, giDiffuse, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    }
    if (giSpecular) {
        vulkanTransition(cmd, giSpecular, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    }
    vulkanTransition(cmd, composite, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

    vulkanBeginProfile(cmd, &pipeline.profile, 0);
    vulkanBindPipe(cmd, &pipeline);

    CompositePushConstants pc = {
        .sceneColorIndex      = (u32)sceneColor->sampledPoolIndex,
        .ssrColorIndex        = (u32)reflColor->sampledPoolIndex,
        .depthIndex           = (u32)depth->sampledPoolIndex,
        .normalsIndex         = (u32)normals->sampledPoolIndex,
        .materialIndex        = (u32)material->sampledPoolIndex,
        .outputImageIndex     = (u32)composite->storagePoolIndex,
        .volumetricColorIndex = volumetric ? (u32)volumetric->sampledPoolIndex : 0u,
        .weatherMaskIndex     = weatherMask ? (u32)weatherMask->sampledPoolIndex : 0xFFFFFFFFu,
        /* CACAO AO output (absent-sentinel pattern, like the weather
         * mask): while AO is disabled the multiply is skipped entirely so
         * the frame stays pixel-identical to pre-AO. */
        .aoIndex              = vulkanAOPassIsDisabled()
                                   ? 0xFFFFFFFFu
                                   : (vulkanAOPassGetOutput()
                                         ? (u32)vulkanAOPassGetOutput()->sampledPoolIndex
                                         : 0xFFFFFFFFu),
        /* Brixelizer GI (absent-sentinel pattern, like AO/weather): the
         * composite adds these only when a matching output pair is ready;
         * with the sentinel the shader skips the fetches entirely, so the
         * frame stays bit-identical to the no-GI composite. */
        .giDiffuseIndex       = giDiffuse ? (u32)giDiffuse->sampledPoolIndex : 0xFFFFFFFFu,
        .giSpecularIndex      = giSpecular ? (u32)giSpecular->sampledPoolIndex : 0xFFFFFFFFu,
        .giDiffuseFactor      = giDiffuseFactor,
        .giSpecularFactor     = giSpecularFactor,
        .width                = composite->extent.width,
        .height               = composite->extent.height,
    };
    vulkanPush(cmd, &pipeline, sizeof(pc), &pc);

    u32 groupsX = (composite->extent.width  + 7) / 8;
    u32 groupsY = (composite->extent.height + 7) / 8;
    vulkanDispatch(cmd, &pipeline, groupsX, groupsY, 1);
    vulkanEndProfile(cmd, &pipeline.profile, 0);

    /* Hand the GI outputs back to GENERAL so the next frame's GI dispatch
     * (UAV writes) sees the layout its wrapper expects. */
    if (giDiffuse) {
        vulkanTransition(cmd, giDiffuse, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    }
    if (giSpecular) {
        vulkanTransition(cmd, giSpecular, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    }
    vulkanTransition(cmd, composite, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    elapsedGPU = pipeline.profile.elapsed;
    elapsedCPU = utils::nanos() - elapsedCPU;
}

void VulkanCompositePass::postUpdate() {
    vulkanCompositePass.cpuElapsed = elapsedCPU;
    vulkanCompositePass.gpuElapsed = elapsedGPU;
}

void VulkanCompositePass::removed() {
    vulkanDestroyPipe(&pipeline);
}
}  // namespace engine
