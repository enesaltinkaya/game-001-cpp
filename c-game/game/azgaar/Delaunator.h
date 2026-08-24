#pragma once

// Delaunator: Delaunay triangulation of a 2D point set.
//
// Faithful C port of Mapbox's "delaunator" (the same algorithm Azgaar's
// Fantasy Map Generator uses to build its Voronoi mesh).  Given a flat array
// of interleaved [x0, y0, x1, y1, ...] coordinates it produces the half-edge
// triangulation (triangles + halfedges) and the convex hull.
//
// The Voronoi diagram (cell adjacency + circumcenter vertices) is derived from
// this output by the caller; see AzgaarWorld.c.

namespace game {
struct Delaunator {
    std::vector<u32> triangles;  // length == trianglesLen; each group of 3 is one CCW triangle
    std::vector<i32> halfedges;  // length == trianglesLen; twin half-edge index or -1 on hull
    u32              trianglesLen;
    std::vector<u32> hull;       // convex hull point indices (CCW)
    u32              hullSize;
};

// Triangulate `n` points stored as interleaved doubles in `coords`
// (length must be n * 2).  Returns a Delaunator whose arrays are heap
// owned by the struct (vectors); call delaunatorDestroy() to release.
Delaunator delaunatorFrom(const double* coords, u32 n);

void delaunatorDestroy(Delaunator* d);
}  // namespace game
