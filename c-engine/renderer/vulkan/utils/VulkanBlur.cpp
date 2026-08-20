#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"

namespace engine {
static VulkanPipe pipelineBlur, pipelineBlurBilateral;
static void initPipe(void);

void vulkanBlur(struct VulkanCommand* cmd, struct VulkanImage* img, struct VulkanImage* tempImg) {
    assert(img->sampledPoolIndex && img->storagePoolIndex);

    if (!pipelineBlur.pipe || !pipelineBlurBilateral.pipe) {
        initPipe();
    }

    struct {
        vec2 direction;
        vec2 imageSize;
        int imageInput;
        int imageOutput;
    } blurPushConstant;

    int width  = img->extent.width;
    int height = img->extent.height;

    int dispatch_x                = (width + 7) / 8;
    int dispatch_y                = (height + 7) / 8;
    blurPushConstant.imageSize[0] = width;
    blurPushConstant.imageSize[1] = height;

    vulkanTransition(cmd, tempImg, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    vulkanTransition(cmd, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    vulkanBindPipe(cmd, &pipelineBlur);
    {
        blurPushConstant.direction[0] = 1.0f;
        blurPushConstant.direction[1] = 0.0f;
        blurPushConstant.imageInput   = img->sampledPoolIndex;
        blurPushConstant.imageOutput  = tempImg->storagePoolIndex;
        vulkanPush(cmd, &pipelineBlur, sizeof(blurPushConstant), &blurPushConstant);
        vulkanDispatch(cmd, &pipelineBlur, dispatch_x, dispatch_y, 1);
    }

    vulkanTransition(cmd, tempImg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, img, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
    {
        blurPushConstant.direction[0] = 0.0f;
        blurPushConstant.direction[1] = 1.0f;
        blurPushConstant.imageInput   = tempImg->sampledPoolIndex;
        blurPushConstant.imageOutput  = img->storagePoolIndex;
        vulkanPush(cmd, &pipelineBlur, sizeof(blurPushConstant), &blurPushConstant);
        vulkanDispatch(cmd, &pipelineBlur, dispatch_x, dispatch_y, 1);
    }
}

void vulkanBlurBilateral(struct VulkanCommand* cmd,
                         struct VulkanImage* original,
                         struct VulkanImage* blurred) {
    assert(original->sampledPoolIndex && original->storagePoolIndex);

    if (!pipelineBlur.pipe || !pipelineBlurBilateral.pipe) {
        initPipe();
    }

    struct {
        vec2 imageSize;
        int imageInput;
        int imageOutput;

        // Controls edge preservation.
        // Small value (0.01 - 0.1) = Keep sharp details.
        // Large value (1.0+) = Act like standard blur.
        float rangeSigma;
    } blurPushConstant;

    int width  = original->extent.width;
    int height = original->extent.height;

    int dispatch_x                = (width + 7) / 8;
    int dispatch_y                = (height + 7) / 8;
    blurPushConstant.imageSize[0] = width;
    blurPushConstant.imageSize[1] = height;
    blurPushConstant.imageInput   = original->sampledPoolIndex;
    blurPushConstant.imageOutput  = blurred->storagePoolIndex;
    blurPushConstant.rangeSigma   = 0.02;

    vulkanTransition(cmd, original, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, blurred, VK_IMAGE_LAYOUT_GENERAL, 0, 1);

    vulkanBindPipe(cmd, &pipelineBlurBilateral);
    {
        vulkanPush(cmd, &pipelineBlurBilateral, sizeof(blurPushConstant), &blurPushConstant);
        vulkanDispatch(cmd, &pipelineBlurBilateral, dispatch_x, dispatch_y, 1);
    }

    // vulkanTransition(cmd, tempImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1);
    // vulkanTransition(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);
    // vulkanCopy(.cmd = cmd, .source.img = tempImg, .target.img = img);
}

void initPipe(void) {
    pipelineBlur = vulkanCreatePipe(.name = "blurPipe",  //
                                    .comp = "shaders/utils/blur/spv/blur.comp.spv");
    pipelineBlurBilateral =
        vulkanCreatePipe(.name = "blurPipe",  //
                         .comp = "shaders/utils/blur/spv/blurBilateral.comp.spv");
}

void vulkanBlurCleanup(void) {
    vulkanDestroyPipe(&pipelineBlur);
    vulkanDestroyPipe(&pipelineBlurBilateral);
}
}  // namespace engine
