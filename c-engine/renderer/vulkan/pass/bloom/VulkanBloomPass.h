#pragma once

#include "ecs/system/System.h"

extern System vulkanBloomPass;

/// Returns the sampled pool index of bloom mip 0 (half-res bloom result).
/// Returns 0 if bloom is not available (first frame, invalid size, etc.).
int vulkanBloomPassGetBloomSampledIndex(void);

/// Returns the bloom strength for the final composite.
float vulkanBloomPassGetStrength(void);

void  vulkanBloomPassSetDisabled(char disabled);
char  vulkanBloomPassIsDisabled(void);
