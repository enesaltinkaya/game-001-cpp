#include "azgaar/Delaunator.h"

#include <math.h>
#include <float.h>
#include "memorymanager/MemoryManager.h"

// Robust predicates are not available here; Azgaar's jittered grid points are
// well separated so plain double arithmetic is sufficient (this matches what
// d3-delaunay falls back to).  Convention: positive => counter-clockwise.
static inline double orient2d(double ax, double ay,
                              double bx, double by,
                              double cx, double cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

static inline double delaunatorDist(double ax, double ay, double bx, double by) {
    double dx = ax - bx;
    double dy = ay - by;
    return dx * dx + dy * dy;
}

// Monotonically increases with real angle, but doesn't need expensive trig.
static inline double pseudoAngle(double dx, double dy) {
    double denom = fabs(dx) + fabs(dy);
    if (denom == 0.0) return 0.0;
    double p = dx / denom;
    return (dy > 0.0 ? 3.0 - p : 1.0 + p) / 4.0;  // [0..1]
}

static double delaunatorCircumradius(double ax, double ay,
                                     double bx, double by,
                                     double cx, double cy) {
    double dx = bx - ax;
    double dy = by - ay;
    double ex = cx - ax;
    double ey = cy - ay;
    double bl = dx * dx + dy * dy;
    double cl = ex * ex + ey * ey;
    double denom = dx * ey - dy * ex;
    if (fabs(denom) < DBL_MIN) return HUGE_VAL;  // collinear
    double d = 0.5 / denom;
    double x = (ey * bl - dy * cl) * d;
    double y = (dx * cl - ex * bl) * d;
    return x * x + y * y;
}

static void delaunatorCircumcenter(double ax, double ay,
                                   double bx, double by,
                                   double cx, double cy,
                                   double* outX, double* outY) {
    double dx = bx - ax;
    double dy = by - ay;
    double ex = cx - ax;
    double ey = cy - ay;
    double bl = dx * dx + dy * dy;
    double cl = ex * ex + ey * ey;
    double denom = dx * ey - dy * ex;
    if (fabs(denom) < DBL_MIN) {  // collinear: bail to a point on the segment
        *outX = ax;
        *outY = ay;
        return;
    }
    double d = 0.5 / denom;
    *outX = ax + (ey * bl - dy * cl) * d;
    *outY = ay + (dx * cl - ex * bl) * d;
}

// Is point P inside the circumcircle of A,B,C (CCW)?  Matches delaunator.inCircle.
static inline bool inCircle(double ax, double ay,
                            double bx, double by,
                            double cx, double cy,
                            double px, double py) {
    double dx = ax - px;
    double dy = ay - py;
    double ex = bx - px;
    double ey = by - py;
    double fx = cx - px;
    double fy = cy - py;
    double ap = dx * dx + dy * dy;
    double bp = ex * ex + ey * ey;
    double cp = fx * fx + fy * fy;
    return (dx * (ey * cp - bp * fy) -
            dy * (ex * cp - bp * fx) +
            ap * (ex * fy - ey * fx)) < 0.0;
}

#define DELAUNATOR_EDGE_STACK 512

struct DelaunayCtx {
    const double* coords;
    u32 n;

    u32*  triangles;
    i32*  halfedges;
    u32   trianglesLen;

    u32*  hullPrev;
    u32*  hullNext;
    u32*  hullTri;
    i32*  hullHash;
    u32   hashSize;
    u32   hullStart;

    u32*    ids;
    double* dists;
    double  cx, cy;

    u32* hull;     // owned by the caller (out.hull), written here
    u32  hullSize;

    u32 edgeStack[DELAUNATOR_EDGE_STACK];
};

static void ctxLink(DelaunayCtx* c, u32 a, i32 b) {
    c->halfedges[a] = b;
    if (b != -1) c->halfedges[static_cast<u32>(b)] = static_cast<i32>(a);
}

static u32 ctxAddTriangle(DelaunayCtx* c, u32 i0, u32 i1, u32 i2, i32 a, i32 b, i32 cEdge) {
    u32 t = c->trianglesLen;
    c->triangles[t]     = i0;
    c->triangles[t + 1] = i1;
    c->triangles[t + 2] = i2;
    ctxLink(c, t, a);
    ctxLink(c, t + 1, b);
    ctxLink(c, t + 2, cEdge);
    c->trianglesLen += 3;
    return t;
}

static inline u32 ctxHashKey(const DelaunayCtx* c, double x, double y) {
    i32 k = static_cast<i32>(floor(pseudoAngle(x - c->cx, y - c->cy) * static_cast<double>(c->hashSize))) % static_cast<i32>(c->hashSize);
    return static_cast<u32>(k < 0 ? k + static_cast<i32>(c->hashSize) : k);
}

static u32 ctxLegalize(DelaunayCtx* c, u32 a) {
    u32  i  = 0;
    u32  ar = 0;

    while (true) {
        i32 b = c->halfedges[a];

        u32 a0 = a - (a % 3);
        ar = a0 + (a + 2) % 3;

        if (b == -1) {  // convex hull edge
            if (i == 0) break;
            a = c->edgeStack[--i];
            continue;
        }

        u32 b0 = static_cast<u32>(b) - static_cast<u32>(b) % 3;
        u32 al = a0 + (a + 1) % 3;
        u32 bl = b0 + (static_cast<u32>(b) + 2) % 3;

        u32 p0 = c->triangles[ar];
        u32 pr = c->triangles[a];
        u32 pl = c->triangles[al];
        u32 p1 = c->triangles[bl];

        bool illegal = inCircle(
            c->coords[2 * p0],     c->coords[2 * p0 + 1],
            c->coords[2 * pr],     c->coords[2 * pr + 1],
            c->coords[2 * pl],     c->coords[2 * pl + 1],
            c->coords[2 * p1],     c->coords[2 * p1 + 1]);

        if (illegal) {
            c->triangles[a] = p1;
            c->triangles[static_cast<u32>(b)] = p0;

            i32 hbl = c->halfedges[bl];

            // edge swapped on the other side of the hull (rare); fix the reference
            if (hbl == -1) {
                u32 e = c->hullStart;
                do {
                    if (c->hullTri[e] == bl) {
                        c->hullTri[e] = a;
                        break;
                    }
                    e = c->hullPrev[e];
                } while (e != c->hullStart);
            }
            ctxLink(c, a, hbl);
            ctxLink(c, static_cast<u32>(b), c->halfedges[ar]);
            ctxLink(c, ar, static_cast<i32>(bl));

            u32 br = b0 + (static_cast<u32>(b) + 1) % 3;
            if (i < DELAUNATOR_EDGE_STACK) c->edgeStack[i++] = br;
        } else {
            if (i == 0) break;
            a = c->edgeStack[--i];
        }
    }
    return ar;
}

// ---- quicksort over point ids by their precomputed distance ----

static void swapU32(u32* arr, u32 i, u32 j) {
    u32 tmp = arr[i];
    arr[i] = arr[j];
    arr[j] = tmp;
}

static void quicksortIds(u32* ids, const double* dists, i32 left, i32 right) {
    if (right - left <= 20) {
        for (i32 ii = left + 1; ii <= right; ii++) {
            u32 temp = ids[ii];
            double tempDist = dists[temp];
            i32 j = ii - 1;
            while (j >= left && dists[ids[j]] > tempDist) {
                ids[j + 1] = ids[j];
                j--;
            }
            ids[j + 1] = temp;
        }
    } else {
        i32 median = (left + right) >> 1;
        i32 i = left + 1;
        i32 j = right;
        swapU32(ids, static_cast<u32>(median), static_cast<u32>(i));
        if (dists[ids[left]] > dists[ids[right]]) swapU32(ids, static_cast<u32>(left), static_cast<u32>(right));
        if (dists[ids[i]] > dists[ids[right]]) swapU32(ids, static_cast<u32>(i), static_cast<u32>(right));
        if (dists[ids[left]] > dists[ids[i]]) swapU32(ids, static_cast<u32>(left), static_cast<u32>(i));

        u32 temp = ids[i];
        double tempDist = dists[temp];
        while (true) {
            do i++; while (dists[ids[i]] < tempDist);
            do j--; while (dists[ids[j]] > tempDist);
            if (j < i) break;
            swapU32(ids, static_cast<u32>(i), static_cast<u32>(j));
        }
        ids[left + 1] = ids[j];
        ids[j] = temp;

        if (right - i + 1 >= j - left) {
            quicksortIds(ids, dists, i, right);
            quicksortIds(ids, dists, left, j - 1);
        } else {
            quicksortIds(ids, dists, left, j - 1);
            quicksortIds(ids, dists, i, right);
        }
    }
}

static void delaunatorUpdate(DelaunayCtx* c) {
    u32 n = c->n;
    const double* coords = c->coords;

    double minX = DBL_MAX, minY = DBL_MAX;
    double maxX = -DBL_MAX, maxY = -DBL_MAX;
    for (u32 i = 0; i < n; i++) {
        double x = coords[2 * i];
        double y = coords[2 * i + 1];
        if (x < minX) minX = x;
        if (y < minY) minY = y;
        if (x > maxX) maxX = x;
        if (y > maxY) maxY = y;
        c->ids[i] = i;
    }
    double cx = (minX + maxX) * 0.5;
    double cy = (minY + maxY) * 0.5;
    c->cx = cx;
    c->cy = cy;

    u32 i0 = 0;
    {
        double minDist = DBL_MAX;
        for (u32 i = 0; i < n; i++) {
            double d = delaunatorDist(cx, cy, coords[2 * i], coords[2 * i + 1]);
            if (d < minDist) { i0 = i; minDist = d; }
        }
    }
    double i0x = coords[2 * i0];
    double i0y = coords[2 * i0 + 1];

    u32 i1 = 0;
    {
        double minDist = DBL_MAX;
        for (u32 i = 0; i < n; i++) {
            if (i == i0) continue;
            double d = delaunatorDist(i0x, i0y, coords[2 * i], coords[2 * i + 1]);
            if (d < minDist && d > 0.0) { i1 = i; minDist = d; }
        }
    }
    double i1x = coords[2 * i1];
    double i1y = coords[2 * i1 + 1];

    u32 i2 = 0;
    double minRadius = DBL_MAX;
    for (u32 i = 0; i < n; i++) {
        if (i == i0 || i == i1) continue;
        double r = delaunatorCircumradius(i0x, i0y, i1x, i1y, coords[2 * i], coords[2 * i + 1]);
        if (r < minRadius) { i2 = i; minRadius = r; }
    }

    if (minRadius == HUGE_VAL) {
        // All points collinear: order them and emit the unique ones as the hull.
        for (u32 i = 0; i < n; i++) {
            c->dists[i] = (coords[2 * i] - coords[0]);
            if (c->dists[i] == 0.0) c->dists[i] = coords[2 * i + 1] - coords[1];
        }
        quicksortIds(c->ids, c->dists, 0, static_cast<i32>(n) - 1);
        c->hullSize = 0;
        double d0 = -HUGE_VAL;
        for (u32 i = 0; i < n; i++) {
            u32 id  = c->ids[i];
            double d = c->dists[id];
            if (d > d0) { c->hull[c->hullSize++] = id; d0 = d; }
        }
        c->trianglesLen = 0;
        return;
    }

    double i2x = coords[2 * i2];
    double i2y = coords[2 * i2 + 1];

    // swap seed order for CCW orientation
    if (orient2d(i0x, i0y, i1x, i1y, i2x, i2y) < 0.0) {
        u32 ti = i1; double tx = i1x, ty = i1y;
        i1 = i2; i1x = i2x; i1y = i2y;
        i2 = ti; i2x = tx; i2y = ty;
    }

    double centerX, centerY;
    delaunatorCircumcenter(i0x, i0y, i1x, i1y, i2x, i2y, &centerX, &centerY);

    for (u32 i = 0; i < n; i++) {
        c->dists[i] = delaunatorDist(coords[2 * i], coords[2 * i + 1], centerX, centerY);
    }
    quicksortIds(c->ids, c->dists, 0, static_cast<i32>(n) - 1);

    c->hullStart = i0;
    u32 hullSize = 3;

    c->hullNext[i0] = c->hullPrev[i2] = i1;
    c->hullNext[i1] = c->hullPrev[i0] = i2;
    c->hullNext[i2] = c->hullPrev[i1] = i0;

    c->hullTri[i0] = 0;
    c->hullTri[i1] = 1;
    c->hullTri[i2] = 2;

    for (u32 i = 0; i < c->hashSize; i++) c->hullHash[i] = -1;
    c->hullHash[ctxHashKey(c, i0x, i0y)] = static_cast<i32>(i0);
    c->hullHash[ctxHashKey(c, i1x, i1y)] = static_cast<i32>(i1);
    c->hullHash[ctxHashKey(c, i2x, i2y)] = static_cast<i32>(i2);

    c->trianglesLen = 0;
    ctxAddTriangle(c, i0, i1, i2, -1, -1, -1);

    double xp = 0.0, yp = 0.0;
    for (u32 k = 0; k < n; k++) {
        u32 i = c->ids[k];
        double x = coords[2 * i];
        double y = coords[2 * i + 1];

        if (k > 0 && fabs(x - xp) <= DBL_EPSILON && fabs(y - yp) <= DBL_EPSILON) continue;
        xp = x;
        yp = y;

        if (i == i0 || i == i1 || i == i2) continue;

        // find a visible edge on the convex hull using the edge hash
        u32 start = 0;
        u32 key0 = ctxHashKey(c, x, y);
        bool found = false;
        for (u32 j = 0; j < c->hashSize; j++) {
            i32 hv = c->hullHash[(key0 + j) % c->hashSize];
            if (hv != -1 && static_cast<u32>(hv) != c->hullNext[static_cast<u32>(hv)]) { start = static_cast<u32>(hv); found = true; break; }
        }
        if (!found) {
            // fall back to hullStart if hash lookup fails entirely
            start = c->hullStart;
        }

        start = c->hullPrev[start];
        u32 e = start;
        while (true) {
            u32 q = c->hullNext[e];
            if (orient2d(x, y, coords[2 * e], coords[2 * e + 1], coords[2 * q], coords[2 * q + 1]) >= 0.0) {
                e = q;
                if (e == start) { e = (u32)-1; break; }
            } else {
                break;
            }
        }
        if (e == (u32)-1) continue;  // likely a near-duplicate point

        u32 t = ctxAddTriangle(c, e, i, c->hullNext[e], -1, -1, static_cast<i32>(c->hullTri[e]));
        c->hullTri[i] = ctxLegalize(c, t + 2);
        c->hullTri[e] = t;
        hullSize++;

        // walk forward
        u32 nn = c->hullNext[e];
        while (true) {
            u32 q = c->hullNext[nn];
            if (orient2d(x, y, coords[2 * nn], coords[2 * nn + 1], coords[2 * q], coords[2 * q + 1]) < 0.0) {
                t = ctxAddTriangle(c, nn, i, q, static_cast<i32>(c->hullTri[i]), -1, static_cast<i32>(c->hullTri[nn]));
                c->hullTri[i] = ctxLegalize(c, t + 2);
                c->hullNext[nn] = nn;  // mark removed
                hullSize--;
                nn = q;
            } else {
                break;
            }
        }

        // walk backward
        if (e == start) {
            while (true) {
                u32 q = c->hullPrev[e];
                if (orient2d(x, y, coords[2 * q], coords[2 * q + 1], coords[2 * e], coords[2 * e + 1]) < 0.0) {
                    t = ctxAddTriangle(c, q, i, e, -1, static_cast<i32>(c->hullTri[e]), static_cast<i32>(c->hullTri[q]));
                    ctxLegalize(c, t + 2);
                    c->hullTri[q] = t;
                    c->hullNext[e] = e;  // mark removed
                    hullSize--;
                    e = q;
                } else {
                    break;
                }
            }
        }

        // update hull indices
        c->hullStart = c->hullPrev[i] = e;
        c->hullNext[e] = i;
        c->hullPrev[nn] = i;
        c->hullNext[i] = nn;

        c->hullHash[ctxHashKey(c, x, y)] = static_cast<i32>(i);
        c->hullHash[ctxHashKey(c, coords[2 * e], coords[2 * e + 1])] = static_cast<i32>(e);
    }

    // Reconstruct the convex hull by walking hullNext links for the tracked
    // number of hull edges (matches delaunator's reference behaviour).
    c->hullSize = 0;
    {
        u32 e = c->hullStart;
        for (u32 i = 0; i < hullSize; i++) {
            c->hull[c->hullSize++] = e;
            e = c->hullNext[e];
        }
    }
}

Delaunator delaunatorFrom(const double* coords, u32 n) {
    Delaunator out = {};
    if (n == 0) return out;

    u32 maxTriangles = 2 * n - 5;
    if (maxTriangles < 2) maxTriangles = 2;

    DelaunayCtx* c = static_cast<DelaunayCtx*>(memoryAlloc(sizeof(DelaunayCtx)));
    *c = DelaunayCtx{0};
    c->coords    = coords;
    c->n         = n;
    c->triangles  = static_cast<u32*>(memoryAlloc(sizeof(u32) * maxTriangles * 3));
    c->halfedges  = static_cast<i32*>(memoryAlloc(sizeof(i32) * maxTriangles * 3));
    c->hashSize  = static_cast<u32>(ceil(sqrt(static_cast<double>(n))));
    if (c->hashSize < 1) c->hashSize = 1;
    c->hullPrev  = static_cast<u32*>(memoryAlloc(sizeof(u32) * n));
    c->hullNext  = static_cast<u32*>(memoryAlloc(sizeof(u32) * n));
    c->hullTri   = static_cast<u32*>(memoryAlloc(sizeof(u32) * n));
    c->hullHash  = static_cast<i32*>(memoryAlloc(sizeof(i32) * c->hashSize));
    c->ids       = static_cast<u32*>(memoryAlloc(sizeof(u32) * n));
    c->dists     = static_cast<double*>(memoryAlloc(sizeof(double) * n));
    c->hull      = static_cast<u32*>(memoryAlloc(sizeof(u32) * n));
    c->hullSize = 0;

    delaunatorUpdate(c);

    out.triangles    = c->triangles;
    out.halfedges    = c->halfedges;
    out.trianglesLen = c->trianglesLen;
    out.hull         = c->hull;
    out.hullSize     = c->hullSize;

    memoryFree(c->hullPrev);
    memoryFree(c->hullNext);
    memoryFree(c->hullTri);
    memoryFree(c->hullHash);
    memoryFree(c->ids);
    memoryFree(c->dists);
    memoryFree(c);

    return out;
}

void delaunatorDestroy(Delaunator* d) {
    if (!d) return;
    memoryFree(d->triangles);
    memoryFree(d->halfedges);
    memoryFree(d->hull);
    *d = Delaunator{0};
}
