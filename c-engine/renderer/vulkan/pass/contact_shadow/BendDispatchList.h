#ifndef BEND_DISPATCH_LIST_H
#define BEND_DISPATCH_LIST_H

/*
 * Bend Studio Screen-Space Shadow dispatch list builder (CPU side).
 *
 * Generates a set of compute dispatches where each wavefront of 64
 * threads samples along a 1D ray pointing toward the projected light.
 * Neighbouring pixels share samples through LDS.
 *
 * Reference: Graham Aldridge, "Screen Space Shadows", Bend Studio / SIGGRAPH.
 */

#include <math.h>
#include <stdbool.h>
#include <string.h>

typedef struct BendDispatchData {
    int WaveCount[3];
    int WaveOffset_Shader[2];
} BendDispatchData;

typedef struct BendDispatchList {
    float LightCoordinate_Shader[4];
    BendDispatchData Dispatch[8];
    int DispatchCount;
} BendDispatchList;

static inline int bend_min(int a, int b) { return a < b ? a : b; }
static inline int bend_max(int a, int b) { return a > b ? a : b; }

static inline BendDispatchList bendBuildDispatchList(const float inLightProjection[4],
                                                     const int   inViewportSize[2],
                                                     const int   inMinRenderBounds[2],
                                                     const int   inMaxRenderBounds[2],
                                                     bool  inExpandedZRange,
                                                     int   inWaveSize) {
    BendDispatchList result;
    memset(&result, 0, sizeof(result));

    float xy_light_w = inLightProjection[3];
    float FP_limit   = 0.000002f * (float)inWaveSize;

    if (xy_light_w >= 0 && xy_light_w < FP_limit)
        xy_light_w = FP_limit;
    else if (xy_light_w < 0 && xy_light_w > -FP_limit)
        xy_light_w = -FP_limit;

    result.LightCoordinate_Shader[0] = ((inLightProjection[0] / xy_light_w) *  0.5f + 0.5f) * (float)inViewportSize[0];
    result.LightCoordinate_Shader[1] = ((inLightProjection[1] / xy_light_w) * -0.5f + 0.5f) * (float)inViewportSize[1];
    result.LightCoordinate_Shader[2] = inLightProjection[3] == 0 ? 0 : (inLightProjection[2] / inLightProjection[3]);
    result.LightCoordinate_Shader[3] = inLightProjection[3] > 0 ? 1.0f : -1.0f;

    if (inExpandedZRange) {
        result.LightCoordinate_Shader[2] = result.LightCoordinate_Shader[2] * 0.5f + 0.5f;
    }

    int light_xy[2] = {
        (int)(result.LightCoordinate_Shader[0] + 0.5f),
        (int)(result.LightCoordinate_Shader[1] + 0.5f)
    };

    const int biased_bounds[4] = {
        inMinRenderBounds[0] - light_xy[0],
        -(inMaxRenderBounds[1] - light_xy[1]),
        inMaxRenderBounds[0] - light_xy[0],
        -(inMinRenderBounds[1] - light_xy[1]),
    };

    for (int q = 0; q < 4; q++) {
        bool vertical = q == 0 || q == 3;

        const int bounds[4] = {
            bend_max(0, ((q & 1) ? biased_bounds[0] : -biased_bounds[2])) / inWaveSize,
            bend_max(0, ((q & 2) ? biased_bounds[1] : -biased_bounds[3])) / inWaveSize,
            bend_max(0, (((q & 1) ? biased_bounds[2] : -biased_bounds[0]) + inWaveSize * (vertical ? 1 : 2) - 1)) / inWaveSize,
            bend_max(0, (((q & 2) ? biased_bounds[3] : -biased_bounds[1]) + inWaveSize * (vertical ? 2 : 1) - 1)) / inWaveSize,
        };

        if ((bounds[2] - bounds[0]) > 0 && (bounds[3] - bounds[1]) > 0) {
            int bias_x = (q == 2 || q == 3) ? 1 : 0;
            int bias_y = (q == 1 || q == 3) ? 1 : 0;

            BendDispatchData* disp = &result.Dispatch[result.DispatchCount++];
            disp->WaveCount[0]         = inWaveSize;
            disp->WaveCount[1]         = bounds[2] - bounds[0];
            disp->WaveCount[2]         = bounds[3] - bounds[1];
            disp->WaveOffset_Shader[0] = ((q & 1) ? bounds[0] : -bounds[2]) + bias_x;
            disp->WaveOffset_Shader[1] = ((q & 2) ? -bounds[3] : bounds[1]) + bias_y;

            int axis_delta = +biased_bounds[0] - biased_bounds[1];
            if (q == 1) axis_delta = +biased_bounds[2] + biased_bounds[1];
            if (q == 2) axis_delta = -biased_bounds[0] - biased_bounds[3];
            if (q == 3) axis_delta = -biased_bounds[2] + biased_bounds[3];

            axis_delta = (axis_delta + inWaveSize - 1) / inWaveSize;

            if (axis_delta > 0) {
                BendDispatchData* disp2 = &result.Dispatch[result.DispatchCount++];
                *disp2 = *disp;

                if (q == 0) {
                    disp2->WaveCount[2] = bend_min(disp->WaveCount[2], axis_delta);
                    disp->WaveCount[2] -= disp2->WaveCount[2];
                    disp2->WaveOffset_Shader[1] = disp->WaveOffset_Shader[1] + disp->WaveCount[2];
                    disp2->WaveOffset_Shader[0]--;
                    disp2->WaveCount[1]++;
                }
                if (q == 1) {
                    disp2->WaveCount[1] = bend_min(disp->WaveCount[1], axis_delta);
                    disp->WaveCount[1] -= disp2->WaveCount[1];
                    disp2->WaveOffset_Shader[0] = disp->WaveOffset_Shader[0] + disp->WaveCount[1];
                    disp2->WaveCount[2]++;
                }
                if (q == 2) {
                    disp2->WaveCount[1] = bend_min(disp->WaveCount[1], axis_delta);
                    disp->WaveCount[1] -= disp2->WaveCount[1];
                    disp->WaveOffset_Shader[0] += disp2->WaveCount[1];
                    disp2->WaveCount[2]++;
                    disp2->WaveOffset_Shader[1]--;
                }
                if (q == 3) {
                    disp2->WaveCount[2] = bend_min(disp->WaveCount[2], axis_delta);
                    disp->WaveCount[2] -= disp2->WaveCount[2];
                    disp->WaveOffset_Shader[1] += disp2->WaveCount[2];
                    disp2->WaveCount[1]++;
                }

                if (disp2->WaveCount[1] <= 0 || disp2->WaveCount[2] <= 0) {
                    result.DispatchCount--;
                }
                if (disp->WaveCount[1] <= 0 || disp->WaveCount[2] <= 0) {
                    *disp = result.Dispatch[--result.DispatchCount];
                }
            }
        }
    }

    for (int i = 0; i < result.DispatchCount; i++) {
        result.Dispatch[i].WaveOffset_Shader[0] *= inWaveSize;
        result.Dispatch[i].WaveOffset_Shader[1] *= inWaveSize;
    }

    return result;
}

#endif /* BEND_DISPATCH_LIST_H */
