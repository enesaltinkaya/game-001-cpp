#include "azgaar/AzgaarSettlements.h"
#include "azgaar/AzgaarProps.h"
#include "azgaar/AzgaarWorld.h"
#include "renderer/vulkan/pass/azgaar_props/VulkanAzgaarPropsPass.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * AzgaarSettlements (workstream D, plans/azgaar-world-population.md)
 * ---------------------------------------------------------------------
 * 1. azgaarSettlementsInit: builds every settlement's building cluster
 *    (deterministic: seeded by map name + settlement id), groups the
 *    instances by species, and uploads them to the azgaar_props pass'
 *    global whole-map instance buffer.
 * 2. azgaarSettlementsPlateauY: the D8 terrain plateau — blends the natural
 *    height toward the settlement's flatY so towns sit on level ground
 *    (y' = mix(y, flatY, 1 - smoothstep(0.55r, r, d)).
 * 3. azgaarSettlementsNearest: the closest settlement whose footprint
 *    (radiusM + 30 m) contains the query point (for the zone banner).
 *
 * Kill switch: ENGINE_AZGAAR_SETTLE_DISABLED=1 disables the instances and
 * the plateau (heightAt then returns the natural height).
 */

// Per-building target height ranges (metres).  Mirrors the kSpecies table in
// AzgaarProps.c (unit-height placeholder meshes: instance scale == height).
static const float kBuildH[AZGAAR_PROP_COUNT][2] = {
    [AZGAAR_PROP_HUT]    = {4.0f, 6.0f},
    [AZGAAR_PROP_HOUSE]  = {5.0f, 8.0f},
    [AZGAAR_PROP_TOWER]  = {8.0f, 15.0f},
    [AZGAAR_PROP_WALL]   = {4.0f, 6.0f},
    [AZGAAR_PROP_TEMPLE] = {6.0f, 10.0f},
    [AZGAAR_PROP_DOCK]   = {2.0f, 4.0f},
    [AZGAAR_PROP_GATE]   = {8.0f, 12.0f},
};

// ── Deterministic RNG (seeded by map name + settlement id) ─────────────────

static u32 settHash3(u32 a, u32 b, u32 c) {
    u32 h = a * 0x8da6b343u ^ b * 0xc2b2ae35u ^ c * 0x27d4eb2fu;
    h = (h ^ (h >> 15)) * 0x2c1b3c6du;
    h = (h ^ (h >> 12)) * 0x297a2d39u;
    return h ^ (h >> 15);
}

static u32 settMapSeed(const char* name) {
    u32 h = 2166136261u; // FNV-1a (same seed as the props scatter)
    if (name) {
        for (const char* p = name; *p; p++) {
            h ^= (u32)(unsigned char)*p;
            h *= 16777619u;
        }
    }
    return h ? h : 1u;
}

// Stable [0,1) random from (settlement seed, salt).
static float settRand(u32 seed, u32 salt) {
    u32 h = settHash3(seed, salt, 0x9E3779B9u);
    return (float)(h >> 8) / 16777216.0f;
}

static float settSmoothstep01(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

// ── State ──────────────────────────────────────────────────────────────────

static const AzgaarWorld* g_world = NULL;
static bool               g_disabled = false;
static u32                g_mapSeed;

static PropInstance*  g_instances = NULL;
static u32            g_instanceCount = 0;
static PropTileRange* g_ranges = NULL;
static u32            g_rangeCount = 0;

// Plateau spatial grid (D8): 1024 m buckets over the map AABB.
typedef struct SettGrid {
    float  invBucket;
    float  minX, minZ;
    u32    gridW, gridH;
    u32*   starts; // bucket -> start index into cells
    u32*   cells;  // settlement indices sorted by bucket
    u32    count;
} SettGrid;
static SettGrid g_grid = {};

// Road centreline hash (section-37 routes): 64 m buckets, used to align
// building street yaw to the nearest road.
typedef struct SettRoadHash {
    float  invBucket;
    float  minX, minZ;
    u32    gridW, gridH;
    u32*   starts;
    u32*   cells; // point indices sorted by bucket
    float*  pts;  // [x, z] pairs in world space
    u32    pointCount;
} SettRoadHash;
static SettRoadHash g_road = {};

// Nearest road point within `maxD` of (wx, wz); fills outRx/outRz.
static bool settRoadNear(float wx, float wz, float maxD, float* outRx, float* outRz) {
    if (g_road.pointCount == 0) return false;
    i32 bx = (i32)((wx - g_road.minX) * g_road.invBucket);
    i32 bz = (i32)((wz - g_road.minZ) * g_road.invBucket);
    float bestD = maxD * maxD;
    float bx0 = wx, bz0 = wz;
    bool found = false;
    for (i32 oz = -1; oz <= 1; oz++) {
        for (i32 ox = -1; ox <= 1; ox++) {
            i32 nx = bx + ox, nz = bz + oz;
            if (nx < 0 || nz < 0 || nx >= (i32)g_road.gridW || nz >= (i32)g_road.gridH) continue;
            u32 b = (u32)nz * g_road.gridW + (u32)nx;
            u32 lo = g_road.starts[b];
            u32 hi = (b + 1 < g_road.gridW * g_road.gridH) ? g_road.starts[b + 1] : g_road.pointCount;
            for (u32 i = lo; i < hi; i++) {
                float px = g_road.pts[i * 2];
                float pz = g_road.pts[i * 2 + 1];
                float dx = px - wx, dz = pz - wz;
                float d2 = dx * dx + dz * dz;
                if (d2 < bestD) {
                    bestD = d2;
                    bx0 = px;
                    bz0 = pz;
                    found = true;
                }
            }
        }
    }
    if (found && outRx) *outRx = bx0;
    if (found && outRz) *outRz = bz0;
    return found;
}

// ── Cluster generation ─────────────────────────────────────────────────────

// Append one instance to the unsorted buffer (offset `w`, capacity `cap`).
static void settPush(PropInstance* temp, u32 cap, u32* w,
                      float x, float y, float z, float yaw, float scale,
                      u32 species, float colorJit, const AzgaarSettlement* s) {
    if (*w >= cap) return;
    PropInstance inst = {};
    inst.pos[0] = x;
    inst.pos[1] = y;
    inst.pos[2] = z;
    inst.yaw    = yaw;
    inst.scale  = scale;
    inst.color[0] = s->stateColor[0] * colorJit;
    inst.color[1] = s->stateColor[1] * colorJit;
    inst.color[2] = s->stateColor[2] * colorJit;
    inst.phase   = 0.0f; // static structures: no wind sway
    inst.species = species;
    temp[(*w)++] = inst;
}

// From the settlement centre, ray-march up to 1.5r in 24 directions.  If the
// height grid reports water (cell height < 20) in any direction, return that
// direction (radians); otherwise return -1 (caller drops the port, plan D3).
static float settWaterDir(const AzgaarWorld* world, const AzgaarSettlement* s) {
    float invMpp = 1.0f / (float)world->metersPerPixel;
    const float stepM = 8.0f;
    const u32 N = 24;
    float bestA = -1.0f;
    float bestDist = 1e9f;
    for (u32 k = 0; k < N; k++) {
        float a = (float)k / (float)N * 2.0f * (float)M_PI;
        float dist = 0.0f;
        float limit = 1.5f * s->radiusM;
        while (dist < limit) {
            dist += stepM;
            float rx = s->wx + cosf(a) * dist;
            float rz = s->wz + sinf(a) * dist;
            float hx = (float)world->widthPx * 0.5f - rx * invMpp;
            float hy = (float)world->heightPx * 0.5f - rz * invMpp;
            if (azgaarWorldSampleHeightSmooth(world, hx, hy) < 20.0f) {
                if (dist < bestDist) {
                    bestDist = dist;
                    bestA = a;
                }
                break;
            }
        }
    }
    return bestA;
}

// Appends one settlement's buildings to the unsorted `temp` buffer (the
// caller passes the current write offset in `w`; returns the number of
// instances generated).  Placement is deterministic (seed, salt) so
// regeneration after streaming is bit-identical.
//
// Each building's Y comes from `groundAt` (the heightmap source' exact
// heightAt: natural + fBm detail + D8 plateau) so buildings sit flush with
// the rendered terrain.  `flatY` is only the fallback when no callback.
static u32 settGenOne(const AzgaarWorld* world, const AzgaarSettlement* s, u32 seed,
                       PropInstance* temp, u32 cap, u32* w,
                       float (*groundAt)(void* userData, float wx, float wz),
                       void* groundUd) {
    u32 start = *w;

    // Water settlements (centre below sea level): place the regular buildings
    // on stilts 1 m above the waterline; piers still go in the water.
    float seaY = azgaarSeaLevelMeters(world);
    bool  waterS = s->flatY < seaY;

    // Sample the exact ground height the terrain will have at (x, z): natural
    // + detail + plateau (the source' heightAt).  Falls back to the natural
    // centre height when no callback is provided.
    #define SETT_GROUND(x, z) (groundAt ? groundAt(groundUd, x, z) : s->flatY)

    // Regular buildings: jittered golden-angle spiral, radius grows with sqrt
    // (plan's building count formula: clamp(round(2 + 9*sqrt(popK)), 3, 220)).
    u32 n = (u32)(2.0f + 9.0f * sqrtf(s->populationK) + 0.5f);
    if (n < 3u) n = 3u;
    if (n > 220u) n = 220u;
    float pHouse = s->populationK / 15.0f;
    if (pHouse > 1.0f) pHouse = 1.0f;
    for (u32 i = 0; i < n; i++) {
        float r = s->radiusM * sqrtf((float)(i + 1u) / (float)n);
        r *= 0.8f + 0.4f * settRand(seed, 0xA1u + i);
        float theta = i * 2.399963f + settRand(seed, 0xA2u + i) * 0.9f;
        float x = s->wx + cosf(theta) * r;
        float z = s->wz + sinf(theta) * r;
        u32 sp = (settRand(seed, 0xA3u + i) < pHouse) ? AZGAAR_PROP_HOUSE : AZGAAR_PROP_HUT;
        float hMin = kBuildH[sp][0], hMax = kBuildH[sp][1];
        float scale = hMin + (hMax - hMin) * settRand(seed, 0xA4u + i);
        float yaw = settRand(seed, 0xA5u + i) * 2.0f * (float)M_PI;
        float rx, rz;
        if (settRoadNear(x, z, s->radiusM + 30.0f, &rx, &rz)) {
            yaw = atan2f(rz - s->wz, rx - s->wx); // streets point at the road
        }
        float cj = 0.85f + 0.3f * settRand(seed, 0xA6u + i);
        float gy = SETT_GROUND(x, z);
        if (waterS) gy = seaY + 1.0f;
        settPush(temp, cap, w, x, gy, z, yaw, scale, sp, cj, s);
    }

    // Fortifications + landmarks (plan D3 flags).
    if (s->flags & AZGAAR_SETT_FLAG_WALLS) {
        // 12 wall segments on a 0.9 r ring, tangential yaw.
        for (u32 k = 0; k < 12; k++) {
            float a = (float)k / 12.0f * 2.0f * (float)M_PI + 0.05f * settRand(seed, 0xB1u + k);
            float r = s->radiusM * 0.9f;
            float x = s->wx + cosf(a) * r;
            float z = s->wz + sinf(a) * r;
            float scale = kBuildH[AZGAAR_PROP_WALL][0] +
                          (kBuildH[AZGAAR_PROP_WALL][1] - kBuildH[AZGAAR_PROP_WALL][0]) *
                          settRand(seed, 0xB2u + k);
            float cj = 0.85f + 0.3f * settRand(seed, 0xB3u + k);
            float gy = SETT_GROUND(x, z);
            if (waterS) gy = seaY + 1.0f;
            settPush(temp, cap, w, x, gy, z, a + (float)M_PI * 0.5f, scale,
                 AZGAAR_PROP_WALL, cj, s);
        }
    }
    if (s->flags & AZGAAR_SETT_FLAG_CITADEL) {
        // Citadel tower near the centre.
        float tx = s->wx + (settRand(seed, 0xC1u) - 0.5f) * 4.0f;
        float tz = s->wz + (settRand(seed, 0xC2u) - 0.5f) * 4.0f;
        float ts = kBuildH[AZGAAR_PROP_TOWER][0] +
                    (kBuildH[AZGAAR_PROP_TOWER][1] - kBuildH[AZGAAR_PROP_TOWER][0]) *
                    settRand(seed, 0xC3u);
        float cj = 0.85f + 0.3f * settRand(seed, 0xC4u);
        float gy = SETT_GROUND(tx, tz);
        if (waterS) gy = seaY + 1.0f;
        settPush(temp, cap, w, tx, gy, tz,
             settRand(seed, 0xC5u) * 2.0f * (float)M_PI, ts, AZGAAR_PROP_TOWER, cj, s);
        // Its gate, on the wall ring (east side).
        if (s->flags & AZGAAR_SETT_FLAG_WALLS) {
            float gx = s->wx + s->radiusM * 0.9f;
            float gs = kBuildH[AZGAAR_PROP_GATE][0] +
                       (kBuildH[AZGAAR_PROP_GATE][1] - kBuildH[AZGAAR_PROP_GATE][0]) *
                       settRand(seed, 0xC6u);
            float cj2 = 0.85f + 0.3f * settRand(seed, 0xC7u);
            float gy = SETT_GROUND(gx, s->wz);
            if (waterS) gy = seaY + 1.0f;
            settPush(temp, cap, w, gx, gy, s->wz, (float)M_PI * 0.5f, gs,
                 AZGAAR_PROP_GATE, cj2, s);
        }
    }
    if (s->flags & AZGAAR_SETT_FLAG_TEMPLE) {
        float tx = s->wx + s->radiusM * 0.3f;
        float tz = s->wz + s->radiusM * 0.3f;
        float ts = kBuildH[AZGAAR_PROP_TEMPLE][0] +
                    (kBuildH[AZGAAR_PROP_TEMPLE][1] - kBuildH[AZGAAR_PROP_TEMPLE][0]) *
                    settRand(seed, 0xD1u);
        float cj = 0.85f + 0.3f * settRand(seed, 0xD2u);
        float gy = SETT_GROUND(tx, tz);
        if (waterS) gy = seaY + 1.0f;
        settPush(temp, cap, w, tx, gy, tz,
             settRand(seed, 0xD3u) * 2.0f * (float)M_PI, ts, AZGAAR_PROP_TEMPLE, cj, s);
    }
    if (s->flags & AZGAAR_SETT_FLAG_PORT) {
        // Plan D3: search the height grid for the nearest water within 1.5r;
        // if found, place 2-3 piers (dock instances) toward it, otherwise
        // drop the port (log once).
        float dir = settWaterDir(world, s);
        if (dir < 0.0f) {
            static bool portLogged = false;
            if (!portLogged) {
                portLogged = true;
                warn("azgaarSettlements: '%s' has a port but no water within 1.5r — piers omitted",
                     s->name);
            }
        } else {
            const float fracs[3] = {0.7f, 0.9f, 1.1f};
            u32 nPiers = (s->populationK >= 10.0f) ? 3u : 2u;
            for (u32 k = 0; k < nPiers; k++) {
                float r = s->radiusM * fracs[k];
                float x = s->wx + cosf(dir) * r;
                float z = s->wz + sinf(dir) * r;
                float ds = kBuildH[AZGAAR_PROP_DOCK][0] +
                           (kBuildH[AZGAAR_PROP_DOCK][1] - kBuildH[AZGAAR_PROP_DOCK][0]) *
                           settRand(seed, 0xE1u + k);
                float cj = 0.85f + 0.3f * settRand(seed, 0xE2u + k);
                // Piers sit in the water: base at the natural centre height.
                settPush(temp, cap, w, x, s->flatY, z, dir + (float)M_PI * 0.5f, ds,
                     AZGAAR_PROP_DOCK, cj, s);
            }
        }
    }
    if (s->flags & AZGAAR_SETT_FLAG_SHANTY) {
        // Poorer outskirts: a few small huts at the edge.
        for (u32 k = 0; k < 4; k++) {
            float a = (float)k / 4.0f * 2.0f * (float)M_PI + settRand(seed, 0xF1u + k) * 0.6f;
            float r = s->radiusM * (0.88f + 0.1f * settRand(seed, 0xF2u + k));
            float x = s->wx + cosf(a) * r;
            float z = s->wz + sinf(a) * r;
            float scale = 4.0f + 1.0f * settRand(seed, 0xF3u + k);
            float cj = 0.85f + 0.3f * settRand(seed, 0xF4u + k);
            float gy = SETT_GROUND(x, z);
            if (waterS) gy = seaY + 1.0f;
            settPush(temp, cap, w, x, gy, z, a, scale, AZGAAR_PROP_HUT, cj, s);
        }
    }
    return *w - start;
}

// ── Grid + road hash builders ─────────────────────────────────────────────

// Builds the plateau grid into `out` (caller zeroed).  `out->count` is set
// LAST so a concurrent reader (the heightmap build thread, which calls
// azgaarHeightmapHeightAt -> azgaarSettlementsPlateauY while tiles are still
// streaming) only ever sees either count==0 (returns the natural height) or
// a fully built grid.  Publishing `count` last makes the publication safe.
static void settGridBuild(const AzgaarWorld* world, SettGrid* out) {
    *out = (SettGrid){0};
    if (!world || world->settlementCount == 0) return;

    const float BUCKET = 1024.0f;
    float halfW = (float)(world->widthPx * 0.5) * (float)world->metersPerPixel + 40.0f;
    float halfH = (float)(world->heightPx * 0.5) * (float)world->metersPerPixel + 40.0f;
    out->invBucket = 1.0f / BUCKET;
    out->minX      = -halfW;
    out->minZ      = -halfH;
    out->gridW     = (u32)(2.0f * halfW / BUCKET) + 1u;
    out->gridH     = (u32)(2.0f * halfH / BUCKET) + 1u;
    u32 buckets = out->gridW * out->gridH;
    out->starts  = static_cast<u32*>(memoryAlloc(sizeof(u32) * (buckets + 1u)));
    out->cells   = static_cast<u32*>(memoryAlloc(sizeof(u32) * world->settlementCount));

    u32* counts = static_cast<u32*>(memoryAlloc(sizeof(u32) * buckets));
    memset(counts, 0, sizeof(u32) * buckets);
    for (u32 i = 0; i < world->settlementCount; i++) {
        const AzgaarSettlement* s = &world->settlements[i];
        i32 bx = (i32)((s->wx - out->minX) * out->invBucket);
        i32 bz = (i32)((s->wz - out->minZ) * out->invBucket);
        if (bx < 0) bx = 0;
        if (bz < 0) bz = 0;
        if (bx >= (i32)out->gridW) bx = (i32)out->gridW - 1;
        if (bz >= (i32)out->gridH) bz = (i32)out->gridH - 1;
        counts[(u32)bz * out->gridW + (u32)bx]++;
    }
    u32 acc = 0;
    for (u32 b = 0; b < buckets; b++) {
        out->starts[b] = acc;
        acc += counts[b];
    }
    out->starts[buckets] = acc;
    u32* cursor = static_cast<u32*>(memoryAlloc(sizeof(u32) * buckets));
    memcpy(cursor, out->starts, sizeof(u32) * buckets);
    for (u32 i = 0; i < world->settlementCount; i++) {
        const AzgaarSettlement* s = &world->settlements[i];
        i32 bx = (i32)((s->wx - out->minX) * out->invBucket);
        i32 bz = (i32)((s->wz - out->minZ) * out->invBucket);
        if (bx < 0) bx = 0;
        if (bz < 0) bz = 0;
        if (bx >= (i32)out->gridW) bx = (i32)out->gridW - 1;
        if (bz >= (i32)out->gridH) bz = (i32)out->gridH - 1;
        u32 b = (u32)bz * out->gridW + (u32)bx;
        out->cells[cursor[b]++] = i;
    }
    memoryFree(counts);
    memoryFree(cursor);
    // Publish last: concurrent readers only see count==0 or the finished grid.
    out->count = world->settlementCount;
}

static void settRoadBuild(const AzgaarWorld* world) {
    g_road = (SettRoadHash){0};
    if (!world || world->routeCount == 0) return;

    const float BUCKET = 64.0f;
    u32 totalPoints = 0;
    for (u32 r = 0; r < world->routeCount; r++) {
        AzgaarRoute* route = &world->routes[r];
        if (route->group == AZGAAR_ROUTE_SEAROUTE) continue;
        totalPoints += route->pointCount;
    }
    if (totalPoints == 0) return;
    float halfW = (float)(world->widthPx * 0.5) * (float)world->metersPerPixel + 40.0f;
    float halfH = (float)(world->heightPx * 0.5) * (float)world->metersPerPixel + 40.0f;
    g_road.invBucket = 1.0f / BUCKET;
    g_road.minX      = -halfW;
    g_road.minZ      = -halfH;
    g_road.gridW     = (u32)(2.0f * halfW / BUCKET) + 1u;
    g_road.gridH     = (u32)(2.0f * halfH / BUCKET) + 1u;
    u32 buckets       = g_road.gridW * g_road.gridH;
    g_road.pointCount = totalPoints;
    g_road.pts         = static_cast<float*>(memoryAlloc(sizeof(float) * 2 * totalPoints));
    g_road.starts      = static_cast<u32*>(memoryAlloc(sizeof(u32) * (buckets + 1u)));
    g_road.cells       = static_cast<u32*>(memoryAlloc(sizeof(u32) * totalPoints));

    u32* counts = static_cast<u32*>(memoryAlloc(sizeof(u32) * buckets));
    memset(counts, 0, sizeof(u32) * buckets);
    u32 write = 0;
    for (u32 r = 0; r < world->routeCount; r++) {
        AzgaarRoute* route = &world->routes[r];
        if (route->group == AZGAAR_ROUTE_SEAROUTE) continue;
        for (u32 p = 0; p < route->pointCount; p++) {
            float wx, wz;
            azgaarMapToWorld(world, route->points[p].x, route->points[p].y, &wx, &wz);
            i32 bx = (i32)((wx - g_road.minX) * g_road.invBucket);
            i32 bz = (i32)((wz - g_road.minZ) * g_road.invBucket);
            if (bx < 0) bx = 0;
            if (bz < 0) bz = 0;
            if (bx >= (i32)g_road.gridW) bx = (i32)g_road.gridW - 1;
            if (bz >= (i32)g_road.gridH) bz = (i32)g_road.gridH - 1;
            u32 b = (u32)bz * g_road.gridW + (u32)bx;
            g_road.pts[write * 2]     = wx;
            g_road.pts[write * 2 + 1] = wz;
            counts[b]++;
            write++;
        }
    }
    u32 acc = 0;
    for (u32 b = 0; b < buckets; b++) {
        g_road.starts[b] = acc;
        acc += counts[b];
    }
    g_road.starts[buckets] = acc;
    u32* cursor = static_cast<u32*>(memoryAlloc(sizeof(u32) * buckets));
    memcpy(cursor, g_road.starts, sizeof(u32) * buckets);
    write = 0;
    for (u32 r = 0; r < world->routeCount; r++) {
        AzgaarRoute* route = &world->routes[r];
        if (route->group == AZGAAR_ROUTE_SEAROUTE) continue;
        for (u32 p = 0; p < route->pointCount; p++) {
            float wx, wz;
            azgaarMapToWorld(world, route->points[p].x, route->points[p].y, &wx, &wz);
            i32 bx = (i32)((wx - g_road.minX) * g_road.invBucket);
            i32 bz = (i32)((wz - g_road.minZ) * g_road.invBucket);
            if (bx < 0) bx = 0;
            if (bz < 0) bz = 0;
            if (bx >= (i32)g_road.gridW) bx = (i32)g_road.gridW - 1;
            if (bz >= (i32)g_road.gridH) bz = (i32)g_road.gridH - 1;
            u32 b = (u32)bz * g_road.gridW + (u32)bx;
            g_road.cells[cursor[b]++] = write;
            write++;
        }
    }
    memoryFree(counts);
    memoryFree(cursor);
}

// ── Public API ─────────────────────────────────────────────────────────────

// NOTE: this runs on the game thread while the heightmap build thread may be
// concurrently calling azgaarSettlementsPlateauY (heightAt) while tiles are
// still streaming.  So we do NOT clear/zero the grid up front (an in-flight
// plateau call could be mid-loop over the old grid).  The new grid is built
// into a local struct and swapped in with `count` published last (see
// settGridBuild).  The old grid's arrays are freed only after the swap.
void azgaarSettlementsInit(const AzgaarWorld* world,
                            float (*groundAt)(void* userData, float wx, float wz),
                            void* groundUserData) {
    if (!world) return;
    g_world    = world;
    g_mapSeed  = settMapSeed(world->mapName);
    g_disabled  = getenv("ENGINE_AZGAAR_SETTLE_DISABLED") != NULL;
    if (g_disabled || world->settlementCount == 0) return;

    settRoadBuild(world);

    // Publish the plateau grid BEFORE generating instances: groundAt (the
    // heightmap source' heightAt) applies the D8 plateau, and each building's
    // Y is sampled through it — this only works once the new grid is live.
    SettGrid newGrid = {};
    settGridBuild(world, &newGrid);
    memoryFree(g_grid.starts);
    memoryFree(g_grid.cells);
    g_grid = newGrid;

    // Budget the temp buffer exactly (n per the plan's formula + specials).
    u32 cap = 0;
    for (u32 i = 0; i < world->settlementCount; i++) {
        const AzgaarSettlement* s = &world->settlements[i];
        u32 n = (u32)(2.0f + 9.0f * sqrtf(s->populationK) + 0.5f);
        if (n < 3u) n = 3u;
        if (n > 220u) n = 220u;
        cap += n;
        if (s->flags & AZGAAR_SETT_FLAG_WALLS) cap += 12;
        if (s->flags & AZGAAR_SETT_FLAG_CITADEL) cap += 2;
        if (s->flags & AZGAAR_SETT_FLAG_TEMPLE) cap += 1;
        if (s->flags & AZGAAR_SETT_FLAG_PORT) cap += 3; // up to 3 piers
        if (s->flags & AZGAAR_SETT_FLAG_SHANTY) cap += 4;
    }
    PropInstance* temp = static_cast<PropInstance*>(memoryAlloc(sizeof(PropInstance) * cap));
    u32 write = 0;
    for (u32 i = 0; i < world->settlementCount; i++) {
        const AzgaarSettlement* s = &world->settlements[i];
        u32 seed = g_mapSeed ^ s->id * 374761393u;
        settGenOne(world, s, seed, temp, cap, &write, groundAt, groundUserData);
    }
    g_instanceCount = write;

    // Group the unsorted instances by species, then upload to the pass.
    u32 perSpecies[AZGAAR_PROP_COUNT] = {};
    for (u32 i = 0; i < g_instanceCount; i++) {
        u32 sp = temp[i].species;
        if (sp < AZGAAR_PROP_COUNT) perSpecies[sp]++;
    }
    if (g_instanceCount > 0) {
        // The pass keeps its own GPU copy; the CPU arrays are ours to replace.
        memoryFree(g_instances);
        memoryFree(g_ranges);
        g_instances  = static_cast<PropInstance*>(memoryAlloc(sizeof(PropInstance) * g_instanceCount));
        u32 offsets[AZGAAR_PROP_COUNT] = {};
        u32 acc = 0;
        for (u32 s2 = 0; s2 < AZGAAR_PROP_COUNT; s2++) {
            offsets[s2] = acc;
            acc += perSpecies[s2];
        }
        u32 cursor[AZGAAR_PROP_COUNT] = {};
        for (u32 i = 0; i < g_instanceCount; i++) {
            u32 sp = temp[i].species;
            u32 dst = offsets[sp] + cursor[sp]++;
            g_instances[dst] = temp[i];
        }
        g_ranges  = static_cast<PropTileRange*>(memoryAlloc(sizeof(PropTileRange) * AZGAAR_PROP_COUNT));
        u32 rc = 0;
        for (u32 s2 = 0; s2 < AZGAAR_PROP_COUNT; s2++) {
            if (perSpecies[s2] > 0) {
                g_ranges[rc] = (PropTileRange){.species = s2, .start = offsets[s2], .count = perSpecies[s2]};
                rc++;
            }
        }
        g_rangeCount = rc;

        float halfW = (float)(world->widthPx * 0.5) * (float)world->metersPerPixel + 40.0f;
        float halfH = (float)(world->heightPx * 0.5) * (float)world->metersPerPixel + 40.0f;
        float aabbMin[3] = {-halfW, -20.0f, -halfH};
        float aabbMax[3] = {halfW, world->maxLandHeightM + 20.0f, halfH};
        azgaarPropsRegisterGlobal(g_instances, g_instanceCount,
                                    g_ranges, g_rangeCount, aabbMin, aabbMax, false);
        info("azgaarSettlements: uploaded %u building instances in %u species ranges",
             g_instanceCount, g_rangeCount);
    }
    memoryFree(temp);
}

void azgaarSettlementsClear(void) {
    azgaarPropsClearGlobal(false);
    memoryFree(g_instances);
    g_instances     = NULL;
    g_instanceCount = 0;
    memoryFree(g_ranges);
    g_ranges     = NULL;
    g_rangeCount = 0;
    memoryFree(g_grid.starts);
    memoryFree(g_grid.cells);
    g_grid = (SettGrid){0};
    memoryFree(g_road.pts);
    memoryFree(g_road.starts);
    memoryFree(g_road.cells);
    g_road = (SettRoadHash){0};
    g_world    = NULL;
    g_disabled = false;
}

float azgaarSettlementsPlateauY(const AzgaarWorld* world, float wx, float wz, float naturalY) {
    if (g_disabled || !world) return naturalY;

    // Snapshot the grid state at entry: an in-flight call keeps its snapshot
    // even if the game thread swaps in a fresh grid mid-call.
    u32   count    = g_grid.count;
    u32*  starts   = g_grid.starts;
    u32*  cells    = g_grid.cells;
    if (count == 0 || !starts || !cells) return naturalY;
    float invBucket = g_grid.invBucket;
    float minX      = g_grid.minX;
    float minZ      = g_grid.minZ;
    u32   gridW     = g_grid.gridW;
    u32   gridH     = g_grid.gridH;
    u32   buckets   = gridW * gridH;

    float y = naturalY;
    i32 bx = (i32)((wx - minX) * invBucket);
    i32 bz = (i32)((wz - minZ) * invBucket);
    for (i32 oz = -1; oz <= 1; oz++) {
        for (i32 ox = -1; ox <= 1; ox++) {
            i32 nx = bx + ox, nz = bz + oz;
            if (nx < 0 || nz < 0 || nx >= (i32)gridW || nz >= (i32)gridH) continue;
            u32 b = (u32)nz * gridW + (u32)nx;
            u32 lo = starts[b];
            u32 hi = (b + 1 < buckets) ? starts[b + 1] : count;
            for (u32 i = lo; i < hi; i++) {
                const AzgaarSettlement* s = &world->settlements[cells[i]];
                float dx = wx - s->wx;
                float dz = wz - s->wz;
                float d = sqrtf(dx * dx + dz * dz);
                if (d < s->radiusM) {
                    // D8: y' = mix(y, flatY, 1 - smoothstep(0.55r, r, d))
                    float t = 1.0f - settSmoothstep01(0.55f * s->radiusM, s->radiusM, d);
                    if (t > 0.0f) y += (s->flatY - y) * t;
                }
            }
        }
    }
    return y;
}

const AzgaarSettlement* azgaarSettlementsNearest(const AzgaarWorld* world, float wx, float wz) {
    if (g_disabled || !world || world->settlementCount == 0) return NULL;
    const AzgaarSettlement* best = NULL;
    float bestD = 0.0f;
    for (u32 i = 0; i < world->settlementCount; i++) {
        const AzgaarSettlement* s = &world->settlements[i];
        float dx = wx - s->wx;
        float dz = wz - s->wz;
        float d = sqrtf(dx * dx + dz * dz);
        if (d <= s->radiusM + 30.0f) {
            if (!best || d < bestD) {
                best = s;
                bestD = d;
            }
        }
    }
    return best;
}
