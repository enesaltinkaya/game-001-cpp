#pragma once

#include "renderer/vulkan/resources/VulkanImage.h"

namespace engine {
struct VulkanCommand;

void vulkanFrameResourcesInit(void);
void vulkanFrameResourcesUpdate(void);
void vulkanFrameResourcesDestroy(void);
VulkanImage* vulkanFrameResourcesGetSceneColor(void);
VulkanImage* vulkanFrameResourcesGetCompositeColor(void);
VulkanImage* vulkanFrameResourcesGetNormals(void);
VulkanImage* vulkanFrameResourcesGetMaterial(void);
VulkanImage* vulkanFrameResourcesGetResolvedColor(void);
VulkanImage* vulkanFrameResourcesGetReflectionColor(void);
VulkanImage* vulkanFrameResourcesGetReflectionDepth(void);
VulkanImage* vulkanFrameResourcesGetOitAccum(void);
VulkanImage* vulkanFrameResourcesGetOitReveal(void);
VulkanImage* vulkanFrameResourcesGetRoadLayer(void);
VulkanImage* vulkanFrameResourcesGetDepth(void);
VulkanImage* vulkanFrameResourcesGetVelocity(void);
VulkanImage* vulkanFrameResourcesGetViewNormal(void);
VulkanImage* vulkanFrameResourcesGetWorldNormal(void);
}  // namespace engine
