#pragma once
#include <vector>

namespace utils {
struct Image {
    void* data;  // can be u8, u16 or float
    u64 size;    // width * height * channels * depth
    int width;
    int height;
    int channels;  // r, rg, rgb, rgba => 1, 2, 3, 4
    int depth;     // in bytes, aka 8bit => 1, 16bit => 2, float => 4
    int vkFormat;
    int mips;
    std::vector<u64> mipSizes;
    bool isKtx;
    bool isRaw;
};

enum KtxFormat {
    KTX_FORMAT_BC7_RGBA = 6,
    KTX_FORMAT_RGBA32   = 13,
};

Image imageLoad(const char* path);
Image imageLoadKtx(const char* path, KtxFormat format);
Image imageLoadFromData(const u8* data, u64 size, const char* mime);
Image imageResize(Image* image, int newWidth, int newHeight);
void imageDestory(Image* image);

void gaussianBlurNormalMap(const signed char* input_normal_map,
                           signed char* output_normal_map,
                           int width,
                           int height,
                           int radius,
                           float sigma);

void gaussianBlur(const float* input, float* output, int width, int height, int radius, float sigma);
void boxBlur(const float* input, float* output, int width, int height, int radius);
void multiPassBoxBlurNormalMap(const signed char* input_normal_map,
                               signed char* output_normal_map,
                               int width,
                               int height,
                               int radius,
                               int passes);
void boxBlurNormalMap(const signed char* input_normal_map, signed char* output_normal_map, int width, int height, int radius);
void boxBlurNormalMapFast(const signed char* input_normal_map, signed char* output_normal_map, int width, int height, int radius);
void boxBlurWithHoles(const float* input, float* output, int width, int height, int radius);
void boxBlurWithHolesOptimized(const float* input, float* output, int width, int height, int radius);
}  // namespace utils
