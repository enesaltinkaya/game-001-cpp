/**
 * navmesh-builder: Standalone tool that reads glTF/GLB files, collects
 * all collision geometry, and generates a tiled Recast/Detour navmesh.
 *
 * Usage: navmesh-builder <input.glb> <output.nav> [--obstacles obs1.glb ...]
 *
 * Output binary format (v2):
 *   Header:
 *     u8[4]   magic = "NAVM"
 *     u32     version = 2
 *     float   bmin[3]     (navmesh AABB min)
 *     float   bmax[3]     (navmesh AABB max)
 *     u32     tileCount
 *     float   tileWorldSize (world-space size of one tile)
 *   Per tile:
 *     u32     tileSize    (Detour tile data size)
 *     u8[]    tileData    (serialized Detour navmesh tile)
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>
#include <atomic>
#include <thread>

// ── cgltf ──────────────────────────────────────────────────────────────────
#define CGLTF_IMPLEMENTATION
extern "C" {
#include "cgltf.h"
}

// ── meshoptimizer (for decompressing gltfpack output) ──────────────────────
#include "meshoptimizer.h"

// ── Recast + Detour ────────────────────────────────────────────────────────
#include "Recast.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include "DetourCommon.h"

typedef uint32_t u32;
typedef uint8_t u8;

static const u8 MAGIC[4] = {'N', 'A', 'V', 'M'};
static const u32 VERSION = 2;

// ── Navmesh config ─────────────────────────────────────────────────────────
// Configurable via environment variables.

static float g_cellSize         = 0.30f;  // outdoor/dev default: sparse pathing mesh
static float g_cellHeight       = 0.20f;
static float g_agentHeight      = 1.80f;
static float g_agentRadius      = 0.50f;  // slightly larger to keep agents away from cliff edges
static float g_agentMaxClimb    = 0.60f;  // outdoor terrain often has ledges/steps
static float g_agentMaxSlope    = 45.0f;
static float g_regionMinSize    = 8.0f;   // discard tiny isolated patches
static float g_regionMergeSize  = 20.0f;  // merge small regions into neighbours
static float g_edgeMaxLen       = 12.0f;  // sparse contours for faster outdoor bakes
static float g_edgeMaxError     = 0.30f;
static float g_detailSampleDist = 2.0f;   // detail mesh is mostly for height/debug, not path topology
static float g_detailSampleErr  = 0.25f;
static int g_tileSize           = 256;  // larger tiles → fewer tiles → less overhead

// below are ideal for indoor scenes
// static float g_cellSize         = 0.15f;
// static float g_cellHeight       = 0.10f;
// static float g_agentHeight      = 1.80f;
// static float g_agentRadius      = 0.30f;
// static float g_agentMaxClimb    = 0.25f;
// static float g_agentMaxSlope    = 45.0f;
// static float g_regionMinSize    = 2.0f;
// static float g_regionMergeSize  = 4.0f;
// static float g_edgeMaxLen       = 4.0f;
// static float g_edgeMaxError     = 0.1f;
// static float g_detailSampleDist = 1.0f;
// static float g_detailSampleErr  = 0.25f;
// static int g_tileSize           = 48;

// Obstacle AABBs (from CONVEX_HULL shapes) to block navmesh spans
struct ObstacleAABB {
    float bmin[3];
    float bmax[3];
};

static std::vector<ObstacleAABB> g_obstacleAABBs;

static float env_float(const char* name, float def) {
    const char* val = getenv(name);
    return val ? (float)atof(val) : def;
}

static int env_int(const char* name, int def) {
    const char* val = getenv(name);
    return val ? atoi(val) : def;
}

// ── helpers ────────────────────────────────────────────────────────────────

static void write_u32(FILE* f, u32 v) {
    fwrite(&v, 4, 1, f);
}

static int decompress_meshopt(cgltf_data* data) {
    for (size_t i = 0; i < data->buffer_views_count; i++) {
        if (!data->buffer_views[i].has_meshopt_compression) continue;
        cgltf_meshopt_compression* mc = &data->buffer_views[i].meshopt_compression;
        const unsigned char* source   = (const unsigned char*)mc->buffer->data;
        if (!source) return -1;
        source += mc->offset;

        void* result = malloc(mc->count * mc->stride);
        if (!result) return -1;

        int rc = -1;
        switch (mc->mode) {
            case cgltf_meshopt_compression_mode_attributes:
                rc = meshopt_decodeVertexBuffer(result, mc->count, mc->stride, source, mc->size);
                break;
            case cgltf_meshopt_compression_mode_triangles:
                rc = meshopt_decodeIndexBuffer(result, mc->count, mc->stride, source, mc->size);
                break;
            case cgltf_meshopt_compression_mode_indices:
                rc = meshopt_decodeIndexSequence(result, mc->count, mc->stride, source, mc->size);
                break;
            default:
                break;
        }
        if (rc != 0) {
            free(result);
            return -1;
        }

        switch (mc->filter) {
            case cgltf_meshopt_compression_filter_octahedral:
                meshopt_decodeFilterOct(result, mc->count, mc->stride);
                break;
            case cgltf_meshopt_compression_filter_quaternion:
                meshopt_decodeFilterQuat(result, mc->count, mc->stride);
                break;
            case cgltf_meshopt_compression_filter_exponential:
                meshopt_decodeFilterExp(result, mc->count, mc->stride);
                break;
            default:
                break;
        }
        data->buffer_views[i].data = result;
    }
    return 0;
}

// ── JSON extra parsing ────────────────────────────────────────────────────

static std::string get_extra_string(cgltf_node* node, const char* key) {
    if (!node->extras.data) return {};
    std::string data(node->extras.data);
    std::string needle = std::string("\"") + key + "\"";
    size_t pos         = data.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    while (pos < data.size() && (data[pos] == ' ' || data[pos] == ':' || data[pos] == '\t')) pos++;
    if (pos >= data.size() || data[pos] != '"') return {};
    size_t end = data.find('"', pos + 1);
    if (end == std::string::npos) return {};
    return data.substr(pos + 1, end - pos - 1);
}

// ── Triangle collection ───────────────────────────────────────────────────

struct TriSoup {
    std::vector<float> verts;       // x,y,z per vertex
    std::vector<uint32_t> indices;  // triangle indices
};

static bool get_positions(cgltf_primitive* prim, std::vector<float>& outVerts) {
    for (size_t ai = 0; ai < prim->attributes_count; ai++) {
        if (prim->attributes[ai].type == cgltf_attribute_type_position) {
            cgltf_accessor* acc = prim->attributes[ai].data;
            size_t count        = acc->count;
            outVerts.resize(count * 3);
            cgltf_accessor_unpack_floats(acc, outVerts.data(), count * 3);
            return true;
        }
    }
    return false;
}

static bool get_indices(cgltf_primitive* prim, std::vector<uint32_t>& outIndices) {
    cgltf_accessor* idx = prim->indices;
    if (!idx) return false;
    size_t count = idx->count;
    outIndices.resize(count);
    cgltf_accessor_unpack_indices(idx, outIndices.data(), sizeof(uint32_t), count);
    return true;
}

static void transform_positions(std::vector<float>& positions, const float* nodeWorldTransform) {
    for (size_t i = 0; i < positions.size(); i += 3) {
        float x = positions[i], y = positions[i + 1], z = positions[i + 2];
        positions[i]     = nodeWorldTransform[0] * x + nodeWorldTransform[4] * y +
                           nodeWorldTransform[8] * z + nodeWorldTransform[12];
        positions[i + 1] = nodeWorldTransform[1] * x + nodeWorldTransform[5] * y +
                           nodeWorldTransform[9] * z + nodeWorldTransform[13];
        positions[i + 2] = nodeWorldTransform[2] * x + nodeWorldTransform[6] * y +
                           nodeWorldTransform[10] * z + nodeWorldTransform[14];
    }
}

static void add_primitive(TriSoup& soup, cgltf_primitive* prim, const float* nodeWorldTransform) {
    std::vector<float> positions;
    std::vector<uint32_t> indices;

    if (!get_positions(prim, positions)) return;
    if (!get_indices(prim, indices)) {
        size_t vertCount = positions.size() / 3;
        indices.resize(vertCount);
        for (size_t i = 0; i < vertCount; i++) indices[i] = (uint32_t)i;
    }

    if (nodeWorldTransform) {
        transform_positions(positions, nodeWorldTransform);
    }

    uint32_t baseVertex = (uint32_t)(soup.verts.size() / 3);
    soup.verts.insert(soup.verts.end(), positions.begin(), positions.end());

    for (size_t i = 0; i < indices.size(); i++) {
        soup.indices.push_back(baseVertex + indices[i]);
    }
}

static void collect_triangles(cgltf_node* node, TriSoup& soup, bool collisionOnly) {
    float worldTransform[16];
    cgltf_node_transform_world(node, worldTransform);
    bool hasTransform = false;
    for (int i = 0; i < 16; i++) {
        if (i == 0 || i == 5 || i == 10 || i == 15) {
            if (worldTransform[i] != 1.0f) {
                hasTransform = true;
                break;
            }
        } else {
            if (worldTransform[i] != 0.0f) {
                hasTransform = true;
                break;
            }
        }
    }

    if (node->mesh) {
        bool collect = false;

        bool isConvexHull = false;
        if (collisionOnly) {
            std::string rbShape = get_extra_string(node, "rigidBodyShape");
            isConvexHull        = (rbShape == "CONVEX_HULL");
            if (!rbShape.empty()) collect = true;
        } else {
            collect = true;
        }

        if (collect) {
            if (isConvexHull) {
                // For CONVEX_HULL, generate an extruded AABB box instead of
                // using raw mesh triangles. Thin walls from the hull mesh don't
                // voxelize reliably, creating disconnected interior navmesh
                // pockets. A solid extruded box ensures clean voxelization.
                float bmin[3]  = {FLT_MAX, FLT_MAX, FLT_MAX};
                float bmax[3]  = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
                bool gotBounds = false;
                for (size_t p = 0; p < node->mesh->primitives_count; p++) {
                    cgltf_accessor* acc = node->mesh->primitives[p].attributes_count > 0
                                              ? node->mesh->primitives[p].attributes[0].data
                                              : nullptr;
                    if (acc && acc->has_min && acc->has_max && acc->is_sparse == 0) {
                        float localMin[3] = {acc->min[0], acc->min[1], acc->min[2]};
                        float localMax[3] = {acc->max[0], acc->max[1], acc->max[2]};
                        // Transform all 8 corners of local AABB through world matrix
                        for (int cz = 0; cz < 2; cz++) {
                            for (int cy = 0; cy < 2; cy++) {
                                for (int cx = 0; cx < 2; cx++) {
                                    float pt[3] = {cx ? localMax[0] : localMin[0],
                                                   cy ? localMax[1] : localMin[1],
                                                   cz ? localMax[2] : localMin[2]};
                                    float wp[3];
                                    if (hasTransform) {
                                        wp[0] = worldTransform[0] * pt[0] +
                                                worldTransform[4] * pt[1] +
                                                worldTransform[8] * pt[2] + worldTransform[12];
                                        wp[1] = worldTransform[1] * pt[0] +
                                                worldTransform[5] * pt[1] +
                                                worldTransform[9] * pt[2] + worldTransform[13];
                                        wp[2] = worldTransform[2] * pt[0] +
                                                worldTransform[6] * pt[1] +
                                                worldTransform[10] * pt[2] + worldTransform[14];
                                    } else {
                                        wp[0] = pt[0];
                                        wp[1] = pt[1];
                                        wp[2] = pt[2];
                                    }
                                    for (int ax = 0; ax < 3; ax++) {
                                        if (wp[ax] < bmin[ax]) bmin[ax] = wp[ax];
                                        if (wp[ax] > bmax[ax]) bmax[ax] = wp[ax];
                                    }
                                }
                            }
                        }
                        gotBounds = true;
                    }
                }
                if (gotBounds) {
                    bmin[1] -= 20.0f;
                    float pad = g_agentRadius;
                    bmin[0] -= pad;
                    bmin[2] -= pad;
                    bmax[0] += pad;
                    bmax[2] += pad;
                    ObstacleAABB aabb;
                    rcVcopy(aabb.bmin, bmin);
                    rcVcopy(aabb.bmax, bmax);
                    g_obstacleAABBs.push_back(aabb);
                    printf("  CONVEX_HULL '%s' obstacle AABB (%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)\n",
                           node->name ? node->name : "(unnamed)",
                           bmin[0],
                           bmin[1],
                           bmin[2],
                           bmax[0],
                           bmax[1],
                           bmax[2]);
                }
            } else {
                for (size_t p = 0; p < node->mesh->primitives_count; p++) {
                    add_primitive(soup,
                                  &node->mesh->primitives[p],
                                  hasTransform ? worldTransform : nullptr);
                }
            }
            float nodePos[3] = {worldTransform[12], worldTransform[13], worldTransform[14]};
            printf("  collected node '%s' at (%.1f,%.1f,%.1f), %zu prims, hasTransform=%d\n",
                   node->name ? node->name : "(unnamed)",
                   nodePos[0],
                   nodePos[1],
                   nodePos[2],
                   node->mesh->primitives_count,
                   hasTransform);
        }
    }

    for (size_t i = 0; i < node->children_count; i++) {
        collect_triangles(node->children[i], soup, collisionOnly);
    }
}

// ── Triangle binning to tiles ──────────────────────────────────────────────

static float tile_query_padding(float cellSize) {
    int walkableRadius = (int)ceilf(g_agentRadius / cellSize);
    int borderSize     = walkableRadius + 3;
    return (float)borderSize * cellSize + g_agentRadius * 2.0f;
}

static void bin_triangles_to_tiles(const TriSoup& soup,
                                   const float* bmin,
                                   const float* bmax,
                                   float tileWorldSize,
                                   int tw,
                                   int th,
                                   std::vector<std::vector<uint32_t>>& tileTris) {
    float padding = tile_query_padding(g_cellSize);
    size_t triCount = soup.indices.size() / 3;

    for (size_t tri = 0; tri < triCount; tri++) {
        size_t t = tri * 3;
        const float* v0 = &soup.verts[soup.indices[t] * 3];
        const float* v1 = &soup.verts[soup.indices[t + 1] * 3];
        const float* v2 = &soup.verts[soup.indices[t + 2] * 3];

        float triMin[3], triMax[3];
        triMin[0] = std::min({v0[0], v1[0], v2[0]});
        triMin[1] = std::min({v0[1], v1[1], v2[1]});
        triMin[2] = std::min({v0[2], v1[2], v2[2]});
        triMax[0] = std::max({v0[0], v1[0], v2[0]});
        triMax[1] = std::max({v0[1], v1[1], v2[1]});
        triMax[2] = std::max({v0[2], v1[2], v2[2]});

        // Match the previous per-tile AABB test, including padded Y bounds.
        if (triMax[1] < bmin[1] - padding || triMin[1] > bmax[1] + padding) continue;

        int tx0 = (int)floorf((triMin[0] - padding - bmin[0]) / tileWorldSize);
        int tx1 = (int)floorf((triMax[0] + padding - bmin[0]) / tileWorldSize);
        int ty0 = (int)floorf((triMin[2] - padding - bmin[2]) / tileWorldSize);
        int ty1 = (int)floorf((triMax[2] + padding - bmin[2]) / tileWorldSize);

        tx0 = rcClamp(tx0, 0, tw - 1);
        tx1 = rcClamp(tx1, 0, tw - 1);
        ty0 = rcClamp(ty0, 0, th - 1);
        ty1 = rcClamp(ty1, 0, th - 1);

        for (int ty = ty0; ty <= ty1; ty++) {
            for (int tx = tx0; tx <= tx1; tx++) {
                float pbminX = bmin[0] + tx * tileWorldSize - padding;
                float pbmaxX = bmin[0] + (tx + 1) * tileWorldSize + padding;
                float pbminZ = bmin[2] + ty * tileWorldSize - padding;
                float pbmaxZ = bmin[2] + (ty + 1) * tileWorldSize + padding;

                // Keep this exactly equivalent to the old per-tile clipping test.
                if (triMax[0] < pbminX || triMin[0] > pbmaxX) continue;
                if (triMax[2] < pbminZ || triMin[2] > pbmaxZ) continue;

                tileTris[(size_t)(tx + ty * tw)].push_back((uint32_t)tri);
            }
        }
    }
}

static TriSoup make_tile_soup(const TriSoup& soup, const std::vector<uint32_t>& tris) {
    TriSoup clipped;
    clipped.verts.reserve(tris.size() * 9);
    clipped.indices.reserve(tris.size() * 3);

    for (uint32_t tri : tris) {
        size_t t = (size_t)tri * 3;
        const float* v0 = &soup.verts[soup.indices[t] * 3];
        const float* v1 = &soup.verts[soup.indices[t + 1] * 3];
        const float* v2 = &soup.verts[soup.indices[t + 2] * 3];

        uint32_t base = (uint32_t)(clipped.verts.size() / 3);
        clipped.verts.insert(clipped.verts.end(), v0, v0 + 3);
        clipped.verts.insert(clipped.verts.end(), v1, v1 + 3);
        clipped.verts.insert(clipped.verts.end(), v2, v2 + 3);
        clipped.indices.push_back(base);
        clipped.indices.push_back(base + 1);
        clipped.indices.push_back(base + 2);
    }
    return clipped;
}

// ── Single tile build ──────────────────────────────────────────────────────

static bool build_tile(const TriSoup& soup,
                       const float* bmin,
                       const float* bmax,
                       float cellSize,
                       int tileX,
                       int tileY,
                       std::vector<u8>& outTileData) {
    if (soup.verts.empty() || soup.indices.empty()) return false;

    int walkableHeight = (int)ceilf(g_agentHeight / g_cellHeight);
    int walkableClimb  = (int)floorf(g_agentMaxClimb / g_cellHeight);
    int walkableRadius = (int)ceilf(g_agentRadius / cellSize);
    int borderSize     = walkableRadius + 3;

    float expandedBmin[3], expandedBmax[3];
    float borderWorldSize = (float)borderSize * cellSize;
    rcVcopy(expandedBmin, bmin);
    rcVcopy(expandedBmax, bmax);
    expandedBmin[0] -= borderWorldSize;
    expandedBmin[2] -= borderWorldSize;
    expandedBmax[0] += borderWorldSize;
    expandedBmax[2] += borderWorldSize;

    rcContext* ctx = new rcContext();

    const float* verts = soup.verts.data();
    int vertCount      = (int)(soup.verts.size() / 3);

    // Debug: Y range of input verts
    float yMin = FLT_MAX, yMax = -FLT_MAX;
    for (int i = 0; i < vertCount; i++) {
        float y = verts[i * 3 + 1];
        if (y < yMin) yMin = y;
        if (y > yMax) yMax = y;
    }
    // printf("    tile (%d,%d): %d tris, %d verts, Y=[%.1f,%.1f]\n",
    //        tileX,
    //        tileY,
    //        (int)(soup.indices.size() / 3),
    //        vertCount,
    //        yMin,
    //        yMax);

    int triCount    = (int)(soup.indices.size() / 3);
    int* triIndices = (int*)malloc(soup.indices.size() * sizeof(int));
    for (size_t i = 0; i < soup.indices.size(); i++) {
        triIndices[i] = (int)soup.indices[i];
    }

    int gw, gh;
    rcCalcGridSize(expandedBmin, expandedBmax, cellSize, &gw, &gh);

    rcHeightfield* solid = rcAllocHeightfield();
    if (!rcCreateHeightfield(ctx,
                             *solid,
                             gw,
                             gh,
                             expandedBmin,
                             expandedBmax,
                             cellSize,
                             g_cellHeight)) {
        free(triIndices);
        rcFreeHeightField(solid);
        delete ctx;
        return false;
    }

    unsigned char* triAreas = (unsigned char*)calloc(triCount, 1);
    rcMarkWalkableTriangles(ctx, g_agentMaxSlope, verts, vertCount, triIndices, triCount, triAreas);

    if (!rcRasterizeTriangles(ctx, verts, vertCount, triIndices, triAreas, triCount, *solid, 1)) {
        free(triAreas);
        rcFreeHeightField(solid);
        free(triIndices);
        delete ctx;
        return false;
    }
    free(triAreas);
    free(triIndices);

    // Debug: count spans
    unsigned int spanCount = 0;
    for (int z = 0; z < solid->height; z++)
        for (int x = 0; x < solid->width; x++)
            for (rcSpan* s = solid->spans[x + z * solid->width]; s; s = s->next) spanCount++;
    // printf("    tile (%d,%d): %d tris, %d verts, grid %dx%d, %u spans\n",
    //        tileX,
    //        tileY,
    //        triCount,
    //        vertCount,
    //        gw,
    //        gh,
    //        spanCount);

    rcFilterLowHangingWalkableObstacles(ctx, walkableClimb, *solid);
    rcFilterLedgeSpans(ctx, walkableHeight, walkableClimb, *solid);
    rcFilterWalkableLowHeightSpans(ctx, walkableHeight, *solid);

    // Block spans inside obstacle AABBs (CONVEX_HULL shapes)
    for (const auto& obs : g_obstacleAABBs) {
        // Convert obstacle AABB to heightfield grid coords
        int x0                    = (int)std::floor((obs.bmin[0] - expandedBmin[0]) / cellSize);
        int x1                    = (int)std::ceil((obs.bmax[0] - expandedBmin[0]) / cellSize);
        int z0                    = (int)std::floor((obs.bmin[2] - expandedBmin[2]) / cellSize);
        int z1                    = (int)std::ceil((obs.bmax[2] - expandedBmin[2]) / cellSize);
        x0                        = rcClamp(x0, 0, solid->width - 1);
        x1                        = rcClamp(x1, 0, solid->width - 1);
        z0                        = rcClamp(z0, 0, solid->height - 1);
        z1                        = rcClamp(z1, 0, solid->height - 1);
        unsigned int blockedCount = 0;
        for (int z = z0; z <= z1; z++) {
            for (int x = x0; x <= x1; x++) {
                rcSpan* span = solid->spans[x + z * solid->width];
                while (span) {
                    float spanY = (float)span->smax * g_cellHeight;
                    if (spanY >= obs.bmin[1] && (float)span->smin * g_cellHeight <= obs.bmax[1]) {
                        span->area = RC_NULL_AREA;
                        blockedCount++;
                    }
                    span = span->next;
                }
            }
        }
    }

    rcCompactHeightfield* chf = rcAllocCompactHeightfield();
    if (!rcBuildCompactHeightfield(ctx, walkableHeight, walkableClimb, *solid, *chf)) {
        rcFreeHeightField(solid);
        rcFreeCompactHeightfield(chf);
        delete ctx;
        return false;
    }
    rcFreeHeightField(solid);

    // printf("    tile (%d,%d): chf %d walkable / %d total cells\n",
    //        tileX,
    //        tileY,
    //        chf->spanCount,
    //        chf->width * chf->height);

    if (!rcErodeWalkableArea(ctx, walkableRadius, *chf)) {
        rcFreeCompactHeightfield(chf);
        delete ctx;
        return false;
    }

    if (!rcBuildDistanceField(ctx, *chf)) {
        rcFreeCompactHeightfield(chf);
        delete ctx;
        return false;
    }
    if (!rcBuildRegionsMonotone(ctx,
                                *chf,
                                borderSize,
                                (int)(g_regionMinSize * 0.1f),
                                (int)(g_regionMergeSize * 0.1f))) {
        rcFreeCompactHeightfield(chf);
        delete ctx;
        return false;
    }

    rcContourSet* cset = rcAllocContourSet();
    if (!rcBuildContours(ctx, *chf, g_edgeMaxError, (int)g_edgeMaxLen, *cset)) {
        rcFreeContourSet(cset);
        rcFreeCompactHeightfield(chf);
        printf("    tile (%d,%d): contours FAILED\n", tileX, tileY);
        delete ctx;
        return false;
    }

    // printf("    tile (%d,%d): %d contours\n", tileX, tileY, cset->nconts);

    if (cset->nconts == 0) {
        rcFreeContourSet(cset);
        rcFreeCompactHeightfield(chf);
        delete ctx;
        return false;
    }

    rcPolyMesh* pmesh = rcAllocPolyMesh();
    if (!rcBuildPolyMesh(ctx, *cset, 6, *pmesh)) {
        rcFreePolyMesh(pmesh);
        rcFreeContourSet(cset);
        rcFreeCompactHeightfield(chf);
        delete ctx;
        return false;
    }

    rcPolyMeshDetail* dmesh = rcAllocPolyMeshDetail();
    if (!rcBuildPolyMeshDetail(ctx, *pmesh, *chf, g_detailSampleDist, g_detailSampleErr, *dmesh)) {
        rcFreePolyMeshDetail(dmesh);
        rcFreePolyMesh(pmesh);
        rcFreeContourSet(cset);
        rcFreeCompactHeightfield(chf);
        delete ctx;
        return false;
    }

    rcFreeContourSet(cset);
    rcFreeCompactHeightfield(chf);

    for (int i = 0; i < pmesh->npolys; i++) pmesh->flags[i] = 1;

    dtNavMeshCreateParams params = {};
    params.verts                 = pmesh->verts;
    params.vertCount             = pmesh->nverts;
    params.polys                 = pmesh->polys;
    params.polyFlags             = pmesh->flags;
    params.polyAreas             = pmesh->areas;
    params.polyCount             = pmesh->npolys;
    params.nvp                   = pmesh->nvp;
    params.detailMeshes          = dmesh->meshes;
    params.detailVerts           = dmesh->verts;
    params.detailVertsCount      = dmesh->nverts;
    params.detailTris            = dmesh->tris;
    params.detailTriCount        = dmesh->ntris;
    params.walkableHeight        = g_agentHeight;
    params.walkableRadius        = g_agentRadius;
    params.walkableClimb         = g_agentMaxClimb;
    params.cs                    = cellSize;
    params.ch                    = g_cellHeight;
    params.tileX                 = tileX;
    params.tileY                 = tileY;
    params.tileLayer             = 0;
    params.buildBvTree           = true;
    rcVcopy(params.bmin, pmesh->bmin);
    rcVcopy(params.bmax, pmesh->bmax);

    unsigned char* tileData = nullptr;
    int tileDataSize        = 0;
    if (!dtCreateNavMeshData(&params, &tileData, &tileDataSize)) {
        rcFreePolyMeshDetail(dmesh);
        rcFreePolyMesh(pmesh);
        delete ctx;
        return false;
    }

    rcFreePolyMeshDetail(dmesh);
    rcFreePolyMesh(pmesh);
    delete ctx;

    outTileData.assign(tileData, tileData + tileDataSize);
    dtFree(tileData);
    return true;
}

// ── main ───────────────────────────────────────────────────────────────────

static void collect_glb_triangles(const char* path, TriSoup& soup, bool collisionOnly) {
    cgltf_options options = {};
    cgltf_data* data      = nullptr;
    cgltf_result result   = cgltf_parse_file(&options, path, &data);
    if (result != cgltf_result_success) {
        fprintf(stderr, "navmesh-builder: failed to parse %s (err %d)\n", path, result);
        return;
    }
    result = cgltf_load_buffers(&options, data, path);
    if (result != cgltf_result_success) {
        fprintf(stderr, "navmesh-builder: failed to load buffers for %s (err %d)\n", path, result);
        cgltf_free(data);
        return;
    }
    if (decompress_meshopt(data) != 0) {
        fprintf(stderr, "navmesh-builder: failed to decompress meshopt for %s\n", path);
        cgltf_free(data);
        return;
    }

    printf("  collecting from %s (%s)\n", path, collisionOnly ? "collision" : "all");
    if (data->scene) {
        for (size_t i = 0; i < data->scene->nodes_count; i++) {
            collect_triangles(data->scene->nodes[i], soup, collisionOnly);
        }
    }
    cgltf_free(data);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <input.glb> <output.nav> [--obstacles obs1.glb ...]\n", argv[0]);
        return 1;
    }

    const char* inputPath  = argv[1];
    const char* outputPath = argv[2];

    std::vector<std::string> obstaclePaths;
    for (int i = 3; i < argc; i++) {
        if (std::string(argv[i]) == "--obstacles") {
            for (int j = i + 1; j < argc; j++) {
                obstaclePaths.push_back(argv[j]);
            }
            break;
        }
    }

    // ── Config from env ────────────────────────────────────────────────────
    g_cellSize         = env_float("NAVMESH_CELL_SIZE", g_cellSize);
    g_cellHeight       = env_float("NAVMESH_CELL_HEIGHT", g_cellHeight);
    g_agentHeight      = env_float("NAVMESH_AGENT_HEIGHT", g_agentHeight);
    g_agentRadius      = env_float("NAVMESH_AGENT_RADIUS", g_agentRadius);
    g_agentMaxClimb    = env_float("NAVMESH_AGENT_MAX_CLIMB", g_agentMaxClimb);
    g_edgeMaxLen       = env_float("NAVMESH_EDGE_MAX_LEN", g_edgeMaxLen);
    g_edgeMaxError     = env_float("NAVMESH_EDGE_MAX_ERROR", g_edgeMaxError);
    g_detailSampleDist = env_float("NAVMESH_DETAIL_SAMPLE_DIST", g_detailSampleDist);
    g_detailSampleErr  = env_float("NAVMESH_DETAIL_SAMPLE_MAX_ERROR", g_detailSampleErr);
    g_tileSize         = env_int("NAVMESH_TILE_SIZE", g_tileSize);
    int threadCount    = env_int("NAVMESH_THREADS", 0);
    unsigned int hwThreads = std::thread::hardware_concurrency();
    if (threadCount <= 0) threadCount = hwThreads > 0 ? (int)hwThreads : 1;
    if (threadCount < 1) threadCount = 1;

    printf("navmesh-builder: cfg cellSize=%.3f cellHeight=%.3f edgeMaxLen=%.3f "
           "edgeMaxError=%.3f detailSampleDist=%.3f detailSampleErr=%.3f threads=%d\n",
           g_cellSize,
           g_cellHeight,
           g_edgeMaxLen,
           g_edgeMaxError,
           g_detailSampleDist,
           g_detailSampleErr,
           threadCount);

    // ── Collect collision triangles ────────────────────────────────────────
    printf("navmesh-builder: collecting triangles from %s\n", inputPath);
    TriSoup soup;
    collect_glb_triangles(inputPath, soup, false);

    for (const auto& obsPath : obstaclePaths) {
        collect_glb_triangles(obsPath.c_str(), soup, true);
    }

    if (soup.verts.empty()) {
        printf("navmesh-builder: no triangles found, skipping\n");
        return 0;
    }

    // ── Calculate overall bounds ───────────────────────────────────────────
    float bmin[3], bmax[3];
    rcCalcBounds(soup.verts.data(), (int)(soup.verts.size() / 3), bmin, bmax);

    const char* clipEnv = getenv("NAVMESH_BOUNDS");
    if (clipEnv) {
        float clipMin[3], clipMax[3];
        if (sscanf(clipEnv,
                   "%f,%f,%f,%f,%f,%f",
                   &clipMin[0],
                   &clipMin[1],
                   &clipMin[2],
                   &clipMax[0],
                   &clipMax[1],
                   &clipMax[2]) == 6) {
            rcVmax(bmin, clipMin);
            rcVmin(bmax, clipMax);
            printf("  clipped bounds to NAVMESH_BOUNDS\n");
        }
    }

    printf("  scene bounds: (%.1f, %.1f, %.1f) - (%.1f, %.1f, %.1f)\n",
           bmin[0],
           bmin[1],
           bmin[2],
           bmax[0],
           bmax[1],
           bmax[2]);
    printf("  %zu vertices, %zu triangles\n", soup.verts.size() / 3, soup.indices.size() / 3);

    // ── Tile grid ──────────────────────────────────────────────────────────
    float tileWorldSize = g_cellSize * g_tileSize;

    int tw = (int)ceilf((bmax[0] - bmin[0]) / tileWorldSize);
    int th = (int)ceilf((bmax[2] - bmin[2]) / tileWorldSize);
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;

    printf("  tile grid: %d x %d (%d tiles), tileSize=%d, tileWorldSize=%.1f\n",
           tw,
           th,
           tw * th,
           g_tileSize,
           tileWorldSize);

    // ── Build tiles ────────────────────────────────────────────────────────
    struct TileResult {
        int tx, ty;
        std::vector<u8> data;
    };

    std::vector<TileResult> tileResults((size_t)tw * (size_t)th);
    std::vector<unsigned char> tileBuilt((size_t)tw * (size_t)th, 0);

    std::atomic<int> nextTile{0};
    std::atomic<int> builtCount{0};
    std::atomic<int> emptyCount{0};
    std::atomic<int> filteredCount{0};
    static const int MIN_POLYS_PER_TILE = 1;  // disabled — keep all tiles for connectivity

    int totalTiles = tw * th;
    if (threadCount > totalTiles) threadCount = totalTiles;

    printf("  binning triangles to tiles... ");
    fflush(stdout);
    std::vector<std::vector<uint32_t>> tileTris((size_t)totalTiles);
    bin_triangles_to_tiles(soup, bmin, bmax, tileWorldSize, tw, th, tileTris);
    size_t totalTileTriRefs = 0;
    for (const auto& tris : tileTris) totalTileTriRefs += tris.size();
    printf("%zu refs\n", totalTileTriRefs);

    auto buildWorker = [&]() {
        for (;;) {
            int tileIndex = nextTile.fetch_add(1, std::memory_order_relaxed);
            if (tileIndex >= totalTiles) break;

            int tx = tileIndex % tw;
            int ty = tileIndex / tw;

            float tbmin[3], tbmax[3];
            tbmin[0] = bmin[0] + tx * tileWorldSize;
            tbmin[1] = bmin[1];
            tbmin[2] = bmin[2] + ty * tileWorldSize;
            tbmax[0] = tbmin[0] + tileWorldSize;
            tbmax[1] = bmax[1];
            tbmax[2] = tbmin[2] + tileWorldSize;

            TriSoup clipped = make_tile_soup(soup, tileTris[(size_t)tileIndex]);
            if (clipped.verts.empty()) {
                emptyCount.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            TileResult result;
            result.tx = tx;
            result.ty = ty;
            if (build_tile(clipped, tbmin, tbmax, g_cellSize, tx, ty, result.data)) {
                int npolys = 0;
                if (result.data.size() >= 28) {
                    memcpy(&npolys, result.data.data() + 24, 4);
                }
                // Filter out tiny artifact tiles that create isolated navmesh islands.
                // Tiles with < MIN_POLYS_PER_TILE polygons are almost always border
                // padding artifacts that break connectivity.
                if (npolys < MIN_POLYS_PER_TILE) {
                    filteredCount.fetch_add(1, std::memory_order_relaxed);
                    printf("  tile (%d,%d): filtered (%d polys, need >= %d)\n",
                           tx,
                           ty,
                           npolys,
                           MIN_POLYS_PER_TILE);
                    continue;
                }
                tileResults[(size_t)tileIndex] = std::move(result);
                tileBuilt[(size_t)tileIndex] = 1;
                builtCount.fetch_add(1, std::memory_order_relaxed);
                // printf("  tile (%d,%d): %zu bytes, %d polys\n",
                //        tx,
                //        ty,
                //        tileResults[(size_t)tileIndex].data.size(),
                //        npolys);
            } else {
                emptyCount.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::vector<std::thread> workers;
    workers.reserve((size_t)threadCount);
    for (int i = 0; i < threadCount; i++) {
        workers.emplace_back(buildWorker);
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    std::vector<TileResult> tiles;
    tiles.reserve((size_t)builtCount.load(std::memory_order_relaxed));
    for (int i = 0; i < totalTiles; i++) {
        if (tileBuilt[(size_t)i]) {
            tiles.push_back(std::move(tileResults[(size_t)i]));
        }
    }

    printf("  built %d tiles, %d empty, %d filtered (too few polys)\n",
           builtCount.load(std::memory_order_relaxed),
           emptyCount.load(std::memory_order_relaxed),
           filteredCount.load(std::memory_order_relaxed));

    if (tiles.empty()) {
        printf("navmesh-builder: no tiles generated\n");
        return 1;
    }

    // ── Write output ───────────────────────────────────────────────────────
    FILE* out = fopen(outputPath, "wb");
    if (!out) {
        fprintf(stderr, "navmesh-builder: cannot open %s for writing\n", outputPath);
        return 1;
    }

    fwrite(MAGIC, 1, 4, out);
    write_u32(out, VERSION);
    fwrite(bmin, sizeof(float), 3, out);
    fwrite(bmax, sizeof(float), 3, out);
    write_u32(out, (u32)tiles.size());
    fwrite(&tileWorldSize, sizeof(float), 1, out);

    size_t totalBytes = 0;
    for (const auto& tile : tiles) {
        write_u32(out, (u32)tile.data.size());
        fwrite(tile.data.data(), 1, tile.data.size(), out);
        totalBytes += tile.data.size();
    }
    fclose(out);

    printf("navmesh-builder: wrote %zu bytes (%d tiles) to %s\n",
           totalBytes,
           (int)tiles.size(),
           outputPath);

    return 0;
}
