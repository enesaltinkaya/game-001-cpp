#pragma once

#include "VulkanImage.h"
#include "renderer/Renderer.h"

typedef struct IblSunLight {
    vec3 direction; // normalized direction toward the sun
    vec3 color;     // HDR radiance
    float angularRadius; // radians
} IblSunLight;

void vulkanIblInit(void);
void vulkanIblDestroy(void);
VulkanImage* vulkanIblGetEnvironmentImage(void);
IblSunLight vulkanIblGetExtractedSun(void);
void vulkanIblSetDisabled(char disabled);
char vulkanIblIsDisabled(void);

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
