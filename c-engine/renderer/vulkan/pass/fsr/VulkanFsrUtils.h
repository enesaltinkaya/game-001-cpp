#pragma once

#include "renderer/Renderer.h"

char vulkanFsrGetRenderResolution(RendererUpscalerMode mode,
                                  u32 displayWidth,
                                  u32 displayHeight,
                                  u32* renderWidth,
                                  u32* renderHeight);
int32_t vulkanFsrGetJitterPhaseCount(u32 renderWidth, u32 displayWidth);
void vulkanFsrGetJitterOffset(float* jitterX,
                              float* jitterY,
                              int32_t index,
                              int32_t phaseCount);
