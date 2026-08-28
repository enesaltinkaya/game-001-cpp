#include "VulkanFrameResources.h"
#include "ecs/system/window/WindowSystem.h"
#include "events/Events.h"
#include "renderer/Renderer.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"

namespace engine {
struct VulkanFrameResources {
    VulkanImage sceneColor;
    VulkanImage compositeColor;
    VulkanImage normals;
    VulkanImage material;
    VulkanImage reflectionColor;
    VulkanImage reflectionDepth;
    VulkanImage oitAccum;
    VulkanImage oitReveal;
    VulkanImage roadLayer;
    VulkanImage resolvedColor;
    VulkanImage depth;
    VulkanImage velocity;
    VulkanImage viewNormal;
    VulkanImage worldNormal;
    u32 width;
    u32 height;
};

static VulkanFrameResources frameResources;

static void recreate(void);
static void swapchainCreated(void* _);
static void destroyImage(VulkanImage* image);
static void destroyAll(void);
static void transitionInitialLayouts(void);

void vulkanFrameResourcesInit(void) {
    utils::signalSubscribe("swapchainCreated", swapchainCreated);
    recreate();
}

void vulkanFrameResourcesUpdate(void) {
    /* Resolution changes are synchronized through the "swapchainCreated" signal.
     * While skipFrame is active we are waiting for a safe recreate point, so
     * do not destroy/recreate render targets here or we may free images that
     * are still in flight on the GPU. */
    if (vulkan.skipFrame) {
        return;
    }

    if (window.renderWidth > 0 && window.renderHeight > 0) {
        recreate();
    }
}

void vulkanFrameResourcesDestroy(void) {
    destroyAll();
}

VulkanImage* vulkanFrameResourcesGetSceneColor(void) {
    return frameResources.sceneColor.img ? &frameResources.sceneColor : nullptr;
}

VulkanImage* vulkanFrameResourcesGetCompositeColor(void) {
    return frameResources.compositeColor.img ? &frameResources.compositeColor : nullptr;
}

VulkanImage* vulkanFrameResourcesGetNormals(void) {
    return frameResources.normals.img ? &frameResources.normals : nullptr;
}

VulkanImage* vulkanFrameResourcesGetMaterial(void) {
    return frameResources.material.img ? &frameResources.material : nullptr;
}

VulkanImage* vulkanFrameResourcesGetResolvedColor(void) {
    return frameResources.resolvedColor.img ? &frameResources.resolvedColor : nullptr;
}

VulkanImage* vulkanFrameResourcesGetReflectionColor(void) {
    return frameResources.reflectionColor.img ? &frameResources.reflectionColor : nullptr;
}

VulkanImage* vulkanFrameResourcesGetReflectionDepth(void) {
    return frameResources.reflectionDepth.img ? &frameResources.reflectionDepth : nullptr;
}

VulkanImage* vulkanFrameResourcesGetOitAccum(void) {
    return frameResources.oitAccum.img ? &frameResources.oitAccum : nullptr;
}

VulkanImage* vulkanFrameResourcesGetOitReveal(void) {
    return frameResources.oitReveal.img ? &frameResources.oitReveal : nullptr;
}

VulkanImage* vulkanFrameResourcesGetRoadLayer(void) {
    return frameResources.roadLayer.img ? &frameResources.roadLayer : nullptr;
}

VulkanImage* vulkanFrameResourcesGetDepth(void) {
    return frameResources.depth.img ? &frameResources.depth : nullptr;
}

VulkanImage* vulkanFrameResourcesGetVelocity(void) {
    return frameResources.velocity.img ? &frameResources.velocity : nullptr;
}

VulkanImage* vulkanFrameResourcesGetViewNormal(void) {
    return frameResources.viewNormal.img ? &frameResources.viewNormal : nullptr;
}

VulkanImage* vulkanFrameResourcesGetWorldNormal(void) {
    return frameResources.worldNormal.img ? &frameResources.worldNormal : nullptr;
}

static void swapchainCreated(void* _) {
    recreate();
}

static void recreate(void) {
    if (window.renderWidth <= 0 || window.renderHeight <= 0) {
        return;
    }

    if (frameResources.width == static_cast<u32>(window.renderWidth) &&
        frameResources.height == static_cast<u32>(window.renderHeight) &&
        frameResources.sceneColor.img &&
        frameResources.sceneColor.samples == VK_SAMPLE_COUNT_1_BIT) {
        return;
    }

    destroyAll();

    frameResources.width   = window.renderWidth;
    frameResources.height  = window.renderHeight;

    frameResources.sceneColor =
        vulkanCreateImage(.name   = "SceneColor",
                          .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                          .usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                          .width  = window.renderWidth,
                          .height = window.renderHeight);

    frameResources.compositeColor =
        vulkanCreateImage(.name   = "CompositeColor",
                          .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                          .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          .width  = window.renderWidth,
                          .height = window.renderHeight);

    frameResources.normals =
        vulkanCreateImage(.name   = "Normals",
                          .format = VK_FORMAT_R16G16_SFLOAT,
                          .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          .width = window.renderWidth,
                          .height = window.renderHeight);

    frameResources.material =
        vulkanCreateImage(.name   = "Material",
                          .format = VK_FORMAT_R8G8B8A8_UNORM,
                          .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          .width = window.renderWidth,
                          .height = window.renderHeight);

    frameResources.resolvedColor =
        vulkanCreateImage(.name   = "ResolvedColor",
                          .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                          .usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          .width  = window.renderWidth,
                          .height = window.renderHeight);

    frameResources.reflectionColor =
        vulkanCreateImage(.name   = "ReflectionColor",
                          .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                          .usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          .width  = window.renderWidth,
                          .height = window.renderHeight);

    frameResources.reflectionDepth =
        vulkanCreateImage(.name   = "ReflectionDepth",
                          .format = VK_FORMAT_D32_SFLOAT,
                          .usage  = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT,
                          .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
                          .width  = window.renderWidth > 1 ? window.renderWidth / 2 : 1,
                          .height = window.renderHeight > 1 ? window.renderHeight / 2 : 1);

    frameResources.oitAccum =
        vulkanCreateImage(.name   = "OitAccum",
                          .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                          .usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          .width  = window.renderWidth,
                          .height = window.renderHeight);

    frameResources.oitReveal =
        vulkanCreateImage(.name   = "OitReveal",
                          .format = VK_FORMAT_R16_SFLOAT,
                          .usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          .width  = window.renderWidth,
                          .height = window.renderHeight);

    // Offscreen layer where road decals accumulate with a union blend (replace
    // RGB / MAX alpha) so overlapping junction rectangles stay idempotent; the
    // decal pass composites it onto the scene once.
    frameResources.roadLayer =
        vulkanCreateImage(.name   = "RoadLayer",
                          .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                          .usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          .width  = window.renderWidth,
                          .height = window.renderHeight);

frameResources.depth =
        vulkanCreateImage(.name   = "Depth",
                          .format = VK_FORMAT_D32_SFLOAT,
                          .usage  = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
                          .width  = window.renderWidth,
                          .height = window.renderHeight);

    /* TRANSFER_SRC/DST: the debug frame-image dump (ENGINE_DEBUG_DUMP_IMAGES)
     * blits the velocity buffer through a TRANSFER_SRC transition; without
     * the usage bits that transition is a validation error. */
    frameResources.velocity =
        vulkanCreateImage(.name   = "Velocity",
                          .format = VK_FORMAT_R16G16_SFLOAT,
                          .usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          .width  = window.renderWidth,
                          .height = window.renderHeight);

    frameResources.viewNormal =
        vulkanCreateImage(.name   = "ViewNormal",
                          .format = VK_FORMAT_R16G16_SNORM,
                          .usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT,
                          .width  = window.renderWidth,
                          .height = window.renderHeight);

    /* Full 3-component world-space normal, written by the depth pre-pass
     * (scene / heightmap / props / water).  Consumed by the FFX shadow
     * classifier (backfacing test) and the shadow denoiser (reprojection
     * z-alignment).  Cleared to 0: a zero normal fails the classifier's
     * facing test, so sky / unrendered pixels stay out of the shadow
     * work (the denoiser's prepare pass excludes depth-0 pixels anyway). */
    frameResources.worldNormal =
        vulkanCreateImage(.name   = "WorldNormal",
                          .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                          .usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          .width  = window.renderWidth,
                          .height = window.renderHeight);

    transitionInitialLayouts();
}

static void destroyImage(VulkanImage* image) {
    if (!image->img) {
        return;
    }

    vulkanDestroyImage(image, nullptr);
    *image = VulkanImage{};
}

static void destroyAll(void) {
    destroyImage(&frameResources.sceneColor);
    destroyImage(&frameResources.compositeColor);
    destroyImage(&frameResources.normals);
    destroyImage(&frameResources.material);
    destroyImage(&frameResources.reflectionColor);
    destroyImage(&frameResources.reflectionDepth);
    destroyImage(&frameResources.oitAccum);
    destroyImage(&frameResources.oitReveal);
    destroyImage(&frameResources.roadLayer);
    destroyImage(&frameResources.resolvedColor);
    destroyImage(&frameResources.depth);
    destroyImage(&frameResources.velocity);
    destroyImage(&frameResources.viewNormal);
    destroyImage(&frameResources.worldNormal);

    frameResources.width  = 0;
    frameResources.height = 0;
}

static void transitionInitialLayouts(void) {
    VulkanCommand* cmd = vulkanTransientBegin();

    vulkanTransition(cmd,
                     &frameResources.sceneColor,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     0,
                     1);
    vulkanTransition(cmd,
                     &frameResources.compositeColor,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     0,
                     1);
    vulkanTransition(cmd, &frameResources.normals, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    vulkanTransition(cmd, &frameResources.material, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    vulkanTransition(cmd,
                     &frameResources.resolvedColor,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     0,
                     1);
    vulkanTransition(cmd,
                     &frameResources.reflectionColor,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     0,
                     1);
    vulkanTransition(cmd,
                     &frameResources.reflectionDepth,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     0,
                     1);
    vulkanTransition(cmd, &frameResources.oitAccum, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    vulkanTransition(cmd,
                     &frameResources.oitReveal,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     0,
                     1);
    vulkanTransition(cmd, &frameResources.roadLayer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    vulkanTransition(cmd, &frameResources.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, 1);
    vulkanTransition(cmd, &frameResources.velocity, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    vulkanTransition(cmd, &frameResources.viewNormal, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    vulkanTransition(cmd, &frameResources.worldNormal, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);

    vulkanTransientEnd(cmd, 1);
}
}  // namespace engine
