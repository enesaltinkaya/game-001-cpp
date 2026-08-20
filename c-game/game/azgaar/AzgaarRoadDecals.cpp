#include "azgaar/AzgaarRoadDecals.h"
#include "azgaar/AzgaarRoadCorridor.h"
#include "azgaar/AzgaarSettlements.h"
#include "renderer/decal/Decal.h"
#include "ecs/system/heightmap/HeightmapTerrain.h"
#include <math.h>

#define AZGAAR_ROAD_DECAL_MAX_HANDLES 8192u

static u32  handles[AZGAAR_ROAD_DECAL_MAX_HANDLES];
static u32  handleCount;
static bool visible = true;

static float routeWidth(AzgaarRouteGroup group) {
    switch (group) {
        case AZGAAR_ROUTE_ROAD: return 34.0f;
        case AZGAAR_ROUTE_TRAIL: return 12.0f;
        default: return 0.0f;
    }
}

static float routeMaxLen(AzgaarRouteGroup group) {
    return group == AZGAAR_ROUTE_ROAD ? 260.0f : 180.0f;
}

static void routeColor(AzgaarRouteGroup group, vec4 out) {
    if (group == AZGAAR_ROUTE_ROAD) {
        glm_vec4_copy((vec4){0.42f, 0.30f, 0.18f, 0.42f}, out);
    } else {
        glm_vec4_copy((vec4){0.36f, 0.27f, 0.16f, 0.34f}, out);
    }
}

void azgaarRoadDecalsClear(void) {
    for (u32 i = 0; i < handleCount; ++i) decalRemove(handles[i]);
    handleCount = 0;
}

void azgaarRoadDecalsSetVisible(bool isVisible) {
    visible = isVisible;
    if (!visible) azgaarRoadDecalsClear();
}

static void addSegment(const AzgaarWorld* world,
                       AzgaarRouteGroup group,
                       float ax,
                       float ay,
                       float bx,
                       float by) {
    if (handleCount >= AZGAAR_ROAD_DECAL_MAX_HANDLES) return;

    float awx, awz, bwx, bwz;
    azgaarMapToWorld(world, ax, ay, &awx, &awz);
    azgaarMapToWorld(world, bx, by, &bwx, &bwz);

    float dx = bwx - awx;
    float dz = bwz - awz;
    float len = sqrtf(dx * dx + dz * dz);
    if (len < 1.0f) return;

    float width = routeWidth(group);
    float maxLen = routeMaxLen(group);
    u32 pieces = (u32)ceilf(len / maxLen);
    if (pieces < 1u) pieces = 1u;

    for (u32 p = 0; p < pieces && handleCount < AZGAAR_ROAD_DECAL_MAX_HANDLES; ++p) {
        float t0 = (float)p / (float)pieces;
        float t1 = (float)(p + 1u) / (float)pieces;
        float sx = awx + dx * t0;
        float sz = awz + dz * t0;
        float ex = awx + dx * t1;
        float ez = awz + dz * t1;
        float sdx = ex - sx;
        float sdz = ez - sz;
        float slen = sqrtf(sdx * sdx + sdz * sdz);
        if (slen < 1.0f) continue;

        float mxPx = ax + (bx - ax) * ((t0 + t1) * 0.5f);
        float myPx = ay + (by - ay) * ((t0 + t1) * 0.5f);
        float midWx    = (sx + ex) * 0.5f;
        float midWz    = (sz + ez) * 0.5f;
        // The decal's natural height follows the heightmap surface (the same
        // source the terrain tiles bake, so decals stay glued through
        // streaming). While tiles are still generating the sample falls back
        // to the source height, which is the identical value.  The fallback
        // applies the settlement plateau (workstream D) so road decals that
        // run through a town stay glued to the flattened ground.
        HeightmapTerrain* ht = heightmapTerrainGetActive();
        float naturalH = ht ? heightmapTerrainSample(ht, midWx, midWz)
                            : azgaarSettlementsPlateauY(world, midWx, midWz,
                                                         azgaarHeightToMeters(world, azgaarWorldSampleHeightSmooth(world, mxPx, myPx)));
        // The decal sits on the road centerline, so sample the corridor
        // (grade-limited) surface height when available; otherwise fall back
        // to the natural terrain height above.
        float h;
        if (!azgaarRoadCorridorSample(midWx, midWz, naturalH, &h)) h = naturalH;
        float yaw = atan2f(sdx, sdz);

        DecalInstance d = {};
        d.position[0] = (sx + ex) * 0.5f;
        // Tall projector so the box always covers the rendered terrain even
        // when the sampled height h is slightly off. The shader keeps only
        // back faces (one fragment per pixel per decal), so a tall volume no
        // longer causes double-blend bands when the camera is near.
        d.position[1] = h + 40.0f;
        d.position[2] = (sz + ez) * 0.5f;
        d.halfExtents[0] = width * 0.5f;
        d.halfExtents[1] = 140.0f;
        // Road segments share exact endpoints by construction (segment i's end
        // == segment i+1's start), so adjacent projector boxes meet exactly
        // along the shared segment boundary with no overlap. Any overlap would
        // double alpha-blend and show up as a brighter cross-band at joints.
        d.halfExtents[2] = slen * 0.5f;
        glm_quatv(d.rotation, yaw, (vec3){0.0f, 1.0f, 0.0f});
        routeColor(group, d.color);
        d.textureId = DECAL_PROCEDURAL_CIRCLE_TEXTURE;
        // Road rectangles overlap at junctions; the union-blend road layer
        // keeps coverage idempotent there so overlaps don't darken.
        d.flags = DECAL_FLAG_GROUND_ONLY | DECAL_FLAG_ROAD;
        d.opacity = 1.0f;
        d.normalThreshold = 0.28f;
        d.edgeFeather = group == AZGAAR_ROUTE_ROAD ? 0.30f : 0.38f;
        d.uvScale[0] = 1.0f;
        d.uvScale[1] = fmaxf(1.0f, slen / width);

        u32 handle = decalAdd(&d);
        if (handle != DECAL_INVALID_HANDLE) handles[handleCount++] = handle;
    }
}

void azgaarRoadDecalsBuild(const AzgaarWorld* world) {
    azgaarRoadDecalsClear();
    if (!visible || !world || !world->routes) return;

    u32 roads = 0, trails = 0, skipped = 0;
    for (u32 r = 0; r < world->routeCount; ++r) {
        const AzgaarRoute* route = &world->routes[r];
        if (route->pointCount < 2u) continue;
        if (route->group == AZGAAR_ROUTE_SEAROUTE) { skipped++; continue; }
        if (route->group == AZGAAR_ROUTE_ROAD) roads++; else trails++;
        for (u32 i = 0; i + 1u < route->pointCount; ++i) {
            addSegment(world,
                       route->group,
                       route->points[i].x,
                       route->points[i].y,
                       route->points[i + 1u].x,
                       route->points[i + 1u].y);
        }
    }

    info("Azgaar road decals: %u decals from %u roads, %u trails (%u searoutes skipped)",
         handleCount,
         roads,
         trails,
         skipped);
}
