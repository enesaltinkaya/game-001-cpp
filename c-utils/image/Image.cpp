#include "Image.h"
#include "Utils.h"
#include "datamanager/DataManager.h"
#include "ktx/git/include/ktx.h"
#include "stb/git/stb_image.h"
#include "stb/git/stb_image_resize2.h" // IWYU pragma: keep
#include "string/String.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>

namespace utils {
Image imageLoadFromData(const u8 *data, u64 size, const char *mime) {
  Image image = {};

  if (strEndsWithC(mime, "ktx2") || strEndsWithC(mime, "ktx")) {
    ktxTexture2 *ktxTexture2 = {};

    if (ktxTexture2_CreateFromMemory(data, size, 0, &ktxTexture2) > 0) {
      terminate("failed to parse ktx");
    }
    image.width = ktxTexture2->baseWidth;
    image.height = ktxTexture2->baseHeight;
    image.isKtx = 1;

    image.mips = ktxTexture2->numLevels;
    uint32_t numChannels = ktxTexture2_GetNumComponents(ktxTexture2);
    ktx_transcode_fmt_e target_format = static_cast<ktx_transcode_fmt_e>(0);

    if (numChannels == 1) {
      target_format = KTX_TTF_BC4_R;
    } else if (numChannels == 2) {
      target_format = KTX_TTF_BC5_RG;
    } else {
      target_format = KTX_TTF_BC7_RGBA;
    }

    if (ktxTexture2_NeedsTranscoding(ktxTexture2)) {
      ktxTexture2_TranscodeBasis(ktxTexture2, target_format, 0);
    }

    for (i32 i = 0, s = image.mips; i < s; i++) {
      size_t size = 0;
      ktxTexture_GetImageOffset((ktxTexture *)ktxTexture2, i, 0, 0, &size);
      image.mipSizes.push_back(size);
    }

    image.vkFormat = ktxTexture2->vkFormat;
    image.size = ktxTexture2->dataSize;
    image.data = new u8[image.size];
    image.channels = numChannels;

    memcpy(image.data, ktxTexture2->pData, image.size);
    ktxTexture_Destroy((ktxTexture *)ktxTexture2);
  } else {
    char is16bit = stbi_is_16_bit_from_memory(data, size);
    int channels = 0;
    image.mips = 1;
    if (is16bit) {
      image.depth = 2;
      image.data = stbi_load_16_from_memory(data, size, &image.width,
                                            &image.height, &channels, 4);
    } else {
      image.depth = 1;
      image.data = stbi_load_from_memory(data, size, &image.width,
                                         &image.height, &channels, 4);
    }
    image.channels = 4; // channels;
    image.size = image.width * image.height * image.channels * image.depth;
  }

  return image;
}

Image imageLoad(const char *path) {
  String fileData = dataManagerRead(path);
  Image image = imageLoadFromData(reinterpret_cast<const u8*>(fileData.data), fileData.size, path);
  stringDestroy(&fileData);
  return image;
}

Image imageLoadKtx(const char *path, KtxFormat format) {
  Image image = {};

  if (strEndsWithC(path, "ktx2") || strEndsWithC(path, "ktx")) {
    ktxTexture2 *ktxTexture2 = {};
    String fileData = dataManagerRead(path);

    if (ktxTexture2_CreateFromMemory(reinterpret_cast<const ktx_uint8_t*>(fileData.data),
                                     fileData.size, static_cast<ktxTextureCreateFlags>(0),
                                     &ktxTexture2) > 0) {
      terminate("failed to parse ktx in %s", path);
    }
    image.width = ktxTexture2->baseWidth;
    image.height = ktxTexture2->baseHeight;
    image.isKtx = 1;

    image.mips = ktxTexture2->numLevels;
    ktx_transcode_fmt_e target_format = (ktx_transcode_fmt_e)format;

    if (ktxTexture2_NeedsTranscoding(ktxTexture2)) {
      ktxTexture2_TranscodeBasis(ktxTexture2, target_format, 0);
    }

    for (i32 i = 0, s = image.mips; i < s; i++) {
      size_t size = 0;
      ktxTexture_GetImageOffset((ktxTexture *)ktxTexture2, i, 0, 0, &size);
      image.mipSizes.push_back(size);
    }

    image.vkFormat = ktxTexture2->vkFormat;
    image.size = ktxTexture2->dataSize;
    image.data = new u8[image.size];
    image.channels = ktxTexture2_GetNumComponents(ktxTexture2);
    memcpy(image.data, ktxTexture2->pData, image.size);
    ktxTexture_Destroy((ktxTexture *)ktxTexture2);
    stringDestroy(&fileData);
  } else {
    String fileData = dataManagerRead(path);
    char is16bit =
        stbi_is_16_bit_from_memory(reinterpret_cast<const stbi_uc*>(fileData.data), fileData.size);
    int channelsInFile = 0;
    int channelsForce = 4;
    image.depth = is16bit ? 2 : 1;
    image.data = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(fileData.data), fileData.size,
                                       &image.width, &image.height,
                                       &channelsInFile, channelsForce);
    image.channels = channelsForce;
    image.size = image.width * image.height * image.channels * image.depth;
    stringDestroy(&fileData);
  }

  return image;
}

void imageDestory(Image *image) {
  if (image->isKtx) {
    delete[] static_cast<u8*>(image->data);
  } else if (!image->isRaw) {
    stbi_image_free(image->data);
  }
}

Image imageResize(Image *image, int newWidth, int newHeight) {
  assert(image->channels == 4);

  Image resizedImage = {};
  resizedImage.data =
      stbir_resize_uint8_srgb(reinterpret_cast<const unsigned char*>(image->data), image->width, image->height, 0, nullptr,
                              newWidth, newHeight, 0, STBIR_RGBA);
  resizedImage.width = newWidth;
  resizedImage.height = newHeight;
  resizedImage.channels = STBIR_RGBA;
  resizedImage.depth = 1;
  resizedImage.size = resizedImage.width * resizedImage.height *
                      resizedImage.channels * resizedImage.depth;
  return resizedImage;
}

// Optimized kernel generation with precomputed values
void generateGaussianKernel(float *kernel, int radius, float sigma) {
  float sum = 0.0f;
  int size = (2 * radius) + 1;
  float inv_2sigma_sq = 1.0f / (2.0f * sigma * sigma);

  for (int i = 0; i < size; i++) {
    int x = i - radius;
    kernel[i] = expf(-(x * x) * inv_2sigma_sq);
    sum += kernel[i];
  }

  float inv_sum = 1.0f / sum;
  for (int i = 0; i < size; i++) {
    kernel[i] *= inv_sum;
  }
}

// Cache-friendly horizontal convolution with loop unrolling
void convolveHorizontal(const float *input, float *output, int width,
                        int height, int num_channels, const float *kernel,
                        int radius) {
  // const int ksize = 2 * radius + 1;

  for (int y = 0; y < height; y++) {
    const float *row_input = &input[y * width * num_channels];
    float *row_output = &output[y * width * num_channels];

    for (int x = 0; x < width; x++) {
      for (int c = 0; c < num_channels; c++) {
        float sum = 0.0f;

        // Manual loop unrolling for common case (radius=3, ksize=7)
        if (radius == 3) {
          int base_idx = (x * num_channels) + c;

          // Clamp indices and accumulate
          int idx0 = ((x - 3) < 0 ? 0 : (x - 3)) * num_channels + c;
          int idx1 = ((x - 2) < 0 ? 0 : (x - 2)) * num_channels + c;
          int idx2 = ((x - 1) < 0 ? 0 : (x - 1)) * num_channels + c;
          int idx3 = base_idx;
          int idx4 =
              ((x + 1) >= width ? (width - 1) : (x + 1)) * num_channels + c;
          int idx5 =
              ((x + 2) >= width ? (width - 1) : (x + 2)) * num_channels + c;
          int idx6 =
              ((x + 3) >= width ? (width - 1) : (x + 3)) * num_channels + c;

          sum = row_input[idx0] * kernel[0] + row_input[idx1] * kernel[1] +
                row_input[idx2] * kernel[2] + row_input[idx3] * kernel[3] +
                row_input[idx4] * kernel[4] + row_input[idx5] * kernel[5] +
                row_input[idx6] * kernel[6];
        } else {
          // Generic case
          for (int k = -radius; k <= radius; k++) {
            int ix = x + k;
            if (ix < 0)
              ix = 0;
            if (ix >= width)
              ix = width - 1;
            sum += row_input[ix * num_channels + c] * kernel[k + radius];
          }
        }

        row_output[x * num_channels + c] = sum;
      }
    }
  }
}

// Cache-friendly vertical convolution with better memory access pattern
void convolveVertical(const float *input, float *output, int width, int height,
                      int num_channels, const float *kernel, int radius) {
  const long stride = width * num_channels;

  for (int x = 0; x < width; x++) {
    for (int c = 0; c < num_channels; c++) {
      const int base_offset = x * num_channels + c;

      for (int y = 0; y < height; y++) {
        float sum = 0.0f;

        // Manual loop unrolling for radius=3
        if (radius == 3) {
          long offset0 = ((y - 3) < 0 ? 0 : (y - 3)) * stride + base_offset;
          long offset1 = ((y - 2) < 0 ? 0 : (y - 2)) * stride + base_offset;
          long offset2 = ((y - 1) < 0 ? 0 : (y - 1)) * stride + base_offset;
          long offset3 = y * stride + base_offset;
          long offset4 = ((y + 1) >= height ? (height - 1) : (y + 1)) * stride +
                         base_offset;
          long offset5 = ((y + 2) >= height ? (height - 1) : (y + 2)) * stride +
                         base_offset;
          long offset6 = ((y + 3) >= height ? (height - 1) : (y + 3)) * stride +
                         base_offset;

          sum = input[offset0] * kernel[0] + input[offset1] * kernel[1] +
                input[offset2] * kernel[2] + input[offset3] * kernel[3] +
                input[offset4] * kernel[4] + input[offset5] * kernel[5] +
                input[offset6] * kernel[6];
        } else {
          // Generic case
          for (int k = -radius; k <= radius; k++) {
            int iy = y + k;
            if (iy < 0)
              iy = 0;
            if (iy >= height)
              iy = height - 1;
            sum += input[iy * stride + base_offset] * kernel[k + radius];
          }
        }

        output[y * stride + base_offset] = sum;
      }
    }
  }
}

// SIMD-optimized horizontal convolution (requires AVX2)
#ifdef __AVX2__
void convolveHorizontalSIMD(const float *input, float *output, int width,
                            int height, int num_channels, const float *kernel,
                            int radius) {
  if (num_channels != 1 || radius != 3) {
    // Fall back to regular implementation
    convolveHorizontal(input, output, width, height, num_channels, kernel,
                       radius);
    return;
  }

  const __m256 k0 = _mm256_set1_ps(kernel[0]);
  const __m256 k1 = _mm256_set1_ps(kernel[1]);
  const __m256 k2 = _mm256_set1_ps(kernel[2]);
  const __m256 k3 = _mm256_set1_ps(kernel[3]);
  const __m256 k4 = _mm256_set1_ps(kernel[4]);
  const __m256 k5 = _mm256_set1_ps(kernel[5]);
  const __m256 k6 = _mm256_set1_ps(kernel[6]);

  for (int y = 0; y < height; y++) {
    const float *row = &input[y * width];
    float *out_row = &output[y * width];

    // Process 8 pixels at a time
    int x;
    for (x = 3; x <= width - 3 - 8; x += 8) {
      __m256 sum = _mm256_setzero_ps();

      sum = _mm256_fmadd_ps(_mm256_loadu_ps(&row[x - 3]), k0, sum);
      sum = _mm256_fmadd_ps(_mm256_loadu_ps(&row[x - 2]), k1, sum);
      sum = _mm256_fmadd_ps(_mm256_loadu_ps(&row[x - 1]), k2, sum);
      sum = _mm256_fmadd_ps(_mm256_loadu_ps(&row[x]), k3, sum);
      sum = _mm256_fmadd_ps(_mm256_loadu_ps(&row[x + 1]), k4, sum);
      sum = _mm256_fmadd_ps(_mm256_loadu_ps(&row[x + 2]), k5, sum);
      sum = _mm256_fmadd_ps(_mm256_loadu_ps(&row[x + 3]), k6, sum);

      _mm256_storeu_ps(&out_row[x], sum);
    }

    // Handle remaining pixels with edge clamping
    for (; x < width; x++) {
      float sum = 0.0f;
      for (int k = -3; k <= 3; k++) {
        int ix = x + k;
        if (ix < 0)
          ix = 0;
        if (ix >= width)
          ix = width - 1;
        sum += row[ix] * kernel[k + 3];
      }
      out_row[x] = sum;
    }

    // Handle edge cases at beginning of row
    for (x = 0; x < 3; x++) {
      float sum = 0.0f;
      for (int k = -3; k <= 3; k++) {
        int ix = x + k;
        if (ix < 0)
          ix = 0;
        sum += row[ix] * kernel[k + 3];
      }
      out_row[x] = sum;
    }
  }
}
#endif

// Main blur function with optimizations
void gaussianBlur(const float *input, float *output, int width, int height,
                  int radius, float sigma) {
  int size = (2 * radius) + 1;
  float *kernel = static_cast<float*>(malloc(size * sizeof(float)));
#ifdef __linux
  float *temp = (float *)aligned_alloc(
      32, static_cast<long>(width) * height * sizeof(float)); // 32-byte aligned for SIMD
#else
  float *temp = (float *)_aligned_malloc(
      32, static_cast<long>(width) * height * sizeof(float)); // 32-byte aligned for SIMD
#endif

  generateGaussianKernel(kernel, radius, sigma);

#if defined(__AVX2__)
  convolveHorizontalSIMD(input, temp, width, height, 1, kernel, radius);
  convolveVertical(temp, output, width, height, 1, kernel, radius);
#else
  convolveHorizontal(input, temp, width, height, 1, kernel, radius);
  convolveVertical(temp, output, width, height, 1, kernel, radius);
#endif

  free(kernel);
  free(temp);
}

// Alternative: Box blur approximation (much faster for large radii)
void boxBlur(const float *input, float *output, int width, int height,
             int radius) {
  float *temp = static_cast<float*>(malloc(static_cast<long>(width) * height * sizeof(float)));
  float inv_size = 1.0f / (2 * radius + 1);

  // Horizontal pass
  for (int y = 0; y < height; y++) {
    float sum = 0.0f;

    // Initialize sum for first pixel
    for (int k = -radius; k <= radius; k++) {
      int ix = (k < 0) ? 0 : k;
      if (ix >= width)
        ix = width - 1;
      sum += input[y * width + ix];
    }
    temp[y * width] = sum * inv_size;

    // Sliding window for rest of row
    for (int x = 1; x < width; x++) {
      int left = x - radius - 1;
      int right = x + radius;

      if (left >= 0)
        sum -= input[y * width + left];
      else
        sum -= input[y * width]; // Clamp to edge

      if (right < width)
        sum += input[y * width + right];
      else
        sum += input[y * width + width - 1]; // Clamp to edge

      temp[y * width + x] = sum * inv_size;
    }
  }

  // Vertical pass with similar sliding window optimization
  for (int x = 0; x < width; x++) {
    float sum = 0.0f;

    for (int k = -radius; k <= radius; k++) {
      int iy = (k < 0) ? 0 : k;
      if (iy >= height)
        iy = height - 1;
      sum += temp[iy * width + x];
    }
    output[x] = sum * inv_size;

    for (int y = 1; y < height; y++) {
      int top = y - radius - 1;
      int bottom = y + radius;

      if (top >= 0)
        sum -= temp[top * width + x];
      else
        sum -= temp[x];

      if (bottom < height)
        sum += temp[bottom * width + x];
      else
        sum += temp[(height - 1) * width + x];

      output[y * width + x] = sum * inv_size;
    }
  }

  free(temp);
}

void gaussianBlurNormalMap(const signed char *input_normal_map,
                           signed char *output_normal_map, int width,
                           int height, int radius, float sigma) {
  const int num_channels = 4;
  long total_elements = static_cast<long>(width) * height * num_channels;
  float *input_float = static_cast<float*>(malloc(total_elements * sizeof(float)));
  for (long i = 0; i < total_elements; i++)
    input_float[i] = static_cast<float>(input_normal_map[i]);
  int kernel_size = (2 * radius) + 1;
  float *kernel = static_cast<float*>(malloc(kernel_size * sizeof(float)));
  generateGaussianKernel(kernel, radius, sigma);
  float *temp_float = static_cast<float*>(malloc(total_elements * sizeof(float)));
  float *output_float_temp = static_cast<float*>(malloc(total_elements * sizeof(float)));
  convolveHorizontal(input_float, temp_float, width, height, num_channels,
                     kernel, radius);
  convolveVertical(temp_float, output_float_temp, width, height, num_channels,
                   kernel, radius);
  for (long i = 0; i < total_elements; i++) {
    float val = output_float_temp[i];
    val = roundf(val);
    if (val > 127.0F) {
      val = 127.0F;
    } else if (val < -128.0F) {
      val = -128.0F;
    }
    output_normal_map[i] = (signed char)val;
  }
  free(kernel);
  free(temp_float);
  free(output_float_temp);
  free(input_float);
}

// Simple box blur for 4-channel normal maps (matching your Gaussian approach)
void boxBlurNormalMap(const signed char *input_normal_map,
                      signed char *output_normal_map, int width, int height,
                      int radius) {
  const int num_channels = 4; // RGBA like your Gaussian version
  const long total_elements = static_cast<long>(width) * height * num_channels;

  // Convert to float (like your Gaussian version)
  float *input_float = static_cast<float*>(malloc(total_elements * sizeof(float)));
  for (long i = 0; i < total_elements; i++) {
    input_float[i] = static_cast<float>(input_normal_map[i]);
  }

  float *temp_float = static_cast<float*>(malloc(total_elements * sizeof(float)));
  float *output_float = static_cast<float*>(malloc(total_elements * sizeof(float)));

  const float inv_kernel_size = 1.0f / (2 * radius + 1);

  // Horizontal pass
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      for (int c = 0; c < num_channels; c++) {
        float sum = 0.0f;

        for (int k = -radius; k <= radius; k++) {
          int ix = x + k;
          if (ix < 0)
            ix = 0;
          if (ix >= width)
            ix = width - 1;
          sum += input_float[((long)y * width + ix) * num_channels + c];
        }

        temp_float[((long)y * width + x) * num_channels + c] =
            sum * inv_kernel_size;
      }
    }
  }

  // Vertical pass
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      for (int c = 0; c < num_channels; c++) {
        float sum = 0.0f;

        for (int k = -radius; k <= radius; k++) {
          int iy = y + k;
          if (iy < 0)
            iy = 0;
          if (iy >= height)
            iy = height - 1;
          sum += temp_float[((long)iy * width + x) * num_channels + c];
        }

        output_float[((long)y * width + x) * num_channels + c] =
            sum * inv_kernel_size;
      }
    }
  }

  // Convert back to signed char (exactly like your Gaussian version)
  for (long i = 0; i < total_elements; i++) {
    float val = output_float[i];
    val = roundf(val);
    if (val > 127.0f) {
      val = 127.0f;
    } else if (val < -128.0f) {
      val = -128.0f;
    }
    output_normal_map[i] = (signed char)val;
  }

  free(input_float);
  free(temp_float);
  free(output_float);
}

// Optimized version with sliding window
void boxBlurNormalMapFast(const signed char *input_normal_map,
                          signed char *output_normal_map, int width, int height,
                          int radius) {
  const int num_channels = 4;
  const long total_elements = static_cast<long>(width) * height * num_channels;

  float *input_float = static_cast<float*>(malloc(total_elements * sizeof(float)));
  for (long i = 0; i < total_elements; i++) {
    input_float[i] = static_cast<float>(input_normal_map[i]);
  }

  float *temp_float = static_cast<float*>(malloc(total_elements * sizeof(float)));
  float *output_float = static_cast<float*>(malloc(total_elements * sizeof(float)));

  const float inv_kernel_size = 1.0f / (2 * radius + 1);

  // Horizontal pass with sliding window optimization
  for (int y = 0; y < height; y++) {
    const long row_offset = (long)y * width * num_channels;

    for (int c = 0; c < num_channels; c++) {
      float sum = 0.0f;

      // Initialize sum for first pixel in row
      for (int k = -radius; k <= radius; k++) {
        int ix = (k < 0) ? 0 : k;
        if (ix >= width)
          ix = width - 1;
        sum += input_float[row_offset + ix * num_channels + c];
      }
      temp_float[row_offset + c] = sum * inv_kernel_size;

      // Sliding window for rest of row
      for (int x = 1; x < width; x++) {
        int left = x - radius - 1;
        int right = x + radius;

        // Remove leftmost pixel from sum
        if (left >= 0) {
          sum -= input_float[row_offset + left * num_channels + c];
        } else {
          sum -= input_float[row_offset + c]; // Clamp to edge
        }

        // Add rightmost pixel to sum
        if (right < width) {
          sum += input_float[row_offset + right * num_channels + c];
        } else {
          sum += input_float[row_offset + (width - 1) * num_channels +
                             c]; // Clamp to edge
        }

        temp_float[row_offset + x * num_channels + c] = sum * inv_kernel_size;
      }
    }
  }

  // Vertical pass with sliding window optimization
  for (int x = 0; x < width; x++) {
    for (int c = 0; c < num_channels; c++) {
      const long col_offset = x * num_channels + c;
      float sum = 0.0f;

      // Initialize sum for first pixel in column
      for (int k = -radius; k <= radius; k++) {
        int iy = (k < 0) ? 0 : k;
        if (iy >= height)
          iy = height - 1;
        sum += temp_float[(long)iy * width * num_channels + col_offset];
      }
      output_float[col_offset] = sum * inv_kernel_size;

      // Sliding window for rest of column
      for (int y = 1; y < height; y++) {
        int top = y - radius - 1;
        int bottom = y + radius;

        // Remove topmost pixel from sum
        if (top >= 0) {
          sum -= temp_float[(long)top * width * num_channels + col_offset];
        } else {
          sum -= temp_float[col_offset]; // Clamp to edge
        }

        // Add bottommost pixel to sum
        if (bottom < height) {
          sum += temp_float[(long)bottom * width * num_channels + col_offset];
        } else {
          sum += temp_float[(long)(height - 1) * width * num_channels +
                            col_offset]; // Clamp to edge
        }

        output_float[(long)y * width * num_channels + col_offset] =
            sum * inv_kernel_size;
      }
    }
  }

  // Convert back to signed char (exactly like your Gaussian version)
  for (long i = 0; i < total_elements; i++) {
    float val = output_float[i];
    val = roundf(val);
    if (val > 127.0f) {
      val = 127.0f;
    } else if (val < -128.0f) {
      val = -128.0f;
    }
    output_normal_map[i] = (signed char)val;
  }

  free(input_float);
  free(temp_float);
  free(output_float);
}

// Multi-pass box blur for better Gaussian approximation
void multiPassBoxBlurNormalMap(const signed char *input_normal_map,
                               signed char *output_normal_map, int width,
                               int height, int radius, int passes) {
  if (passes <= 0)
    passes = 1;
  if (passes == 1) {
    boxBlurNormalMapFast(input_normal_map, output_normal_map, width, height,
                         radius);
    return;
  }

  const long total_elements = static_cast<long>(width) * height * 4;
  signed char *temp_buffers[2];
  temp_buffers[0] = (signed char *)malloc(total_elements);
  temp_buffers[1] = (signed char *)malloc(total_elements);

  // First pass: input -> temp_buffers[0]
  boxBlurNormalMapFast(input_normal_map, temp_buffers[0], width, height,
                       radius);

  // Middle passes: ping-pong between temp buffers
  for (int pass = 1; pass < passes - 1; pass++) {
    int src = (pass - 1) % 2;
    int dst = pass % 2;
    boxBlurNormalMapFast(temp_buffers[src], temp_buffers[dst], width, height,
                         radius);
  }

  // Final pass: temp_buffer -> output
  if (passes > 1) {
    int final_src = (passes - 2) % 2;
    boxBlurNormalMapFast(temp_buffers[final_src], output_normal_map, width,
                         height, radius);
  }

  free(temp_buffers[0]);
  free(temp_buffers[1]);
}

// Box blur that ignores FLT_MAX values (holes) and preserves them
void boxBlurWithHoles(const float *input, float *output, int width, int height,
                      int radius) {
  float *temp = static_cast<float*>(malloc(static_cast<long>(width) * height * sizeof(float)));

  // Horizontal pass
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      // If current pixel is a hole, preserve it
      if (input[y * width + x] == FLT_MAX) {
        temp[y * width + x] = FLT_MAX;
        continue;
      }

      float sum = 0.0f;
      int count = 0;

      // Sum valid pixels in horizontal window
      for (int k = -radius; k <= radius; k++) {
        int ix = x + k;
        // Clamp to edges
        if (ix < 0)
          ix = 0;
        if (ix >= width)
          ix = width - 1;

        float val = input[y * width + ix];
        if (val != FLT_MAX) {
          sum += val;
          count++;
        }
      }

      // If no valid pixels found, mark as hole
      if (count == 0) {
        temp[y * width + x] = FLT_MAX;
      } else {
        temp[y * width + x] = sum / count;
      }
    }
  }

  // Vertical pass
  for (int x = 0; x < width; x++) {
    for (int y = 0; y < height; y++) {
      // If current pixel is a hole, preserve it
      if (temp[y * width + x] == FLT_MAX) {
        output[y * width + x] = FLT_MAX;
        continue;
      }

      float sum = 0.0f;
      int count = 0;

      // Sum valid pixels in vertical window
      for (int k = -radius; k <= radius; k++) {
        int iy = y + k;
        // Clamp to edges
        if (iy < 0)
          iy = 0;
        if (iy >= height)
          iy = height - 1;

        float val = temp[iy * width + x];
        if (val != FLT_MAX) {
          sum += val;
          count++;
        }
      }

      // If no valid pixels found, mark as hole
      if (count == 0) {
        output[y * width + x] = FLT_MAX;
      } else {
        output[y * width + x] = sum / count;
      }
    }
  }

  free(temp);
}

// Optimized version with sliding window (more complex but faster)
void boxBlurWithHolesOptimized(const float *input, float *output, int width,
                               int height, int radius) {
  float *temp = static_cast<float*>(malloc(static_cast<long>(width) * height * sizeof(float)));

  // Horizontal pass with sliding window
  for (int y = 0; y < height; y++) {
    // Process each pixel
    for (int x = 0; x < width; x++) {
      if (input[y * width + x] == FLT_MAX) {
        temp[y * width + x] = FLT_MAX;
        continue;
      }

      float sum = 0.0f;
      int count = 0;

      for (int k = -radius; k <= radius; k++) {
        int ix = x + k;
        if (ix < 0)
          ix = 0;
        if (ix >= width)
          ix = width - 1;

        float val = input[y * width + ix];
        if (val != FLT_MAX) {
          sum += val;
          count++;
        }
      }

      temp[y * width + x] = (count > 0) ? sum / count : FLT_MAX;
    }
  }

  // Vertical pass with sliding window
  for (int x = 0; x < width; x++) {
    for (int y = 0; y < height; y++) {
      if (temp[y * width + x] == FLT_MAX) {
        output[y * width + x] = FLT_MAX;
        continue;
      }

      float sum = 0.0f;
      int count = 0;

      for (int k = -radius; k <= radius; k++) {
        int iy = y + k;
        if (iy < 0)
          iy = 0;
        if (iy >= height)
          iy = height - 1;

        float val = temp[iy * width + x];
        if (val != FLT_MAX) {
          sum += val;
          count++;
        }
      }

      output[y * width + x] = (count > 0) ? sum / count : FLT_MAX;
    }
  }

  free(temp);
}
}  // namespace utils
