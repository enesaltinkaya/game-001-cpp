#include "ExrLoader.h"
#include <openexr.h>

// ---------------------------------------------------------------------------
// Memory-stream userdata + callbacks for OpenEXRCore
// ---------------------------------------------------------------------------

struct ExrMemStream {
    const uint8_t* data;
    uint64_t       size;
};

static int64_t exrMemRead(
    exr_const_context_t         ctxt,
    void*                       userdata,
    void*                       buffer,
    uint64_t                    sz,
    uint64_t                    offset,
    exr_stream_error_func_ptr_t error_cb)
{
    (void)ctxt;
    ExrMemStream* s  = static_cast<ExrMemStream*>(userdata);
    if (offset >= s->size) {
        if (error_cb) error_cb(ctxt, EXR_ERR_READ_IO, "read past end");
        return -1;
    }
    uint64_t avail = s->size - offset;
    if (sz > avail) sz = avail;
    memcpy(buffer, s->data + offset, sz);
    return static_cast<int64_t>(sz);
}

static int64_t exrMemSize(exr_const_context_t ctxt, void* userdata)
{
    (void)ctxt;
    ExrMemStream* s  = static_cast<ExrMemStream*>(userdata);
    return static_cast<int64_t>(s->size);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

float* exrLoadFromMemory(
    const void* data, u64 dataSize, int* outWidth, int* outHeight)
{
    *outWidth  = 0;
    *outHeight = 0;

    ExrMemStream stream = {reinterpret_cast<const uint8_t*>(data), dataSize};

    exr_context_initializer_t init = EXR_DEFAULT_CONTEXT_INITIALIZER;
    init.user_data = &stream;
    init.read_fn   = exrMemRead;
    init.size_fn   = exrMemSize;

    exr_context_t ctx = nullptr;
    exr_result_t  rv  = exr_start_read(&ctx, "<memory>", &init);
    if (rv != EXR_ERR_SUCCESS) {
        warn("exrLoad: exr_start_read failed: %s", exr_get_error_code_as_string(rv));
        return nullptr;
    }

    // Use part 0
    const int partIdx = 0;

    // Get data window -> image dimensions
    exr_attr_box2i_t dataWindow;
    rv = exr_get_data_window(ctx, partIdx, &dataWindow);
    if (rv != EXR_ERR_SUCCESS) {
        warn("exrLoad: exr_get_data_window failed");
        exr_finish(&ctx);
        return nullptr;
    }

    int width  = dataWindow.max.x - dataWindow.min.x + 1;
    int height = dataWindow.max.y - dataWindow.min.y + 1;
    if (width <= 0 || height <= 0) {
        warn("exrLoad: invalid dimensions %dx%d", width, height);
        exr_finish(&ctx);
        return nullptr;
    }

    // Get channel list
    const exr_attr_chlist_t* chlist = nullptr;
    rv = exr_get_channels(ctx, partIdx, &chlist);
    if (rv != EXR_ERR_SUCCESS || !chlist) {
        warn("exrLoad: exr_get_channels failed");
        exr_finish(&ctx);
        return nullptr;
    }

    // Find R, G, B, A channel indices (-1 if absent)
    int chIdxR = -1, chIdxG = -1, chIdxB = -1, chIdxA = -1;
    for (int i = 0; i < chlist->num_channels; i++) {
        const char* name = chlist->entries[i].name.str;
        if      (strcmp(name, "R") == 0) chIdxR = i;
        else if (strcmp(name, "G") == 0) chIdxG = i;
        else if (strcmp(name, "B") == 0) chIdxB = i;
        else if (strcmp(name, "A") == 0) chIdxA = i;
    }

    if (chIdxR < 0 || chIdxG < 0 || chIdxB < 0) {
        warn("exrLoad: missing R/G/B channels");
        exr_finish(&ctx);
        return nullptr;
    }

    // Allocate output: RGBA float32
    u64 pixelCount = static_cast<u64>(width) * height;
    float* pixels = static_cast<float*>(memoryAlloc(pixelCount * 4 * sizeof(float)));

    // Get scanlines-per-chunk
    int32_t scanlinesPerChunk = 0;
    exr_get_scanlines_per_chunk(ctx, partIdx, &scanlinesPerChunk);
    if (scanlinesPerChunk <= 0) scanlinesPerChunk = 1;

    // Stride: output is RGBA interleaved float32, so 16 bytes per pixel
    int32_t pixelStride = 4 * static_cast<int32_t>(sizeof(float)); // 16
    int32_t lineStride  = width * pixelStride;

    // Decode chunk by chunk
    int32_t chunkCount = 0;
    exr_get_chunk_count(ctx, partIdx, &chunkCount);

    exr_decode_pipeline_t decoder = EXR_DECODE_PIPELINE_INITIALIZER;
    bool decoderInited = false;

    for (int32_t ci = 0; ci < chunkCount; ci++) {
        exr_chunk_info_t cinfo;
        rv = exr_read_scanline_chunk_info(ctx, partIdx,
                                          dataWindow.min.y + ci * scanlinesPerChunk,
                                          &cinfo);
        if (rv != EXR_ERR_SUCCESS) {
            warn("exrLoad: chunk info %d failed: %s", ci, exr_get_error_code_as_string(rv));
            goto fail;
        }

        if (!decoderInited) {
            rv = exr_decoding_initialize(ctx, partIdx, &cinfo, &decoder);
            if (rv != EXR_ERR_SUCCESS) {
                warn("exrLoad: decoding_initialize failed: %s", exr_get_error_code_as_string(rv));
                goto fail;
            }
            decoderInited = true;
        } else {
            rv = exr_decoding_update(ctx, partIdx, &cinfo, &decoder);
            if (rv != EXR_ERR_SUCCESS) {
                warn("exrLoad: decoding_update failed: %s", exr_get_error_code_as_string(rv));
                goto fail;
            }
        }

        // Set up channel output pointers for this chunk
        int chunkY0    = cinfo.start_y - dataWindow.min.y;
        uint8_t* base  = reinterpret_cast<uint8_t*>(pixels) + static_cast<u64>(chunkY0) * lineStride;

        for (int16_t ch = 0; ch < decoder.channel_count; ch++) {
            exr_coding_channel_info_t* info = &decoder.channels[ch];
            const char* name = info->channel_name;

            int outOff = -1; // byte offset within each RGBA pixel
            if      (strcmp(name, "R") == 0) outOff = 0;
            else if (strcmp(name, "G") == 0) outOff = 1 * static_cast<int>(sizeof(float));
            else if (strcmp(name, "B") == 0) outOff = 2 * static_cast<int>(sizeof(float));
            else if (strcmp(name, "A") == 0) outOff = 3 * static_cast<int>(sizeof(float));

            if (outOff >= 0) {
                info->decode_to_ptr       = base + outOff;
                info->user_pixel_stride   = pixelStride;
                info->user_line_stride    = lineStride;
                info->user_bytes_per_element = static_cast<int16_t>(sizeof(float));
                info->user_data_type      = static_cast<uint16_t>(EXR_PIXEL_FLOAT);
            } else {
                // Skip unknown channels
                info->decode_to_ptr = nullptr;
            }
        }

        rv = exr_decoding_choose_default_routines(ctx, partIdx, &decoder);
        if (rv != EXR_ERR_SUCCESS) {
            warn("exrLoad: choose_default_routines failed: %s", exr_get_error_code_as_string(rv));
            goto fail;
        }

        rv = exr_decoding_run(ctx, partIdx, &decoder);
        if (rv != EXR_ERR_SUCCESS) {
            warn("exrLoad: decoding_run chunk %d failed: %s", ci, exr_get_error_code_as_string(rv));
            goto fail;
        }
    }

    exr_decoding_destroy(ctx, &decoder);
    exr_finish(&ctx);

    // Fill alpha = 1.0 if no A channel
    if (chIdxA < 0) {
        for (u64 i = 0; i < pixelCount; i++) {
            pixels[i * 4 + 3] = 1.0f;
        }
    }

    *outWidth  = width;
    *outHeight = height;
    return pixels;

fail:
    if (decoderInited) exr_decoding_destroy(ctx, &decoder);
    exr_finish(&ctx);
    memoryFree(pixels);
    return nullptr;
}
