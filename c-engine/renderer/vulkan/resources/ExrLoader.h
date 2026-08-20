#pragma once

/// Load an EXR image from a memory buffer.
/// Returns a malloc'd array of RGBA float32 pixels, or NULL on failure.
/// Caller must free the returned pointer with memoryFree().
/// @param data       Raw EXR file bytes
/// @param dataSize   Size of the data buffer
/// @param outWidth   Receives image width
/// @param outHeight  Receives image height
float* exrLoadFromMemory(
    const void* data, u64 dataSize, int* outWidth, int* outHeight);
