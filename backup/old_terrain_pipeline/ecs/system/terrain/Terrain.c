#include "Terrain.h"

float terrainGetHeight(Terrain* terrain, float wx, float wz) {
    TerrainHeightfield* hf = &terrain->heightfield;
    if (!hf->heights) return 0.0f;

    float fx = (wx - hf->minX) / hf->spacing;
    float fz = (wz - hf->minZ) / hf->spacing;

    i32 x0 = (i32)floorf(fx);
    i32 z0 = (i32)floorf(fz);
    // Clamp to valid bilinear region (edges return boundary height)
    if (x0 < 0) x0 = 0;
    if (z0 < 0) z0 = 0;
    if (x0 >= (i32)hf->gridW - 1) x0 = hf->gridW - 2;
    if (z0 >= (i32)hf->gridH - 1) z0 = hf->gridH - 2;

    float tx = fx - (float)x0;
    float tz = fz - (float)z0;

    float h00 = hf->heights[z0       * hf->gridW + x0];
    float h10 = hf->heights[z0       * hf->gridW + x0 + 1];
    float h01 = hf->heights[(z0 + 1) * hf->gridW + x0];
    float h11 = hf->heights[(z0 + 1) * hf->gridW + x0 + 1];

    return (1-tx)*(1-tz)*h00 + tx*(1-tz)*h10 + (1-tx)*tz*h01 + tx*tz*h11;
}
