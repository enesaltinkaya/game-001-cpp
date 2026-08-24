#pragma once
#include <vector>

/// Load an EXR image from a memory buffer.
/// Returns a vector of RGBA float32 pixels (empty on failure).
/// @param data       Raw EXR file bytes
/// @param dataSize   Size of the data buffer
/// @param outWidth   Receives image width
/// @param outHeight  Receives image height
namespace engine {
std::vector<float> exrLoadFromMemory(
    const void* data, u64 dataSize, int* outWidth, int* outHeight);
}  // namespace engine
