#pragma once
#include "renderer/Renderer.h"

/* Image-based lighting.
 *
 * Loads an equirectangular HDR/EXR environment map, extracts an L1
 * spherical-harmonic irradiance approximation plus a dominant-hotspot sun
 * (direction + HDR radiance + angular radius), and precomputes the
 * irradiance cubemap, the roughness-mipped prefiltered radiance cubemap and
 * the split-sum BRDF LUT.  The state (bindless pool indices, SH
 * coefficients, env rotation, intensities) is pushed into the scene buffer
 * (VulkanIblData / IblData in globalset.shader) and consumed by the scene
 * and heightmap_terrain fragment shaders.
 *
 * The extracted sun is also the scene's directional light: LightSystem
 * applies it (via rendererGetSun) on "rendererInitialized" and every
 * "iblChanged" emission.  Rotating the sun (debug GUI) re-rotates the
 * environment sample directions so shadows, reflections and the extracted
 * light stay in agreement.
 */

namespace engine {
void vulkanIblInit(void);
void vulkanIblDestroy(void);
char vulkanIblIsReady(void);
RendererSunLight vulkanIblGetExtractedSun(void);
void vulkanIblSetDisabled(char disabled);
char vulkanIblIsDisabled(void);

/* Cycle through available IBL environment maps in images/hdr/. */
void vulkanIblCycleNext(void);
void vulkanIblCyclePrev(void);
const char* vulkanIblGetCurrentName(void);

/* Rotate the IBL-extracted sun direction (degrees). */
void vulkanIblRotateSun(float azimuthDeg, float elevationDeg);

/* IBL intensity (diffuse + specular together). */
void vulkanIblSetIntensity(float intensity);
float vulkanIblGetIntensity(void);
}  // namespace engine
