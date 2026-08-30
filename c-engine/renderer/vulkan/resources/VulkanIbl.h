#pragma once

#include "renderer/Renderer.h"
#include "renderer/vulkan/resources/VulkanImage.h"

namespace engine {

/* IBL module (port of the C renderer's VulkanIbl): loads HDR/EXR
 * equirect environment maps from images/studiolights/ in the engine pak,
 * precomputes the irradiance / prefiltered-env / BRDF-LUT cubemaps,
 * extracts the spherical-harmonic diffuse coefficients and a dominant
 * sun, and pushes everything into the SceneBuffer (VulkanIblData) that
 * the scene / terrain / OIT shaders sample. */

void vulkanIblInit(void);
void vulkanIblDestroy(void);
VulkanImage* vulkanIblGetEnvironmentImage(void);
RendererSunLight vulkanIblGetExtractedSun(void);
void vulkanIblSetDisabled(bool disabled);
bool vulkanIblIsDisabled(void);

// Cycle through the available IBL environment maps in images/studiolights/
void vulkanIblCycleNext(void);
void vulkanIblCyclePrev(void);
const char* vulkanIblGetCurrentName(void);

// Rotate the IBL-extracted sun direction (and the IBL sampling direction
// through the envRotation matrix).
void vulkanIblRotateSun(float azimuthDeg, float elevationDeg);

// IBL intensity (diffuse + specular together).
void vulkanIblSetIntensity(float intensity);
float vulkanIblGetIntensity(void);

}  // namespace engine
