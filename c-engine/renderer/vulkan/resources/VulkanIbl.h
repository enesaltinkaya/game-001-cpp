#pragma once

#include "VulkanImage.h"
#include "renderer/Renderer.h"

namespace engine {
struct IblSunLight {
    vec3 direction; // normalized direction toward the sun
    vec3 color;     // HDR radiance
    float angularRadius; // radians
};

void vulkanIblInit(void);
void vulkanIblDestroy(void);
VulkanImage* vulkanIblGetEnvironmentImage(void);
/* Prefiltered specular environment cubemap (R16G16B16A16, mipped) — the
 * image-based-lighting fallback for screen-space reflections (SSSR). */
VulkanImage* vulkanIblGetPrefilterImage(void);
/* Precomputed BRDF integration LUT (R16G16, 512x512). */
VulkanImage* vulkanIblGetBrdfLutImage(void);
IblSunLight vulkanIblGetExtractedSun(void);
void vulkanIblSetDisabled(bool disabled);
bool vulkanIblIsDisabled(void);

// Cycle through available IBL environment maps in studiolights/
void vulkanIblCycleNext(void);
void vulkanIblCyclePrev(void);
const char* vulkanIblGetCurrentName(void);

// Rotate the IBL-extracted sun direction.
void vulkanIblRotateSun(float azimuthDeg, float elevationDeg);

// IBL intensity (diffuse + specular together).
void vulkanIblSetIntensity(float intensity);
float vulkanIblGetIntensity(void);

// Tonemap mode for IBL rendering.
void vulkanIblSetTonemapMode(TonemapMode mode);
}  // namespace engine
