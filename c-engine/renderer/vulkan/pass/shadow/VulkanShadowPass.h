#pragma once

#include "ecs/system/System.h"
#include "ecs/system/light/LightComponent.h"

namespace engine {
struct VulkanImage;

/* Shadow quality levels.  OFF keeps the raster CSM (and the FFX hybrid mask)
 * disabled; the other levels select map size, cascade count and the shadow
 * cast range from the quality table in VulkanShadowPass.cpp. */
enum ShadowQuality {
    SHADOW_QUALITY_OFF = 0,
    SHADOW_QUALITY_LOW,
    SHADOW_QUALITY_MEDIUM,
    SHADOW_QUALITY_HIGH,
    SHADOW_QUALITY_COUNT
};

/* Per-cascade light-view / light-projection matrices, exposed for the FFX
 * shadow classifier.  cascadeLightView[i] is the (shared) light-view rotation
 * (translation = 0); cascadeProj[i] is cascade i's light orthographic
 * projection.  The classifier maps  shadowUV = cascadeProj[i] * (lightView * world)
 * by using lightView as its single LightView and the diagonal / translation of
 * cascadeProj[i] as cascadeScale / cascadeOffset.  cascadeCount / cascadeSize
 * report the active quality level (0 when shadows are off). */
struct ShadowCascadeData {
    mat4 cascadeProj[SHADOW_CASCADE_COUNT];
    mat4 cascadeLightView[SHADOW_CASCADE_COUNT];
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

void vulkanShadowPassSetQuality(ShadowQuality quality);
ShadowQuality vulkanShadowPassGetQuality(void);

void vulkanShadowPassSetPCSS(char enabled);
char vulkanShadowPassIsPCSS(void);

void vulkanShadowPassSetFocusDistance(float distance);
void vulkanShadowPassGetCascadeData(ShadowCascadeData* out);
struct VulkanImage* vulkanShadowPassGetShadowMapLayer(int index);
}  // namespace engine
