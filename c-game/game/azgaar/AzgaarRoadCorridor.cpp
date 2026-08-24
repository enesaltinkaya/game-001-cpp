#include "azgaar/AzgaarRoadCorridor.h"
#include <math.h>
#include <float.h>

// ── Tunables (see plans/azgaar-road-walkable-terrain.md) ────────────────────
// Corridor half-width must cover the painted road decal. The road decal width
// is 34 m (AzgaarRoadDecals.c::routeWidth), so 34/2 + a margin.
#define AZGAAR_ROAD_CORRIDOR_HALF_WIDTH_ROAD  19.0f
#define AZGAAR_ROAD_CORRIDOR_HALF_WIDTH_TRAIL 12.0f
// Width of the soft blend band from the road edge back to natural terrain.
#define AZGAAR_ROAD_CORRIDOR_BLEND_WIDTH        6.0f
// Maximum road grade (rise/run). Deliberately below the 45° character
// controller limit (Player.c) to leave margin for the smootherstep peak inside
// the corridor and the edge-blend ramp back to natural terrain.
#define AZGAAR_ROAD_MAX_GRADE_DEG              34.0f

// ── Geometry ────────────────────────────────────────────────────────────────
namespace game {
struct RoadSeg {
    float ax, az;     // world-space endpoint A (XZ)
    float bx, bz;     // world-space endpoint B (XZ)
    float ha;         // grade-limited height at A (meters)
    float hb;         // grade-limited height at B (meters)
    float halfWidth;  // corridor half-width for this segment (meters)
};

struct RoadCorridorGrid {
    float originX, originZ;
    float size;
    float invSize;
    u32   cols, rows;
    std::vector<u32> bucketStart;  // length cols*rows + 1 (CSR-style offsets)
    std::vector<u32> bucketSegs;   // segment indices, grouped by bucket
};

static std::vector<RoadSeg> g_segs;
static RoadCorridorGrid g_grid;
static bool g_built;

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Local smootherstep (the terrain mesher was removed in the heightmap
// cutover; this copy is the only one left).
static float smootherstepf(float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static float controlHeightMeters(const AzgaarWorld* world, const AzgaarRoutePoint* pt) {
    // The route point's cell is authoritative; fall back to the nearest cell
    // when the index is out of range (Azgaar occasionally emits 0/placeholder).
    float raw;
    if (pt->cell < world->cellCount) {
        raw = world->cells[pt->cell].height;
    } else {
        raw = azgaarWorldSampleHeightNearest(world, pt->x, pt->y);
    }
    return azgaarHeightToMeters(world, raw);
}

// Forward grade-limit pass: clamps each control height so the slope to the
// previous control point stays within budget. Keeps each H[i] as close to its
// natural height as the slope budget allows. Once the sequence is feasible a
// backward pass is a no-op (each H[i] already satisfies its right-neighbour
// constraint), so a single forward sweep is complete.
static void gradeLimitRoute(const AzgaarWorld* world,
                            const AzgaarRoute* route,
                            float* H,
                            float maxGrade) {
    for (u32 i = 1u; i < route->pointCount; ++i) {
        float pax, paz, pbx, pbz;
        azgaarMapToWorld(world, route->points[i - 1u].x, route->points[i - 1u].y, &pax, &paz);
        azgaarMapToWorld(world, route->points[i].x, route->points[i].y, &pbx, &pbz);
        float dx   = pbx - pax;
        float dz   = pbz - paz;
        float dist = sqrtf(dx * dx + dz * dz);
        float maxDelta = maxGrade * dist;
        H[i] = clampf(H[i], H[i - 1u] - maxDelta, H[i - 1u] + maxDelta);
    }
}

// Min/max inflated bucket range (inclusive) a segment's reachable AABB covers.
static void segBucketRange(const RoadCorridorGrid* g,
                           const RoadSeg* s,
                           float reach,
                           i32* outBx0, i32* outBx1,
                           i32* outBz0, i32* outBz1) {
    float sx0 = (fminf(s->ax, s->bx) - reach - g->originX) * g->invSize;
    float sx1 = (fmaxf(s->ax, s->bx) + reach - g->originX) * g->invSize;
    float sz0 = (fminf(s->az, s->bz) - reach - g->originZ) * g->invSize;
    float sz1 = (fmaxf(s->az, s->bz) + reach - g->originZ) * g->invSize;

    i32 bx0 = static_cast<i32>(floorf(sx0));
    i32 bx1 = static_cast<i32>(floorf(sx1));
    i32 bz0 = static_cast<i32>(floorf(sz0));
    i32 bz1 = static_cast<i32>(floorf(sz1));
    if (bx0 < 0) bx0 = 0;
    if (bz0 < 0) bz0 = 0;
    if (bx1 >= static_cast<i32>(g->cols)) bx1 = static_cast<i32>(g->cols) - 1;
    if (bz1 >= static_cast<i32>(g->rows)) bz1 = static_cast<i32>(g->rows) - 1;

    *outBx0 = bx0; *outBx1 = bx1;
    *outBz0 = bz0; *outBz1 = bz1;
}

static void buildGrid(void) {
    const float reach = AZGAAR_ROAD_CORRIDOR_HALF_WIDTH_ROAD + AZGAAR_ROAD_CORRIDOR_BLEND_WIDTH;

    float minX = FLT_MAX, minZ = FLT_MAX, maxX = -FLT_MAX, maxZ = -FLT_MAX;
    for (u32 i = 0; i < g_segs.size(); ++i) {
        const RoadSeg* s = &g_segs[i];
        minX = fminf(minX, fminf(s->ax, s->bx));
        maxX = fmaxf(maxX, fmaxf(s->ax, s->bx));
        minZ = fminf(minZ, fminf(s->az, s->bz));
        maxZ = fmaxf(maxZ, fmaxf(s->az, s->bz));
    }
    minX -= reach; minZ -= reach; maxX += reach; maxZ += reach;

    g_grid.originX = minX;
    g_grid.originZ = minZ;
    g_grid.size    = fmaxf(reach * 2.0f, 1.0f);  // bucket ~= corridor diameter
    g_grid.invSize = 1.0f / g_grid.size;
    g_grid.cols    = static_cast<u32>(ceilf((maxX - minX) * g_grid.invSize));
    g_grid.rows    = static_cast<u32>(ceilf((maxZ - minZ) * g_grid.invSize));
    if (g_grid.cols == 0u) g_grid.cols = 1u;
    if (g_grid.rows == 0u) g_grid.rows = 1u;
    u32 bucketCount = g_grid.cols * g_grid.rows;

    std::vector<u32> counts(bucketCount, 0u);

    // Insert each segment into every bucket its reachable AABB overlaps, so a
    // query only needs to scan the single bucket containing the query point.
    for (u32 i = 0; i < g_segs.size(); ++i) {
        i32 bx0, bx1, bz0, bz1;
        segBucketRange(&g_grid, &g_segs[i], reach, &bx0, &bx1, &bz0, &bz1);
        for (i32 z = bz0; z <= bz1; ++z)
            for (i32 x = bx0; x <= bx1; ++x)
                ++counts[static_cast<u32>(z) * g_grid.cols + static_cast<u32>(x)];
    }

    g_grid.bucketStart.resize(bucketCount + 1u);
    u32 sum = 0u;
    for (u32 i = 0u; i < bucketCount; ++i) {
        g_grid.bucketStart[i] = sum;
        sum += counts[i];
    }
    g_grid.bucketStart[bucketCount] = sum;
    g_grid.bucketSegs.resize(sum);

    for (u32 i = 0u; i < bucketCount; ++i) counts[i] = 0u;
    for (u32 i = 0; i < g_segs.size(); ++i) {
        i32 bx0, bx1, bz0, bz1;
        segBucketRange(&g_grid, &g_segs[i], reach, &bx0, &bx1, &bz0, &bz1);
        for (i32 z = bz0; z <= bz1; ++z)
            for (i32 x = bx0; x <= bx1; ++x) {
                u32 b = static_cast<u32>(z) * g_grid.cols + static_cast<u32>(x);
                g_grid.bucketSegs[g_grid.bucketStart[b] + counts[b]++] = i;
            }
    }
}

void azgaarRoadCorridorBuild(const AzgaarWorld* world) {
    azgaarRoadCorridorClear();
    if (!world || world->routes.empty() || world->routeCount == 0u) return;

    const float maxGrade = tanf(glm_rad(AZGAAR_ROAD_MAX_GRADE_DEG));

    for (u32 r = 0u; r < world->routeCount; ++r) {
        const AzgaarRoute* route = &world->routes[r];
        if (route->group != AZGAAR_ROUTE_ROAD) continue;   // Phase 1: roads only
        if (route->pointCount < 2u) continue;

        u32 n = route->pointCount;
        std::vector<float> H(n);
        for (u32 i = 0u; i < n; ++i) H[i] = controlHeightMeters(world, &route->points[i]);
        gradeLimitRoute(world, route, H.data(), maxGrade);

        const float hw = AZGAAR_ROAD_CORRIDOR_HALF_WIDTH_ROAD;
        for (u32 i = 0u; i + 1u < n; ++i) {
            float ax, az, bx, bz;
            azgaarMapToWorld(world, route->points[i].x, route->points[i].y, &ax, &az);
            azgaarMapToWorld(world, route->points[i + 1u].x, route->points[i + 1u].y, &bx, &bz);
            RoadSeg s = {
                .ax = ax, .az = az, .bx = bx, .bz = bz,
                .ha = H[i], .hb = H[i + 1u], .halfWidth = hw,
            };
            g_segs.push_back(s);
        }
    }

    if (static_cast<i32>(g_segs.size()) == 0u) {
        g_built = true;
        return;
    }

    buildGrid();
    g_built = true;
    utils::info("azgaarRoadCorridor: built %u road segments, max grade %.0f deg, grid %ux%u",
         static_cast<u32>(static_cast<i32>(g_segs.size())), AZGAAR_ROAD_MAX_GRADE_DEG, g_grid.cols, g_grid.rows);
}

void azgaarRoadCorridorClear(void) {
    g_grid  = RoadCorridorGrid{};
    g_built = false;
}

bool azgaarRoadCorridorSample(float worldX, float worldZ, float naturalY, float* outHeight) {
    if (!g_built || !outHeight || static_cast<i32>(g_segs.size()) == 0u) return false;

    i32 bx = static_cast<i32>(floorf((worldX - g_grid.originX) * g_grid.invSize));
    i32 bz = static_cast<i32>(floorf((worldZ - g_grid.originZ) * g_grid.invSize));
    if (bx < 0 || bx >= static_cast<i32>(g_grid.cols) || bz < 0 || bz >= static_cast<i32>(g_grid.rows)) return false;

    u32 b  = static_cast<u32>(bz) * g_grid.cols + static_cast<u32>(bx);
    u32 s0 = g_grid.bucketStart[b];
    u32 s1 = g_grid.bucketStart[b + 1u];

    float bestDist  = FLT_MAX;
    float bestHc    = 0.0f;
    float bestHalf  = 0.0f;
    bool  found     = false;

    for (u32 k = s0; k < s1; ++k) {
        const RoadSeg* s = &g_segs[g_grid.bucketSegs[k]];
        float ex   = s->bx - s->ax;
        float ez   = s->bz - s->az;
        float len2 = ex * ex + ez * ez;
        float t    = 0.0f;
        if (len2 > 1e-8f) {
            t = ((worldX - s->ax) * ex + (worldZ - s->az) * ez) / len2;
            t = clampf(t, 0.0f, 1.0f);
        }
        float cx   = s->ax + ex * t;
        float cz   = s->az + ez * t;
        float ddx  = worldX - cx;
        float ddz  = worldZ - cz;
        float d    = sqrtf(ddx * ddx + ddz * ddz);

        float totalR = s->halfWidth + AZGAAR_ROAD_CORRIDOR_BLEND_WIDTH;
        if (d > totalR) continue;          // outside this segment's corridor
        if (d < bestDist) {
            bestDist = d;
            bestHalf = s->halfWidth;
            bestHc   = s->ha + (s->hb - s->ha) * t;
            found    = true;
        }
    }
    if (!found) return false;

    float y;
    if (bestDist <= bestHalf) {
        y = bestHc;                        // fully on the road surface
    } else {
        float tt = (bestDist - bestHalf) / AZGAAR_ROAD_CORRIDOR_BLEND_WIDTH;
        y = bestHc + (naturalY - bestHc) * smootherstepf(tt);
    }
    *outHeight = y;
    return true;
}
}  // namespace game
