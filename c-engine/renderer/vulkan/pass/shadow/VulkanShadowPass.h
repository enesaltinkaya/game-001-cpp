#pragma once

#include "ecs/system/System.h"

namespace engine {
struct VulkanImage;

/* Per-cascade light-view / light-projection matrices, exposed for the FFX
 * shadow classifier.  cascadeLightView[i] is the (shared) light-view rotation
 * (translation = 0); cascadeProj[i] is cascade i's light orthographic
 * projection.  The classifier maps  shadowUV = cascadeProj[i] * (lightView * world)
 * by using lightView as its single LightView and the diagonal / translation of
 * cascadeProj[i] as cascadeScale / cascadeOffset. */
struct ShadowCascadeData {
    mat4 cascadeProj[2];
    mat4 cascadeLightView[2];
    int  cascadeCount;
    int  cascadeSize;
    vec3 lightDir; /* toward the scene */
};

class VulkanShadowPass : public System {
public:
    VulkanShadowPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
};

extern VulkanShadowPass vulkanShadowPass;

void vulkanShadowPassSetDisabled(char disabled);
char vulkanShadowPassIsDisabled(void);

void vulkanShadowPassSetPCSS(char enabled);
char vulkanShadowPassIsPCSS(void);

void vulkanShadowPassSetFocusDistance(float distance);
void vulkanShadowPassGetCascadeData(ShadowCascadeData* out);
struct VulkanImage* vulkanShadowPassGetShadowMapLayer(int index);
}  // namespace engine
