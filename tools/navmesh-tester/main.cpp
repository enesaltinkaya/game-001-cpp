/**
 * navmesh-tester: Comprehensive navmesh diagnostics tool.
 *
 * Loads a .nav.dat file (zstd-compressed or raw) and runs diagnostics:
 *   - Tile/polygon statistics
 *   - Connectivity analysis (connected components)
 *   - Grid coverage probe (find gaps)
 *   - OBJ export (visualize in Blender/meshlab)
 *   - Path queries
 *
 * Usage:
 *   navmesh-tester <file.nav.dat> [mode] [args...]
 *
 * Modes:
 *   stats                  Print detailed tile/polygon stats (default)
 *   coverage [step]        Grid coverage probe (default step=20)
 *   obj <output.obj>       Export navmesh polygons to OBJ file
 *   path <sx sy sz> <ex ey ez>
 *                          Find path between two points
 *   probe <x> <y> <z>      Closest-point + Y-sweep at position
 *   connectivity           Analyze connected components
 *   full [step]            Stats + connectivity + coverage
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <unordered_map>

#include <zstd.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t u8;

// ── Direct Detour access (bypass C API for inspection) ────────────────────
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "DetourNavMeshBuilder.h"
#include "DetourCommon.h"

// Also use the C API for loading
#include "recast_c_api.h"

// Access the internals via the known struct layout
struct RcNavMesh       { dtNavMesh* navMesh; };
struct DtNavMeshQuery  { dtNavMeshQuery* query; const dtNavMesh* navMesh; };

static const dtNavMesh* unwrapNavMesh(const RcNavMesh* m) { return m->navMesh; }
static dtNavMeshQuery*  unwrapQuery(const DtNavMeshQuery* q) { return q->query; }

// ── File I/O ───────────────────────────────────────────────────────────────

static std::vector<u8> loadFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "failed to open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<u8> buf(size);
    fread(buf.data(), 1, size, f);
    fclose(f);
    return buf;
}

static std::vector<u8> decompressZstd(const std::vector<u8>& compressed) {
    u64 rSize = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
    if (rSize == ZSTD_CONTENTSIZE_ERROR || rSize == ZSTD_CONTENTSIZE_UNKNOWN)
        return compressed;  // not compressed
    std::vector<u8> buf(rSize);
    u64 dSize = ZSTD_decompress(buf.data(), rSize, compressed.data(), compressed.size());
    if (ZSTD_isError(dSize)) { fprintf(stderr, "zstd: decompression failed\n"); exit(1); }
    buf.resize(dSize);
    return buf;
}

// ── Binary parsing ─────────────────────────────────────────────────────────

static u32 read_u32(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }
static float read_float(const u8* p) { float v; memcpy(&v, p, 4); return v; }

struct NavFile {
    float bmin[3], bmax[3];
    float tileWorldSize;
    u32 version;
    u32 tileCount;
    std::vector<const u8*> tilePtrs;
    std::vector<u32> tileSizes;
    std::vector<u8> data;
};

static NavFile parseNavFile(const char* path) {
    NavFile nf = {};
    std::vector<u8> fileData = loadFile(path);
    nf.data = decompressZstd(fileData);

    printf("loaded %s: %zu bytes (compressed: %zu)\n",
           path, nf.data.size(), fileData.size());

    if (nf.data.size() < 32 || memcmp(nf.data.data(), "NAVM", 4) != 0) {
        fprintf(stderr, "invalid navmesh header\n"); exit(1);
    }

    const u8* p = nf.data.data();
    const u8* end = p + nf.data.size();

    nf.version = read_u32(p + 4);
    for (int i = 0; i < 3; i++) nf.bmin[i] = read_float(p + 8 + i * 4);
    for (int i = 0; i < 3; i++) nf.bmax[i] = read_float(p + 20 + i * 4);
    p += 32;

    if (nf.version == 2) {
        nf.tileCount = read_u32(p); p += 4;
        nf.tileWorldSize = read_float(p); p += 4;
        nf.tilePtrs.resize(nf.tileCount);
        nf.tileSizes.resize(nf.tileCount);
        for (u32 i = 0; i < nf.tileCount; i++) {
            u32 ts = read_u32(p); p += 4;
            nf.tilePtrs[i] = p;
            nf.tileSizes[i] = ts;
            p += ts;
        }
    } else {
        nf.tileCount = 1;
        nf.tileWorldSize = nf.bmax[0] - nf.bmin[0];
        nf.tilePtrs.resize(1);
        nf.tileSizes.resize(1);
        u32 ts = read_u32(p); p += 4;
        nf.tilePtrs[0] = p;
        nf.tileSizes[0] = ts;
    }

    printf("  version: %u\n", nf.version);
    printf("  bounds: (%.1f, %.1f, %.1f) - (%.1f, %.1f, %.1f)\n",
           nf.bmin[0], nf.bmin[1], nf.bmin[2],
           nf.bmax[0], nf.bmax[1], nf.bmax[2]);
    printf("  tiles: %u, tileWorldSize: %.1f\n\n", nf.tileCount, nf.tileWorldSize);
    return nf;
}

static RcNavMesh* loadNavMesh(const NavFile& nf) {
    if (nf.version == 2) {
        // Cast away const for C API compatibility
    std::vector<const void*> ptrs(nf.tileCount);
    for (u32 i = 0; i < nf.tileCount; i++) ptrs[i] = nf.tilePtrs[i];
    return rcNavMeshLoadTiled(ptrs.data(), nf.tileSizes.data(),
                                  nf.tileCount, nf.bmin, nf.bmax,
                                  nf.tileWorldSize, nf.tileWorldSize);
    }
    return rcNavMeshLoad(nf.tilePtrs[0], nf.tileSizes[0]);
}

// ── Helper: iterate all links from a polygon ──────────────────────────────

struct LinkIter {
    const dtMeshTile* tile;
    unsigned int idx;

    bool valid() const { return idx != DT_NULL_LINK; }
    const dtLink& link() const { return tile->links[idx]; }
    void next() { idx = link().next; }
};

static LinkIter firstLink(const dtMeshTile* tile, const dtPoly* poly) {
    return {tile, poly->firstLink};
}

// ── Stats mode ─────────────────────────────────────────────────────────────

static void cmdStats(const NavFile& nf, RcNavMesh* mesh) {
    const dtNavMesh* nav = unwrapNavMesh(mesh);

    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  NAVMESH STATISTICS                                         ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    int totalPolys = 0, totalVerts = 0, totalDetVerts = 0, totalDetTris = 0;
    int tilesWithData = 0;
    float globalYMin = 1e9f, globalYMax = -1e9f;

    struct TileInfo {
        int index, polys, verts, detVerts, detTris;
        float bmin[3], bmax[3], yMin, yMax;
        u32 dataSize;
        int tileX, tileY;
    };
    std::vector<TileInfo> tiles;

    for (int ti = 0; ti < nav->getMaxTiles(); ti++) {
        const dtMeshTile* tile = nav->getTile(ti);
        if (!tile || !tile->header) continue;

        TileInfo info = {};
        info.index     = ti;
        info.polys     = tile->header->polyCount;
        info.verts     = tile->header->vertCount;
        info.detVerts  = tile->header->detailVertCount;
        info.detTris   = tile->header->detailTriCount;
        info.dataSize  = (u32)tile->dataSize;
        info.tileX     = tile->header->x;
        info.tileY     = tile->header->y;
        dtVcopy(info.bmin, tile->header->bmin);
        dtVcopy(info.bmax, tile->header->bmax);

        info.yMin = 1e9f; info.yMax = -1e9f;
        for (int vi = 0; vi < tile->header->vertCount; vi++) {
            float y = tile->verts[vi * 3 + 1];
            if (y < info.yMin) info.yMin = y;
            if (y > info.yMax) info.yMax = y;
        }

        totalPolys += info.polys;
        totalVerts += info.verts;
        totalDetVerts += info.detVerts;
        totalDetTris += info.detTris;
        tilesWithData++;
        if (info.yMin < globalYMin) globalYMin = info.yMin;
        if (info.yMax > globalYMax) globalYMax = info.yMax;

        tiles.push_back(info);
    }

    int tw = (int)ceilf((nf.bmax[0] - nf.bmin[0]) / nf.tileWorldSize);
    int th = (int)ceilf((nf.bmax[2] - nf.bmin[2]) / nf.tileWorldSize);

    printf("Scene bounds: (%.0f, %.0f, %.0f) - (%.0f, %.0f, %.0f)\n",
           nf.bmin[0], nf.bmin[1], nf.bmin[2], nf.bmax[0], nf.bmax[1], nf.bmax[2]);
    printf("Scene size:   %.0f x %.0f x %.0f\n",
           nf.bmax[0]-nf.bmin[0], nf.bmax[1]-nf.bmin[1], nf.bmax[2]-nf.bmin[2]);
    printf("Tile grid:    %d x %d = %d tiles\n", tw, th, tw*th);
    printf("Tile size:    %.1f world units\n\n", nf.tileWorldSize);

    // Per-tile table
    printf("Per-tile details:\n");
    printf("  %-6s %-6s %-8s %-8s %-10s %-10s %-10s %-16s\n",
           "Tile", "Grid", "Polys", "Verts", "DetV", "DetT", "Size", "Y range");
    printf("  %-6s %-6s %-8s %-8s %-10s %-10s %-10s %-16s\n",
           "────", "────", "─────", "─────", "────", "────", "────", "───────");

    for (const auto& t : tiles) {
        printf("  %-6d %d,%-4d %-8d %-8d %-10d %-10d %-10u [%.0f,%.0f]\n",
               t.index, t.tileX, t.tileY,
               t.polys, t.verts, t.detVerts, t.detTris, t.dataSize,
               t.yMin, t.yMax);
    }

    // Tile heat map
    printf("\nTile polygon heat map (%dx%d grid):\n", tw, th);
    printf("  Each cell = polygon count. '.' = 0 (empty tile).\n\n");

    std::unordered_map<u64, int> gridMap;
    for (const auto& t : tiles) {
        u64 key = ((u64)t.tileX << 32) | (u64)t.tileY;
        gridMap[key] = t.polys;
    }

    printf("       ");
    for (int tx = 0; tx < tw; tx++) printf("%-8d", tx);
    printf("\n");

    for (int ty = 0; ty < th; ty++) {
        printf("  ty%-2d ", ty);
        for (int tx = 0; tx < tw; tx++) {
            u64 key = ((u64)tx << 32) | (u64)ty;
            int p = 0;
            auto it = gridMap.find(key);
            if (it != gridMap.end()) p = it->second;
            if (p == 0) printf("  .     ");
            else printf("  %-6d", p);
        }
        printf("\n");
    }

    // Summary
    printf("\nSummary:\n");
    printf("  Tiles with data: %d / %d\n", tilesWithData, nav->getMaxTiles());
    printf("  Total polygons:  %d\n", totalPolys);
    printf("  Total vertices:  %d\n", totalVerts);
    printf("  Detail verts:    %d\n", totalDetVerts);
    printf("  Detail tris:     %d\n", totalDetTris);
    printf("  Y range:         [%.1f, %.1f]\n", globalYMin, globalYMax);

    // Debug triangle estimate
    int debugTriCount = 0;
    for (int ti = 0; ti < nav->getMaxTiles(); ti++) {
        const dtMeshTile* tile = nav->getTile(ti);
        if (!tile || !tile->header) continue;
        for (int pi = 0; pi < tile->header->polyCount; pi++) {
            const dtPoly* poly = &tile->polys[pi];
            if (poly->getType() == DT_POLYTYPE_GROUND)
                debugTriCount += poly->vertCount - 2;
        }
    }
    printf("  Debug triangles: %d (visualization fan triangulation)\n", debugTriCount);
    if (debugTriCount > 65536) {
        printf("  ⚠  EXCEEDS MAX_DEBUG_TRIANGLES=65536 — visualization truncated!\n");
        printf("     Increase to at least %d.\n", debugTriCount + debugTriCount/10);
    }
}

// ── Coverage mode ──────────────────────────────────────────────────────────

static void cmdCoverage(const NavFile& nf, RcNavMesh* mesh, float step) {
    const dtNavMesh* nav = unwrapNavMesh(mesh);
    DtNavMeshQuery* q = rcQueryCreate(mesh);
    if (!q) { fprintf(stderr, "failed to create query\n"); return; }
    dtNavMeshQuery* query = unwrapQuery(q);

    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  GRID COVERAGE PROBE (step=%.0f)                             ║\n", step);
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    const float halfExtents[3] = {2.0f, 32.0f, 2.0f};
    dtQueryFilter filter;
    filter.setIncludeFlags(0xffff);
    filter.setExcludeFlags(0);

    // Use a Y in the middle of the actual navmesh, not the scene bounds.
    // First, scan the navmesh tiles for the actual Y range.
    float navYMin = 1e9f, navYMax = -1e9f;
    for (int ti = 0; ti < nav->getMaxTiles(); ti++) {
        const dtMeshTile* tile = nav->getTile(ti);
        if (!tile || !tile->header || tile->header->polyCount == 0) continue;
        for (int vi = 0; vi < tile->header->vertCount; vi++) {
            float y = tile->verts[vi * 3 + 1];
            if (y < navYMin) navYMin = y;
            if (y > navYMax) navYMax = y;
        }
    }
    float probeY = (navYMin + navYMax) / 2.0f;
    printf("Probing (%.0f,_,%.0f)-(%0.f,_,%.0f) Y=%.0f step=%.0f\n\n",
           nf.bmin[0], nf.bmin[2], nf.bmax[0], nf.bmax[2], probeY, step);

    // ASCII coverage map
    int cols = 64;
    float aspect = (nf.bmax[2] - nf.bmin[2]) / (nf.bmax[0] - nf.bmin[0]);
    int rows = (int)(cols * aspect);
    if (rows < 10) rows = 10;
    if (rows > 50) rows = 50;

    int hitCount = 0, missCount = 0;

    printf("  '#' = hit, '.' = miss\n\n");
    printf("  Z min=%.0f\n", nf.bmin[2]);

    for (int r = 0; r < rows; r++) {
        float z = nf.bmin[2] + (nf.bmax[2] - nf.bmin[2]) * (r + 0.5f) / rows;
        printf("  ");
        for (int c = 0; c < cols; c++) {
            float x = nf.bmin[0] + (nf.bmax[0] - nf.bmin[0]) * (c + 0.5f) / cols;
            float pos[3] = {x, probeY, z};
            float nearest[3];
            dtPolyRef ref = 0;
            query->findNearestPoly(pos, halfExtents, &filter, &ref, nearest);
            if (ref) { hitCount++; printf("#"); }
            else     { missCount++; printf("."); }
        }
        printf("\n");
    }
    printf("  Z max=%.0f\n", nf.bmax[2]);
    printf("  X min=%.0f →→→ X max=%.0f\n\n", nf.bmin[0], nf.bmax[0]);

    int total = hitCount + missCount;
    printf("Coverage: %d/%d = %.1f%% hit, %d miss\n\n",
           hitCount, total, total ? (float)hitCount/total*100 : 0, missCount);

    // Quantitative grid probe at the requested step
    printf("Quantitative probe (step=%.0f):\n", step);
    int qHit = 0, qMiss = 0;
    struct Gap { float x, z; };
    std::vector<Gap> gaps;

    for (float z = nf.bmin[2]; z <= nf.bmax[2]; z += step) {
        for (float x = nf.bmin[0]; x <= nf.bmax[0]; x += step) {
            float pos[3] = {x, probeY, z};
            float nearest[3];
            dtPolyRef ref = 0;
            query->findNearestPoly(pos, halfExtents, &filter, &ref, nearest);
            if (ref) qHit++;
            else { qMiss++; gaps.push_back({x, z}); }
        }
    }
    int qTotal = qHit + qMiss;
    printf("  Probes: %d, Hit: %d, Miss: %d, Coverage: %.1f%%\n",
           qTotal, qHit, qMiss, qTotal ? (float)qHit/qTotal*100 : 0);

    if (!gaps.empty()) {
        printf("  Gap samples (first 30):\n");
        for (size_t i = 0; i < gaps.size() && i < 30; i++)
            printf("    (%.0f, ?, %.0f)\n", gaps[i].x, gaps[i].z);
        if (gaps.size() > 30) printf("    ... and %zu more\n", gaps.size() - 30);
    }

    rcQueryDestroy(q);
}

// ── OBJ export ─────────────────────────────────────────────────────────────

static void cmdObj(const NavFile& nf, RcNavMesh* mesh, const char* outPath) {
    const dtNavMesh* nav = unwrapNavMesh(mesh);

    printf("Exporting navmesh to OBJ: %s\n", outPath);

    FILE* f = fopen(outPath, "w");
    if (!f) { fprintf(stderr, "failed to open %s\n", outPath); return; }

    fprintf(f, "# NavMesh OBJ export\n");
    fprintf(f, "# Bounds: (%.0f,%.0f,%.0f) - (%.0f,%.0f,%.0f)\n\n",
            nf.bmin[0], nf.bmin[1], nf.bmin[2],
            nf.bmax[0], nf.bmax[1], nf.bmax[2]);

    int vertOffset = 0;
    int totalFaces = 0;
    int tileIdx = 0;

    for (int ti = 0; ti < nav->getMaxTiles(); ti++) {
        const dtMeshTile* tile = nav->getTile(ti);
        if (!tile || !tile->header) continue;
        if (tile->header->polyCount == 0) continue;

        fprintf(f, "o tile_%d_%d\n", tile->header->x, tile->header->y);

        // Write detail mesh vertices for higher accuracy
        // Detour stores: base verts first, then detail verts appended after
        // tile->verts has the base polygon vertices
        // tile->detailVerts has additional detail vertices (indexed from detailMeshes)
        // For simplicity, write base verts first, then detail verts

        // Write base vertices
        for (int vi = 0; vi < tile->header->vertCount; vi++) {
            const float* v = &tile->verts[vi * 3];
            fprintf(f, "v %.4f %.4f %.4f\n", v[0], v[1], v[2]);
        }

        // Triangulate polygons as fans
        for (int pi = 0; pi < tile->header->polyCount; pi++) {
            const dtPoly* poly = &tile->polys[pi];
            if (poly->getType() != DT_POLYTYPE_GROUND) continue;

            int nv = poly->vertCount;
            for (int j = 2; j < nv; j++) {
                fprintf(f, "f %d %d %d\n",
                        vertOffset + poly->verts[0] + 1,
                        vertOffset + poly->verts[j-1] + 1,
                        vertOffset + poly->verts[j] + 1);
                totalFaces++;
            }
        }

        // Write detail mesh triangles for better surface accuracy
        // Each detail mesh entry: [vertCount, triCount, baseVertIdx, baseTriIdx]
        // We only output base poly faces above; for full detail, we'd need
        // to also output detailVerts and detailTris. Skip for now — the
        // polygon-level mesh is sufficient for verifying coverage.

        vertOffset += tile->header->vertCount;
        tileIdx++;
    }

    fclose(f);
    printf("  Written: %d tiles, %d faces\n", tileIdx, totalFaces);
    printf("  → %s\n", outPath);
    printf("  Open in Blender: File → Import → Wavefront (.obj)\n");
}

// ── Connectivity analysis ──────────────────────────────────────────────────

static void cmdConnectivity(RcNavMesh* mesh) {
    const dtNavMesh* nav = unwrapNavMesh(mesh);

    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  CONNECTIVITY ANALYSIS                                      ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    // Build global polygon index
    struct PGlob { int tileIdx; int polyIdx; dtPolyRef ref; };
    std::vector<PGlob> allPolys;
    std::unordered_map<dtPolyRef, int> refToIdx;

    for (int ti = 0; ti < nav->getMaxTiles(); ti++) {
        const dtMeshTile* tile = nav->getTile(ti);
        if (!tile || !tile->header) continue;
        for (int pi = 0; pi < tile->header->polyCount; pi++) {
            dtPolyRef ref = nav->getPolyRefBase(tile) + (dtPolyRef)pi;
            int idx = (int)allPolys.size();
            allPolys.push_back({ti, pi, ref});
            refToIdx[ref] = idx;
        }
    }

    int internalEdges = 0, externalEdges = 0, offMesh = 0;

    for (auto& pg : allPolys) {
        const dtMeshTile* tile = nav->getTile(pg.tileIdx);
        const dtPoly* poly = &tile->polys[pg.polyIdx];

        if (poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION) { offMesh++; continue; }

        // Internal neighbors (via neis[] array)
        for (int e = 0; e < poly->vertCount; e++) {
            if (poly->neis[e] != 0) internalEdges++;
        }

        // External links (tile-to-tile, via links list)
        for (auto li = firstLink(tile, poly); li.valid(); li.next()) {
            if (li.link().side != 0) externalEdges++;
        }
    }

    printf("Total polygons:       %zu\n", allPolys.size());
    printf("Internal edges:       %d (within tiles)\n", internalEdges);
    printf("External edges:       %d (between tiles)\n", externalEdges);
    printf("Off-mesh connections: %d\n\n", offMesh);

    // BFS connected components
    std::vector<int> comp(allPolys.size(), -1);
    std::vector<int> compSizes;

    for (int i = 0; i < (int)allPolys.size(); i++) {
        if (comp[i] != -1) continue;
        int cid = (int)compSizes.size();

        std::vector<int> queue;
        queue.push_back(i);
        comp[i] = cid;
        int head = 0;

        while (head < (int)queue.size()) {
            int ci = queue[head++];
            const auto& pg = allPolys[ci];
            const dtMeshTile* tile = nav->getTile(pg.tileIdx);
            const dtPoly* poly = &tile->polys[pg.polyIdx];

            auto enqueue = [&](dtPolyRef ref) {
                auto it = refToIdx.find(ref);
                if (it != refToIdx.end() && comp[it->second] == -1) {
                    comp[it->second] = cid;
                    queue.push_back(it->second);
                }
            };

            // Internal neighbors
            for (int e = 0; e < poly->vertCount; e++) {
                if (poly->neis[e] != 0)
                    enqueue(nav->getPolyRefBase(tile) + (dtPolyRef)(poly->neis[e] - 1));
            }

            // External links
            for (auto li = firstLink(tile, poly); li.valid(); li.next())
                enqueue(li.link().ref);
        }

        compSizes.push_back((int)queue.size());
    }

    // Sort by size descending
    std::vector<int> order(compSizes.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return compSizes[a] > compSizes[b];
    });

    printf("Connected components: %zu\n", compSizes.size());
    for (size_t i = 0; i < order.size() && i < 10; i++) {
        float pct = (float)compSizes[order[i]] / allPolys.size() * 100.0f;
        printf("  Component %zu: %d polygons (%.1f%%)\n", i+1, compSizes[order[i]], pct);
    }

    if (compSizes.size() == 1) {
        printf("\n  ✓ Navmesh is fully connected.\n");
    } else {
        printf("\n  ⚠  %zu disconnected components!\n", compSizes.size());
        printf("  AI agents cannot path between components.\n");
        printf("  Largest component: %d polygons (%.1f%%)\n",
               compSizes[order[0]],
               (float)compSizes[order[0]]/allPolys.size()*100);

        // Find where small components are located
        printf("\n  Small component locations:\n");
        for (size_t ci = 1; ci < order.size() && ci < 6; ci++) {
            int cid = order[ci];
            float cx = 0, cy = 0, cz = 0;
            int cnt = 0;
            for (size_t j = 0; j < allPolys.size(); j++) {
                if (comp[j] != cid) continue;
                const dtMeshTile* tile = nav->getTile(allPolys[j].tileIdx);
                const dtPoly* poly = &tile->polys[allPolys[j].polyIdx];
                for (int v = 0; v < poly->vertCount; v++) {
                    const float* vt = &tile->verts[poly->verts[v]*3];
                    cx += vt[0]; cy += vt[1]; cz += vt[2];
                    cnt++;
                }
            }
            if (cnt > 0)
                printf("    Component %zu (%d polys): center=(%.0f, %.0f, %.0f)\n",
                       ci+1, compSizes[cid], cx/cnt, cy/cnt, cz/cnt);
        }
    }
}

// ── Pathfinding mode ───────────────────────────────────────────────────────

static void cmdPath(RcNavMesh* mesh, float sx, float sy, float sz,
                    float ex, float ey, float ez) {
    DtNavMeshQuery* q = rcQueryCreate(mesh);
    if (!q) { fprintf(stderr, "failed to create query\n"); return; }

    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  PATH QUERY                                                 ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    float start[] = {sx, sy, sz};
    float end[]   = {ex, ey, ez};
    printf("Start: (%.1f, %.1f, %.1f)\n", sx, sy, sz);
    printf("End:   (%.1f, %.1f, %.1f)\n", ex, ey, ez);
    printf("Dist:  %.1f\n\n", dtVdist(start, end));

    float cs[3], ce[3];
    printf("Closest to start: ");
    if (rcQueryClosestPoint(q, start, cs))
        printf("(%.1f, %.1f, %.1f) dist=%.2f\n", cs[0], cs[1], cs[2], dtVdist(start, cs));
    else
        printf("FAIL\n");

    printf("Closest to end:   ");
    if (rcQueryClosestPoint(q, end, ce))
        printf("(%.1f, %.1f, %.1f) dist=%.2f\n", ce[0], ce[1], ce[2], dtVdist(end, ce));
    else
        printf("FAIL\n");

    printf("\n");
    float outPath[256 * 3];
    uint32_t n = rcQueryFindPath(q, start, end, outPath, 256);
    if (n > 0) {
        float len = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (i > 0) {
                float d = dtVdist(&outPath[i*3], &outPath[(i-1)*3]);
                len += d;
            }
            printf("  [%2u] (%7.1f, %6.1f, %7.1f)\n", i, outPath[i*3], outPath[i*3+1], outPath[i*3+2]);
        }
        printf("  Total: %.1f, straight: %.1f, ratio: %.2f\n", len, dtVdist(start, end),
               dtVdist(start,end) > 0 ? len/dtVdist(start,end) : 0);
    } else {
        printf("  NO PATH FOUND\n");
    }
    rcQueryDestroy(q);
}

// ── Probe mode ─────────────────────────────────────────────────────────────

static void cmdProbe(RcNavMesh* mesh, float x, float y, float z) {
    DtNavMeshQuery* q = rcQueryCreate(mesh);
    if (!q) { fprintf(stderr, "failed to create query\n"); return; }

    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  POINT PROBE: (%.1f, %.1f, %.1f)                            ║\n", x, y, z);
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    printf("Y sweep (all navmesh surfaces at XZ):\n");
    for (float py = 0; py <= 900; py += 10) {
        float pos[3] = {x, py, z};
        float nearest[3];
        if (rcQueryClosestPoint(q, pos, nearest)) {
            float d = dtVdist(pos, nearest);
            if (d < 10.0f)
                printf("  Y=%6.0f -> (%.1f, %.1f, %.1f) dist=%.2f\n", py, nearest[0], nearest[1], nearest[2], d);
        }
    }

    float pos[3] = {x, y, z};
    float nearest[3];
    printf("\nClosest: ");
    if (rcQueryClosestPoint(q, pos, nearest))
        printf("(%.1f, %.1f, %.1f) dist=%.2f\n", nearest[0], nearest[1], nearest[2], dtVdist(pos, nearest));
    else
        printf("FAIL\n");

    rcQueryDestroy(q);
}

// ── Main ───────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage:\n");
        printf("  %s <file.nav.dat> [mode] [args...]\n\n", argv[0]);
        printf("Modes:\n");
        printf("  stats              Tile/polygon stats (default)\n");
        printf("  coverage [step]    Grid coverage probe\n");
        printf("  obj <out.obj>      Export to OBJ\n");
        printf("  path <sx sy sz> <ex ey ez>\n");
        printf("  probe <x y z>      Point probe + Y-sweep\n");
        printf("  connectivity       Connected components\n");
        printf("  full [step]        All diagnostics\n");
        return 1;
    }

    NavFile nf = parseNavFile(argv[1]);
    RcNavMesh* mesh = loadNavMesh(nf);
    if (!mesh) { fprintf(stderr, "failed to load navmesh\n"); return 1; }

    std::string mode = argc >= 3 ? argv[2] : "stats";

    if (mode == "stats") {
        cmdStats(nf, mesh);
    } else if (mode == "coverage") {
        cmdCoverage(nf, mesh, argc >= 4 ? (float)atof(argv[3]) : 20.0f);
    } else if (mode == "obj") {
        if (argc < 4) { fprintf(stderr, "usage: ... obj <output.obj>\n"); return 1; }
        cmdObj(nf, mesh, argv[3]);
    } else if (mode == "path") {
        if (argc < 9) { fprintf(stderr, "usage: ... path sx sy sz ex ey ez\n"); return 1; }
        cmdPath(mesh, atof(argv[3]), atof(argv[4]), atof(argv[5]),
                       atof(argv[6]), atof(argv[7]), atof(argv[8]));
    } else if (mode == "probe") {
        if (argc < 6) { fprintf(stderr, "usage: ... probe x y z\n"); return 1; }
        cmdProbe(mesh, atof(argv[3]), atof(argv[4]), atof(argv[5]));
    } else if (mode == "connectivity") {
        cmdConnectivity(mesh);
    } else if (mode == "full") {
        cmdStats(nf, mesh);
        printf("\n");
        cmdConnectivity(mesh);
        printf("\n");
        cmdCoverage(nf, mesh, argc >= 4 ? (float)atof(argv[3]) : 20.0f);
    } else {
        fprintf(stderr, "unknown mode: %s\n", mode.c_str());
        return 1;
    }

    rcNavMeshDestroy(mesh);
    return 0;
}
