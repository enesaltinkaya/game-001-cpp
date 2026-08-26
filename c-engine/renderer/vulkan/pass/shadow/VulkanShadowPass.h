#pragma once

#include "ecs/system/System.h"
#include "ecs/system/light/LightComponent.h"

namespace engine {
struct VulkanImage;

/* Shadow quality levels.  OFF keeps the raster CSM disabled; the other
 * levels select map size, cascade count and the shadow cast range from the
 * quality table in VulkanShadowPass.cpp. */
enum ShadowQuality {
    SHADOW_QUALITY_OFF = 0,
    SHADOW_QUALITY_LOW,
    SHADOW_QUALITY_MEDIUM,
    SHADOW_QUALITY_HIGH,
    SHADOW_QUALITY_COUNT
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
}  // namespace engine
