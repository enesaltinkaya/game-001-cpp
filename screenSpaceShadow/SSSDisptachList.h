#ifndef BEND_SSS_CPU_H
#define BEND_SSS_CPU_H

#include <math.h>
#include <stdbool.h>
#include <string.h>  // For memset

// Common screen space shadow projection code (CPU):
//--------------------------------------------------------------

// Generating a screen-space-shadow requires a number of Compute Shader dispatches.
// The compute shader reads from a depth buffer, and writes a single-channel texture of the same dimensions.
// Each dispatch is of the same compute shader.

typedef struct Bend_DispatchData {
    int WaveCount[3];          // Compute Shader Dispatch(X,Y,Z) wave counts X/Y/Z
    int WaveOffset_Shader[2];  // This value is passed in to shader. It will be different for each dispatch
} Bend_DispatchData;

typedef struct Bend_DispatchList {
    float LightCoordinate_Shader[4];  // This value is passed in to shader, same for all dispatches
    struct Bend_DispatchData Dispatch[8];    // List of dispatches (max count is 8)
    int DispatchCount;                // Number of compute dispatches written to the list
} Bend_DispatchList;

// Helper functions
static inline int bend_min(const int a, const int b) {
    return a > b ? b : a;
}

static inline int bend_max(const int a, const int b) {
    return a > b ? a : b;
}

// Call this function on the CPU to get a list of Compute Shader dispatches required to generate a screen-space shadow for a given light.
//
// inLightProjection:       Homogeneous coordinate of the light, result of {light} * {ViewProjectionMatrix}, (without W divide)
//                          For infinite directional lights, use {light} = float4(normalized light direction, 0) and for point/spot lights use {light} =
//                          float4(light world position, 1)
// inViewportSize:          width/height of the render target
// inRenderBounds:          2D Screen Bounds of the light within the viewport, inclusive. [0,0], [width,height] for full-screen.
// inExpandedZRange:        Set to true if the rendering API expects z/w coordinate output from a vertex shader to be a [-1,+1] expanded range.
// inWaveSize:              Wavefront size of the compiled compute shader (currently only tested with 64)

static inline struct Bend_DispatchList Bend_BuildDispatchList(float inLightProjection[4],
                                                       int inViewportSize[2],
                                                       int inMinRenderBounds[2],
                                                       int inMaxRenderBounds[2],
                                                       bool inExpandedZRange,
                                                       int inWaveSize) {
    struct Bend_DispatchList result;
    memset(&result, 0, sizeof(result));

    // Floating point division in the shader has a practical limit for precision when the light is *very* far off screen
    float xy_light_w = inLightProjection[3];
    float FP_limit   = 0.000002f * (float)inWaveSize;

    if (xy_light_w >= 0 && xy_light_w < FP_limit)
        xy_light_w = FP_limit;
    else if (xy_light_w < 0 && xy_light_w > -FP_limit)
        xy_light_w = -FP_limit;

    // Need precise XY pixel coordinates of the light
    // NOTE: Keeping original coordinate system logic as requested (DX12 style)
    result.LightCoordinate_Shader[0] = ((inLightProjection[0] / xy_light_w) * +0.5f + 0.5f) * (float)inViewportSize[0];
    result.LightCoordinate_Shader[1] = ((inLightProjection[1] / xy_light_w) * -0.5f + 0.5f) * (float)inViewportSize[1];
    result.LightCoordinate_Shader[2] = inLightProjection[3] == 0 ? 0 : (inLightProjection[2] / inLightProjection[3]);
    result.LightCoordinate_Shader[3] = inLightProjection[3] > 0 ? 1.0f : -1.0f;

    if (inExpandedZRange) {
        result.LightCoordinate_Shader[2] = result.LightCoordinate_Shader[2] * 0.5f + 0.5f;
    }

    int light_xy[2] = {(int)(result.LightCoordinate_Shader[0] + 0.5f), (int)(result.LightCoordinate_Shader[1] + 0.5f)};

    // Make the bounds inclusive, relative to the light
    const int biased_bounds[4] = {
        inMinRenderBounds[0] - light_xy[0],
        -(inMaxRenderBounds[1] - light_xy[1]),
        inMaxRenderBounds[0] - light_xy[0],
        -(inMinRenderBounds[1] - light_xy[1]),
    };

    // Process 4 quadrants around the light center
    for (int q = 0; q < 4; q++) {
        // Quads 0 and 3 needs to be +1 vertically, 1 and 2 need to be +1 horizontally
        bool vertical = q == 0 || q == 3;

        // Bounds relative to the quadrant
        const int bounds[4] = {
            bend_max(0, ((q & 1) ? biased_bounds[0] : -biased_bounds[2])) / inWaveSize,
            bend_max(0, ((q & 2) ? biased_bounds[1] : -biased_bounds[3])) / inWaveSize,
            bend_max(0, (((q & 1) ? biased_bounds[2] : -biased_bounds[0]) + inWaveSize * (vertical ? 1 : 2) - 1)) / inWaveSize,
            bend_max(0, (((q & 2) ? biased_bounds[3] : -biased_bounds[1]) + inWaveSize * (vertical ? 2 : 1) - 1)) / inWaveSize,
        };

        if ((bounds[2] - bounds[0]) > 0 && (bounds[3] - bounds[1]) > 0) {
            int bias_x = (q == 2 || q == 3) ? 1 : 0;
            int bias_y = (q == 1 || q == 3) ? 1 : 0;

            struct Bend_DispatchData* disp = &result.Dispatch[result.DispatchCount++];

            disp->WaveCount[0]         = inWaveSize;
            disp->WaveCount[1]         = bounds[2] - bounds[0];
            disp->WaveCount[2]         = bounds[3] - bounds[1];
            disp->WaveOffset_Shader[0] = ((q & 1) ? bounds[0] : -bounds[2]) + bias_x;
            disp->WaveOffset_Shader[1] = ((q & 2) ? -bounds[3] : bounds[1]) + bias_y;

            // We want the far corner of this quadrant relative to the light
            int axis_delta = +biased_bounds[0] - biased_bounds[1];
            if (q == 1)
                axis_delta = +biased_bounds[2] + biased_bounds[1];
            if (q == 2)
                axis_delta = -biased_bounds[0] - biased_bounds[3];
            if (q == 3)
                axis_delta = -biased_bounds[2] + biased_bounds[3];

            axis_delta = (axis_delta + inWaveSize - 1) / inWaveSize;

            if (axis_delta > 0) {
                struct Bend_DispatchData* disp2 = &result.Dispatch[result.DispatchCount++];

                // Take copy of current volume
                *disp2 = *disp;

                if (q == 0) {
                    // Split on Y, split becomes -1 larger on x
                    disp2->WaveCount[2] = bend_min(disp->WaveCount[2], axis_delta);
                    disp->WaveCount[2] -= disp2->WaveCount[2];
                    disp2->WaveOffset_Shader[1] = disp->WaveOffset_Shader[1] + disp->WaveCount[2];
                    disp2->WaveOffset_Shader[0]--;
                    disp2->WaveCount[1]++;
                }
                if (q == 1) {
                    // Split on X, split becomes +1 larger on y
                    disp2->WaveCount[1] = bend_min(disp->WaveCount[1], axis_delta);
                    disp->WaveCount[1] -= disp2->WaveCount[1];
                    disp2->WaveOffset_Shader[0] = disp->WaveOffset_Shader[0] + disp->WaveCount[1];
                    disp2->WaveCount[2]++;
                }
                if (q == 2) {
                    // Split on X, split becomes -1 larger on y
                    disp2->WaveCount[1] = bend_min(disp->WaveCount[1], axis_delta);
                    disp->WaveCount[1] -= disp2->WaveCount[1];
                    disp->WaveOffset_Shader[0] += disp2->WaveCount[1];
                    disp2->WaveCount[2]++;
                    disp2->WaveOffset_Shader[1]--;
                }
                if (q == 3) {
                    // Split on Y, split becomes +1 larger on x
                    disp2->WaveCount[2] = bend_min(disp->WaveCount[2], axis_delta);
                    disp->WaveCount[2] -= disp2->WaveCount[2];
                    disp->WaveOffset_Shader[1] += disp2->WaveCount[2];
                    disp2->WaveCount[1]++;
                }

                // Remove if too small
                if (disp2->WaveCount[1] <= 0 || disp2->WaveCount[2] <= 0) {
                    result.DispatchCount--;
                }
                if (disp->WaveCount[1] <= 0 || disp->WaveCount[2] <= 0) {
                    // If the first dispatch became invalid, we need to overwrite it with the next valid one (if any)
                    // or just decrement count. Since disp2 is at count-1 and disp is at count-2.
                    // Simple logic: if disp is invalid, we effectively remove it.
                    // However, disp2 was added *after*.
                    // If disp became invalid, we need to move disp2 into disp's slot if disp2 is valid.
                    // But here we just decrement. The logic in C++ was:
                    // disp = result.Dispatch[--result.DispatchCount];
                    // This implies copying the last element to the current slot.

                    // Re-implementing the C++ logic strictly:
                    struct Bend_DispatchData temp = result.Dispatch[--result.DispatchCount];
                    // If the one we just popped was actually disp2 (which we might have just popped in the previous if block),
                    // then we are popping disp2. If disp2 was valid, it's still in the array?
                    // Actually, the C++ code does: disp2 = result.Dispatch[--result.DispatchCount];
                    // This effectively discards the newly added disp2.
                    // Then: disp = result.Dispatch[--result.DispatchCount];
                    // This effectively discards disp (and replaces it with whatever was at the end, which is nothing since we are at the end).

                    // Let's stick to the C++ logic exactly:
                    // Since we are adding sequentially, 'disp' is at index N, 'disp2' is at index N+1.
                    // If disp2 is invalid, we decrement count (removing disp2).
                    // If disp is invalid, we decrement count.
                    // Note: In C++, 'disp' is a reference. Assigning to it modifies the array content.
                    // In C, 'disp' is a pointer. Assigning to *disp modifies the array content.

                    // The C++ code: disp = result.Dispatch[--result.DispatchCount];
                    // This copies the element at the new end to the position of disp.
                    *disp = result.Dispatch[result.DispatchCount];
                }
            }
        }
    }

    // Scale the shader values by the wave count
    for (int i = 0; i < result.DispatchCount; i++) {
        result.Dispatch[i].WaveOffset_Shader[0] *= inWaveSize;
        result.Dispatch[i].WaveOffset_Shader[1] *= inWaveSize;
    }

    return result;
}


#endif  // BEND_SSS_CPU_H
