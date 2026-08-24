#include "azgaar/AzgaarRivers.h"
#include "azgaar/AzgaarWorld.h"
#include "ecs/system/heightmap/HeightmapTerrain.h"
#include "renderer/decal/Decal.h"
#include "renderer/vulkan/scene/VulkanScene.h"
#include "renderer/vulkan/pass/azgaar_river/VulkanAzgaarRiverPass.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace game {
static vec3 Y_UP = {0.0f, 1.0f, 0.0f};

// One spatial-hash entry: a resampled river centerline point in world space.
// Kept parallel to AzgaarRiverNearHit (the query result type).
struct RiverHashPoint {
    float wx, wz;
    u32   riverId;
    float widthM;
};

// ── Azgaar rivers (workstream C, plans/azgaar-world-population.md) ────
// Builds, from the parsed AzgaarWorld rivers (section 32 metadata + section
// 5 SVG centerlines):
//   • a thin animated ribbon mesh uploaded to the azgaar_river pass
//   • a river-point spatial hash (10 m buckets) shared with riparian (B) and
//     bridges (E)
//   • a dark wet-earth ground strip via GROUND_ONLY decals
// All of it is gated by the ENGINE_AZGAAR_RIVERS_DISABLED kill switch.

#define AZGAAR_RIVER_SPACING_M   10.0f  // centerline resample spacing
#define AZGAAR_RIVER_MIN_W_M     2.0f   // min ribbon width
#define AZGAAR_RIVER_MAX_W_M     40.0f   // max ribbon width
#define AZGAAR_RIVER_LIFT_M      0.03f   // z-fight guard lift above surface
#define AZGAAR_RIVER_BUCKET_M    10.0f   // hash bucket size
#define AZGAAR_RIVER_STRIP_SPACING_M 40.0f // wet decal spacing
#define AZGAAR_RIVER_DECAL_MAX   8192u

// ── Growable CPU arrays ─────────────────────────────────────────────────
static std::vector<engine::SceneVertex> g_verts;
static size_t g_vertCount, g_vertCap;
static std::vector<u32> g_indices;
static size_t g_idxCount, g_idxCap;
static std::vector<RiverHashPoint> g_hashPts;
static size_t g_hashCount, g_hashCap;

static void growVerts(size_t need) {
    if (need <= g_vertCap) return;
    size_t cap = g_vertCap ? g_vertCap : 4096u;
    while (cap < need) cap *= 2u;
    g_verts.resize(cap);
    g_vertCap = cap;
}
static void growIndices(size_t need) {
    if (need <= g_idxCap) return;
    size_t cap = g_idxCap ? g_idxCap : 8192u;
    while (cap < need) cap *= 2u;
    g_indices.resize(cap);
    g_idxCap = cap;
}
static void growHash(size_t need) {
    if (need <= g_hashCap) return;
    size_t cap = g_hashCap ? g_hashCap : 4096u;
    while (cap < need) cap *= 2u;
    g_hashPts.resize(cap);
    g_hashCap = cap;
}

// A single resampled centerline sample (world space).
struct RiverSample {
    float wx, wy, wz;
    float tx, tz;   // unit flow direction (source → mouth)
    float arcLen;   // cumulative arc length in metres
    float widthM;
};

// ── Hash storage (sparse) ───────────────────────────────────────────────
// The world spans hundreds of km, so a dense numBX*numBY grid would be
// gigabytes.  Instead: a compact list of DISTINCT buckets + an open
// addressing map from packed (by,bx) key → compact index.  g_hashPts stays
// bucket-sorted; g_bucketStart holds start offsets (size = distinct+1).
static std::vector<u64> g_bucketKeys;      // one packed (by,bx) key per distinct bucket
static u32              g_bucketCount;     // number of distinct buckets
static std::vector<u32> g_bucketStart;     // start offsets into g_hashPts (size = distinct+1)
static std::vector<u64> g_bucketMap;       // open-addressing key table
static std::vector<u8>  g_mapUsed;         // per-slot used flag (so key == 0 is representable)
static std::vector<u32> g_bucketMapIdx;    // compact bucket index per occupied slot
static size_t g_mapCap;        // power-of-two table size
static bool   g_hashReady;

static u64 packBucket(int by, int bx) {
    // Two's-complement via u32 cast so negative coordinates pack correctly.
    return (static_cast<u64>(static_cast<u32>(static_cast<int32_t>(by))) << 32) | static_cast<u32>(static_cast<int32_t>(bx));
}

static u32 bucketLookup(u64 key) {
    if (g_mapUsed.empty() || g_mapCap == 0) return 0xFFFFFFFFu;
    size_t idx = static_cast<size_t>((key * 0x9E3779B97F4A7C15ULL) & static_cast<u64>(g_mapCap - 1));
    while (g_mapUsed[idx]) {
        if (g_bucketMap[idx] == key) return g_bucketMapIdx[idx];
        idx = (idx + 1) & (g_mapCap - 1);
    }
    return 0xFFFFFFFFu;
}

static void bucketInsert(u64 key, u32 compactIdx) {
    size_t idx = static_cast<size_t>((key * 0x9E3779B97F4A7C15ULL) & static_cast<u64>(g_mapCap - 1));
    while (g_mapUsed[idx]) {
        if (g_bucketMap[idx] == key) { g_bucketMapIdx[idx] = compactIdx; return; }
        idx = (idx + 1) & (g_mapCap - 1);
    }
    g_bucketMap[idx] = key;
    g_bucketMapIdx[idx] = compactIdx;
    g_mapUsed[idx] = 1;
}

// Free all sparse hash structures (called before a rebuild and on clear).
static void freeHashStructs(void) {
    g_bucketKeys.clear();
    g_bucketStart.clear();
    g_bucketMap.clear();
    g_mapUsed.clear();
    g_bucketMapIdx.clear();
    g_bucketCount = 0;
    g_mapCap = 0;
}

// ── Wet-strip decals ────────────────────────────────────────────────────
static u32  g_decals[AZGAAR_RIVER_DECAL_MAX];
static u32  g_decalCount;

static void decalRemoveAll(void) {
    for (u32 i = 0; i < g_decalCount; ++i) engine::decalRemove(g_decals[i]);
    g_decalCount = 0;
}

void azgaarRiversClear(void) {
    engine::vulkanAzgaarRiverClear();
    decalRemoveAll();
    g_hashPts.clear();
    freeHashStructs();
    g_verts.clear();
    g_indices.clear();
    g_vertCount = g_vertCap = 0;
    g_idxCount = g_idxCap = 0;
    g_hashCount = g_hashCap = 0;
    g_hashReady = false;
}

// Fast bilinear read of the raw height grid (FMG 0..100) at map px.  Used as
// the cheap fallback when the heightmap terrain is not active, replacing the
// slower 4x4 Catmull-Rom smoothing (azgaarWorldSampleHeightSmooth) on the
// per-sample resample path.
static float azgaarDirectHeight(const AzgaarWorld* world, float xPx, float yPx) {
    if (world->heightGrid.empty() || world->heightGridWidth < 2 || world->heightGridHeight < 2) return 0.0f;
    float cellW = static_cast<float>(world->widthPx) / static_cast<float>(world->heightGridWidth);
    float cellH = static_cast<float>(world->heightPx) / static_cast<float>(world->heightGridHeight);
    float gx = xPx / cellW - 0.5f;
    float gy = yPx / cellH - 0.5f;
    i32 x1 = static_cast<i32>(floorf(gx));
    i32 y1 = static_cast<i32>(floorf(gy));
    float tx = gx - static_cast<float>(x1);
    float ty = gy - static_cast<float>(y1);
    if (x1 < 0) { x1 = 0; tx = 0.0f; }
    if (y1 < 0) { y1 = 0; ty = 0.0f; }
    if (x1 > static_cast<i32>(world->heightGridWidth) - 2) { x1 = static_cast<i32>(world->heightGridWidth) - 2; tx = 1.0f; }
    if (y1 > static_cast<i32>(world->heightGridHeight) - 2) { y1 = static_cast<i32>(world->heightGridHeight) - 2; ty = 1.0f; }
    u32 w = world->heightGridWidth;
    float top    = world->heightGrid[static_cast<u32>(y1) * w + static_cast<u32>(x1)]
                 + (world->heightGrid[static_cast<u32>(y1) * w + static_cast<u32>(x1 + 1)] - world->heightGrid[static_cast<u32>(y1) * w + static_cast<u32>(x1)]) * tx;
    float bottom = world->heightGrid[static_cast<u32>(y1 + 1) * w + static_cast<u32>(x1)]
                 + (world->heightGrid[static_cast<u32>(y1 + 1) * w + static_cast<u32>(x1 + 1)] - world->heightGrid[static_cast<u32>(y1 + 1) * w + static_cast<u32>(x1)]) * tx;
    return top + (bottom - top) * ty;
}

// ── Resample a river's centerline to uniform spacing (world XZ) ────────
// out must hold (totalLen / SPACING_M + 1) samples.  Returns the count.
static u32 resampleRiver(const AzgaarWorld* world, const AzgaarRiver* r,
                           RiverSample* out, u32 cap) {
    u32 n = r->pointCount;
    if (r->pointsPx.empty() || n < 2u) return 0;

    // Convert map px → world XZ
    std::vector<float> wxz(n * 2);
    for (u32 i = 0; i < n; ++i) {
        azgaarMapToWorld(world, r->pointsPx[i * 2], r->pointsPx[i * 2 + 1],
                           &wxz[i * 2], &wxz[i * 2 + 1]);
    }

    // Cumulative arc length (XZ, metres)
    float totalLen = 0.0f;
    std::vector<float> cum(n);
    cum[0] = 0.0f;
    for (u32 i = 0; i + 1u < n; ++i) {
        float dx = wxz[(i + 1u) * 2] - wxz[i * 2];
        float dz = wxz[(i + 1u) * 2 + 1] - wxz[i * 2 + 1];
        totalLen += sqrtf(dx * dx + dz * dz);
        cum[i + 1u] = totalLen;
    }

    // Width profile: w(s) = lerp(sourceWidthPx, widthPx, s / lengthM) * mpp,
    // clamped to [MIN_W_M, MAX_W_M].  Missing metadata → default 4 m width.
    float lengthM = r->lengthKm * 1000.0f;

    u32 count = static_cast<u32>(totalLen / AZGAAR_RIVER_SPACING_M);
    if (count + 1u > cap) count = cap - 1u;
    if (count < 1u) count = 1u;

    engine::HeightmapTerrain* ht = engine::heightmapTerrainGetActive();
    float seaLevel = azgaarSeaLevelMeters(world);
    double mppD = world->metersPerPixel;

    // `target = k * SPACING_M` increases monotonically, so the containing
    // segment index only moves forward.  A single running pointer keeps this
    // O(total samples) instead of O(samples * points) per river.
    u32 seg = 0;
    for (u32 k = 0; k <= count; ++k) {
        float target = static_cast<float>(k) * AZGAAR_RIVER_SPACING_M;
        while (seg + 1u < n && cum[seg + 1u] < target) seg++;
        u32 i = seg;

        float wx, wz, tx, tz, hPxX, hPxY;
        if (i >= n - 1u) {
            // The final sample lands at (or beyond) the mouth: snap to the
            // last centerline point.  (Prevents an out-of-bounds index into
            // wxz/cum when target reaches totalLen.)
            i = n - 1u;
            wx = wxz[i * 2];
            wz = wxz[i * 2 + 1];
            float pdx = wxz[i * 2] - wxz[(i - 1u) * 2];
            float pdz = wxz[i * 2 + 1] - wxz[(i - 1u) * 2 + 1];
            float pdl = sqrtf(pdx * pdx + pdz * pdz);
            tx = pdl > 1e-6f ? pdx / pdl : 1.0f;
            tz = pdl > 1e-6f ? pdz / pdl : 0.0f;
            hPxX = r->pointsPx[i * 2];
            hPxY = r->pointsPx[i * 2 + 1];
        } else {
            float segLen = cum[i + 1u] - cum[i];
            float t = segLen > 1e-6f ? (target - cum[i]) / segLen : 0.0f;
            wx = wxz[i * 2] + (wxz[(i + 1u) * 2] - wxz[i * 2]) * t;
            wz = wxz[i * 2 + 1] + (wxz[(i + 1u) * 2 + 1] - wxz[i * 2 + 1]) * t;
            float dx = wxz[(i + 1u) * 2] - wxz[i * 2];
            float dz = wxz[(i + 1u) * 2 + 1] - wxz[i * 2];
            float dl = sqrtf(dx * dx + dz * dz);
            tx = dl > 1e-6f ? dx / dl : 1.0f;
            tz = dl > 1e-6f ? dz / dl : 0.0f;
            hPxX = r->pointsPx[i * 2] + (r->pointsPx[(i + 1u) * 2] - r->pointsPx[i * 2]) * t;
            hPxY = r->pointsPx[i * 2 + 1] + (r->pointsPx[(i + 1u) * 2 + 1] - r->pointsPx[i * 2 + 1]) * t;
        }

        // Lift: terrain height (or sea level at mouths) + 3 cm guard.  Uses the
        // fast bilinear height-grid read as the fallback.
        float naturalH = ht ? engine::heightmapTerrainSample(ht, wx, wz)
                             : azgaarHeightToMeters(world, azgaarDirectHeight(world, hPxX, hPxY));
        float y = fmaxf(naturalH, seaLevel) + AZGAAR_RIVER_LIFT_M;

        // Width profile at fraction s/lengthM
        float frac = lengthM > 1e-6f ? target / lengthM : 0.0f;
        float wPx = r->sourceWidthPx + (r->widthPx - r->sourceWidthPx) * frac;
        float widthM = fminf(fmaxf(wPx * static_cast<float>(mppD), AZGAAR_RIVER_MIN_W_M), AZGAAR_RIVER_MAX_W_M);

        out[k].wx = wx; out[k].wz = wz; out[k].wy = y;
        out[k].tx = tx; out[k].tz = tz;
        out[k].arcLen = target;
        out[k].widthM = widthM;
    }

    return count + 1u;
}

// ── Spatial hash build (10 m buckets, sparse) ──────────────────────────
// Compute each point's bucket key once, dedup into g_bucketKeys, count per
// distinct bucket, prefix-sum for g_bucketStart, and scatter g_hashPts into
// bucket-sorted order.  An open-addressing map (g_bucketMap) maps key →
// compact bucket index for O(1) query lookups.  No dense numBX*numBY grid.
static void buildHash(void) {
    if (g_hashPts.empty() || g_hashCount == 0) { g_hashReady = false; return; }
    size_t n = g_hashCount;

    // Reset any prior sparse structures.
    freeHashStructs();

    // Open-addressing map sized to a power of two >= 2n (keeps load factor low).
    size_t mcap = 4096u;
    while (mcap < n * 2u) mcap *= 2u;
    g_mapCap = mcap;
    g_bucketMap.resize(mcap);
    g_mapUsed.assign(mcap, 0u);
    g_bucketMapIdx.resize(mcap);

    // Per-point bucket keys.
    std::vector<u64> keysTmp(n);
    for (size_t i = 0; i < n; ++i) {
        int bx = (int)floorf(g_hashPts[i].wx / AZGAAR_RIVER_BUCKET_M);
        int by = (int)floorf(g_hashPts[i].wz / AZGAAR_RIVER_BUCKET_M);
        keysTmp[i] = packBucket(by, bx);
    }

    // Dedup keys into g_bucketKeys (one per distinct bucket) + count points.
    g_bucketKeys.resize(n);
    std::vector<u32> counts(n + 1, 0u);
    for (size_t i = 0; i < n; ++i) {
        u64 key = keysTmp[i];
        u32 ci = bucketLookup(key);
        if (ci == 0xFFFFFFFFu) {
            ci = g_bucketCount++;
            g_bucketKeys[ci] = key;
            bucketInsert(key, ci);
        }
        counts[ci]++;
    }

    // Prefix sum over the compact distinct buckets -> g_bucketStart.
    g_bucketStart.resize(static_cast<size_t>(g_bucketCount) + 1);
    u32 running = 0;
    for (u32 b = 0; b < g_bucketCount; ++b) {
        g_bucketStart[b] = running;
        running += counts[b];
    }
    g_bucketStart[g_bucketCount] = running; // == n

    // Scatter g_hashPts into bucket-sorted order (compact bucket index order).
    std::vector<u32> cursor(g_bucketStart.begin(), g_bucketStart.end() - 1);
    std::vector<RiverHashPoint> sorted(n);
    for (size_t i = 0; i < n; ++i) {
        u32 ci = bucketLookup(keysTmp[i]);
        sorted[cursor[ci]++] = g_hashPts[i];
    }
    g_hashPts = std::move(sorted);

    g_hashReady = true;
}

// ── Public: query the river-point hash ──────────────────────────────────
u32 azgaarRiversNear(float wx, float wz, float radius,
                       AzgaarRiverNearHit* out, u32 maxPts) {
    if (!g_hashReady || g_hashPts.empty()) return 0;
    int qbx = (int)floorf(wx / AZGAAR_RIVER_BUCKET_M);
    int qby = (int)floorf(wz / AZGAAR_RIVER_BUCKET_M);
    u32 written = 0;
    float r2 = radius * radius;
    for (int oy = -1; oy <= 1; ++oy) {
        for (int ox = -1; ox <= 1; ++ox) {
            u32 ci = bucketLookup(packBucket(qby + oy, qbx + ox));
            if (ci == 0xFFFFFFFFu) continue;
            u32 start = g_bucketStart[ci];
            u32 end   = g_bucketStart[ci + 1];
            for (u32 i = start; i < end && written < maxPts; ++i) {
                RiverHashPoint* p = &g_hashPts[i];
                float dx = p->wx - wx;
                float dz = p->wz - wz;
                if (dx * dx + dz * dz <= r2) {
                    out[written].wx = p->wx;
                    out[written].wz = p->wz;
                    out[written].riverId = p->riverId;
                    out[written].widthM = p->widthM;
                    written++;
                }
            }
        }
    }
    return written;
}

// ── Build entry point ────────────────────────────────────────────────────
void azgaarRiversInit(const AzgaarWorld* world) {
    const char* env = getenv("ENGINE_AZGAAR_RIVERS_DISABLED");
    if (env && atoi(env)) {
        utils::info("Azgaar rivers: disabled via ENGINE_AZGAAR_RIVERS_DISABLED");
        return;
    }
    if (world->rivers.empty() || world->riverCount == 0u) {
        utils::info("Azgaar rivers: no river geometry, skipping");
        return;
    }

    azgaarRiversClear();

    double t0 = utils::millies();

    // Discharge normalisation for the per-river flow factor (0..1).
    float maxDischarge = 0.0f;
    for (u32 i = 0; i < world->riverCount; ++i)
        if (world->rivers[i].discharge > maxDischarge)
            maxDischarge = world->rivers[i].discharge;

    // Single pass: compute each river's resampled sample count and pre-size
    // the growable buffers (avoids recomputing map-to-world three times).
    std::vector<u32> sampleCounts(world->riverCount);
    u32 totalVerts = 0;
    u32 totalSamples = 0;
    size_t idxNeed = 0;
    for (u32 i = 0; i < world->riverCount; ++i) {
        const AzgaarRiver* r = &world->rivers[i];
        sampleCounts[i] = 0;
        if (r->pointCount < 2u) continue;
        std::vector<float> wxz(r->pointCount * 2);
        for (u32 p = 0; p < r->pointCount; ++p)
            azgaarMapToWorld(world, r->pointsPx[p * 2], r->pointsPx[p * 2 + 1],
                              &wxz[p * 2], &wxz[p * 2 + 1]);
        float totalLen = 0.0f;
        for (u32 p = 0; p + 1u < r->pointCount; ++p) {
            float dx = wxz[(p + 1u) * 2] - wxz[p * 2];
            float dz = wxz[(p + 1u) * 2 + 1] - wxz[p * 2 + 1];
            totalLen += sqrtf(dx * dx + dz * dz);
        }
        u32 samples = static_cast<u32>(totalLen / AZGAAR_RIVER_SPACING_M) + 1u;
        sampleCounts[i] = samples;
        totalVerts += samples * 2u;
        totalSamples += samples;
        idxNeed += static_cast<size_t>(samples - 1u) * 6u;
    }

    growVerts(static_cast<size_t>(totalVerts));
    growIndices(idxNeed);
    growHash(static_cast<size_t>(totalSamples));

    u32 riverWithGeom = 0;
    for (u32 i = 0; i < world->riverCount; ++i) {
        const AzgaarRiver* r = &world->rivers[i];
        u32 samples = sampleCounts[i];
        if (r->pointCount < 2u || samples < 2u) continue;
        riverWithGeom++;

        float flowFactor = maxDischarge > 0.0f ? r->discharge / maxDischarge : 0.0f;

        std::vector<RiverSample> smp(samples);
        u32 n = resampleRiver(world, r, smp.data(), samples);
        if (n < 2u) continue;

        // Ribbon vertices: 2 per sample (left/right edges).
        size_t vBase = g_vertCount;
        for (u32 k = 0; k < n; ++k) {
            RiverSample* s = &smp[k];
            // Perpendicular to flow dir in XZ (rotate +90°)
            float px = -s->tz;
            float pz = s->tx;
            float halfW = s->widthM * 0.5f;
            size_t li = g_vertCount;
            // Buffers were pre-sized to the exact total (growVerts(totalVerts)),
            // so no per-vertex realloc is needed here.
            // left
            engine::SceneVertex* L = &g_verts[li];
            L->position[0] = s->wx - px * halfW;
            L->position[1] = s->wy;
            L->position[2] = s->wz - pz * halfW;
            L->normal[0] = 0.0f; L->normal[1] = 1.0f; L->normal[2] = 0.0f;
            L->tangent[0] = s->tx; L->tangent[1] = 0.0f; L->tangent[2] = s->tz; L->tangent[3] = s->widthM;
            L->uv[0] = s->arcLen; L->uv[1] = flowFactor;
            // right
            engine::SceneVertex* R = &g_verts[li + 1u];
            R->position[0] = s->wx + px * halfW;
            R->position[1] = s->wy;
            R->position[2] = s->wz + pz * halfW;
            R->normal[0] = 0.0f; R->normal[1] = 1.0f; R->normal[2] = 0.0f;
            R->tangent[0] = s->tx; R->tangent[1] = 0.0f; R->tangent[2] = s->tz; R->tangent[3] = s->widthM;
            R->uv[0] = s->arcLen; R->uv[1] = flowFactor;
            g_vertCount += 2u;

            // Hash point (one per centerline sample).  Buffer pre-sized via
            // growHash(totalSamples) — no per-sample realloc.
            RiverHashPoint* hp = &g_hashPts[g_hashCount++];
            hp->wx = s->wx; hp->wz = s->wz; hp->riverId = r->id; hp->widthM = s->widthM;
        }

        // Ribbon indices: quads between consecutive samples.  Buffer pre-sized
        // via growIndices(idxNeed) — no per-quad realloc.
        for (u32 k = 0; k + 1u < n; ++k) {
            u32 li = vBase + static_cast<size_t>(k) * 2u;
            u32 li1 = vBase + static_cast<size_t>(k + 1u) * 2u;
            g_indices[g_idxCount++] = li;      // L_k
            g_indices[g_idxCount++] = li + 1u; // R_k
            g_indices[g_idxCount++] = li1;     // L_{k+1}
            g_indices[g_idxCount++] = li + 1u; // R_k
            g_indices[g_idxCount++] = li1 + 1u;// R_{k+1}
            g_indices[g_idxCount++] = li1;     // L_{k+1}
        }

        // Wet-strip ground decals along the centerline (coarse spacing).
        float stripSpacing = AZGAAR_RIVER_STRIP_SPACING_M;
        u32 lastDecalS = 0;
        engine::HeightmapTerrain* ht2 = engine::heightmapTerrainGetActive();
        for (u32 k = 0; k < n && g_decalCount < AZGAAR_RIVER_DECAL_MAX; ++k) {
            RiverSample* s = &smp[k];
            if (s->arcLen - lastDecalS < stripSpacing) continue;
            lastDecalS = s->arcLen;
            float stripW = s->widthM * 1.6f;
            // Sample ground height for the decal's natural height.
            float naturalH = ht2 ? engine::heightmapTerrainSample(ht2, s->wx, s->wz) : 0.0f;
            // yaw from flow direction
            float yaw = atan2f(s->tx, s->tz);
            engine::DecalInstance d = {};
            d.position[0] = s->wx;
            d.position[1] = naturalH + 40.0f;
            d.position[2] = s->wz;
            d.halfExtents[0] = stripW * 0.5f;
            d.halfExtents[1] = 140.0f;
            d.halfExtents[2] = stripSpacing * 0.5f;
            glm_quatv(d.rotation, yaw, Y_UP);
            vec4 riverColor = {0.20f, 0.16f, 0.10f, 0.35f};
            glm_vec4_copy(riverColor, d.color);
            d.textureId = DECAL_PROCEDURAL_CIRCLE_TEXTURE;
            d.flags = engine::DECAL_FLAG_GROUND_ONLY;
            d.opacity = 1.0f;
            d.normalThreshold = 0.28f;
            d.edgeFeather = 0.34f;
            d.uvScale[0] = 1.0f;
            d.uvScale[1] = fmaxf(1.0f, stripSpacing / stripW);
              u32 handle = engine::decalAdd(&d);
              if (handle != DECAL_INVALID_HANDLE) g_decals[g_decalCount++] = handle;
        }

    }

    // Build the spatial hash from the collected centerline points.
    buildHash();

    // Upload the ribbon mesh to the azgaar_river pass.
    if (g_vertCount && g_idxCount) {
        vulkanAzgaarRiverSetMesh(g_verts.data(), static_cast<u32>(g_vertCount), g_indices.data(), static_cast<u32>(g_idxCount));
    }

    utils::info("Azgaar rivers: %u rivers (%u with geometry), %u ribbon verts, %u indices, "
          "%u hash points, %u wet decals, built in %.1f ms",
          world->riverCount, riverWithGeom, static_cast<u32>(g_vertCount), static_cast<u32>(g_idxCount),
          static_cast<u32>(g_hashCount), g_decalCount, utils::millies() - t0);
}
}  // namespace game
