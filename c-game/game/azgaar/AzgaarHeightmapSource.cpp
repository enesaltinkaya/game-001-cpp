#include "azgaar/AzgaarHeightmapSource.h"
#include "azgaar/AzgaarSettlements.h"
#include "ecs/system/heightmap/HeightmapTerrain.h"

// ── Deterministic value noise (world-anchored) ─────────────────────────────

// Geometry-band fBm: 2 octaves, wavelengths 128/64 m. The band is limited to
// wavelengths >= 64 m so every frequency is well above Nyquist on ALL views
// of the surface: the 4 m CPU grid (>= 16 samples), the 8.03 m physics grid
// and ring-0 render lattice (>= 8 samples), and the coarser far rings.
// Shorter wavelengths (32/16/8/4 m) live in the fragment shader's micro band
// (normal perturbation only): putting them in geometry would alias on the
// ring-0 lattice (a 16 m octave is 1 sample/wavelength there — it vanishes
// from the rendered surface while CPU/physics keep it, so grass and the
// player float above the rendered ground).
enum {
    AZGAAR_HM_DETAIL_OCTAVES = 2,
};

static const float azgaarHmDetailWavelengths[AZGAAR_HM_DETAIL_OCTAVES] = {128.0f, 64.0f};
static const float azgaarHmDetailAmplitudes[AZGAAR_HM_DETAIL_OCTAVES]  = {1.2f, 0.8f};

static u32 azgaarHmHash(u32 x, u32 y, u32 seed) {
    u32 h = x * 0x8da6b343u;
    h     = h * 1013904223u + y * 0xc2b2ae35u + seed;
    h     = (h ^ (h >> 15)) * 0x2c1b3c6du;
    h     = (h ^ (h >> 12)) * 0x297a2d39u;
    h     = h ^ (h >> 15);
    return h;
}

// Hash lattice value noise, output in [-1, 1]. Lattice cells are 1 unit;
// callers pre-scale coordinates by 1/wavelength.
static float azgaarHmValueNoise(float x, float y, u32 seed) {
    i32 xi = static_cast<i32>(floorf(x));
    i32 yi = static_cast<i32>(floorf(y));
    float xf = x - static_cast<float>(xi);
    float yf = y - static_cast<float>(yi);
    float u  = xf * xf * (3.0f - 2.0f * xf);
    float v  = yf * yf * (3.0f - 2.0f * yf);

    u32 kx = static_cast<u32>(xi);
    u32 ky = static_cast<u32>(yi);
    float a = static_cast<float>(azgaarHmHash(kx, ky, seed));
    float b = static_cast<float>(azgaarHmHash(kx + 1, ky, seed));
    float c = static_cast<float>(azgaarHmHash(kx, ky + 1, seed));
    float d = static_cast<float>(azgaarHmHash(kx + 1, ky + 1, seed));
    a      *= 1.0f / 4294967295.0f;
    b      *= 1.0f / 4294967295.0f;
    c      *= 1.0f / 4294967295.0f;
    d      *= 1.0f / 4294967295.0f;

    float top    = a + (b - a) * u;
    float bottom = c + (d - c) * u;
    return 2.0f * (top + (bottom - top) * v) - 1.0f;
}

static float azgaarHmDetail(const AzgaarHeightmapSource* src, float wx, float wz) {
    float sum = 0.0f;
    for (i32 i = 0; i < AZGAAR_HM_DETAIL_OCTAVES; ++i) {
        float inv = 1.0f / azgaarHmDetailWavelengths[i];
        u32 seed  = src->noiseSeed + static_cast<u32>(i + 1) * 1013904223u;
        sum += azgaarHmDetailAmplitudes[i] * azgaarHmValueNoise(wx * inv, wz * inv, seed);
    }
    return sum;
}

// ── HeightmapSource callbacks ──────────────────────────────────────────────

static float azgaarHeightmapHeightAt(void* userData, float wx, float wz) {
    AzgaarHeightmapSource* src   = (AzgaarHeightmapSource*)userData;
    const AzgaarWorld* world     = src->world;
    if (!world) return 0.0f;

    // World -> map pixels (inverse of azgaarMapToWorld).
    float mapX = static_cast<float>(world->widthPx) * 0.5f - wx / static_cast<float>(world->metersPerPixel);
    float mapY = static_cast<float>(world->heightPx) * 0.5f - wz / static_cast<float>(world->metersPerPixel);

    float h      = azgaarWorldSampleHeightSmooth(world, mapX, mapY);
    float meters = azgaarHeightToMeters(world, h);

    if (src->detailEnabled) {
        // Fade the detail out below ~-10 m so the deep seabed keeps the clean
        // FMG shelf (nobody walks there; the water surface hides it anyway).
        float fade = (meters + 10.0f) / 10.0f;
        if (fade < 0.0f) fade = 0.0f;
        else if (fade > 1.0f) fade = 1.0f;
        meters += azgaarHmDetail(src, wx, wz) * fade;
    }

    // D8 (workstream D): blend the (natural + detail) height toward each
    // nearby settlement's flatY LAST, so the plateau wins inside the
    // settlement radius and the ground under buildings is exactly flatY.
    // (If the detail ran after the plateau, its ±2 m noise would make the
    // houses float above the ground.)
    meters = azgaarSettlementsPlateauY(world, wx, wz, meters);

    return meters;
}

// ── Init ───────────────────────────────────────────────────────────────────

static u32 azgaarHeightmapSourceSeed(const char* name) {
    u32 h = 2166136261u; // FNV-1a
    if (name) {
        for (const char* p = name; *p; ++p) {
            h ^= static_cast<u32>(static_cast<unsigned char>(*p));
            h *= 16777619u;
        }
    }
    return h ? h : 1u;
}

void azgaarHeightmapSourceInit(AzgaarHeightmapSource* src,
                               const AzgaarWorld* world,
                               const char* mapName) {
    src->world         = world;
    src->noiseSeed     = azgaarHeightmapSourceSeed(mapName);
    src->detailEnabled = true;
    src->vtable.heightAt = azgaarHeightmapHeightAt;
    src->vtable.userData = src;
}