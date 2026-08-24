# Recast NavMesh System

The game uses [Recast/Detour](https://github.com/recastnavigation/recastnavigation) for AI enemy pathfinding. The C++ Recast/Detour library is wrapped in a thin `extern "C"` API so the game code never touches Recast's C++ types directly.

## Architecture Overview

```
┌────────────────────────────────────────────────────────────────┐
│  c-game (C++)                                                  │
│                                                                │
│  NavMeshSystem.cpp ── navMeshFindPath() ──┐                    │
│                    ── navMeshClosestPoint()─┤                  │
│                                          │                     │
│  EnemySystem.cpp ── chase/retreat AI ──────┘                   │
├────────────────────────────────────────────────────────────────┤
│  libcrecast.a (C wrapper)                                      │
│                                                                │
│  recast_c_api.cpp  ── RcNavMesh, DtNavMeshQuery opaque types   │
├────────────────────────────────────────────────────────────────┤
│  libRecast.a  +  libDetour.a  (C++ core)                       │
│                                                                │
│  Recast:  heightfield → compact → regions → contours → polymesh│
│  Detour:  polygonal pathfinding on the baked navmesh           │
└────────────────────────────────────────────────────────────────┘
```

## Components

### 1. C API Wrapper (`cpp-thirdparty/recast/wrapper/`)

| File                   | Purpose                                                                             |
| ---------------------- | ----------------------------------------------------------------------------------- |
| `src/recast_c_api.h`   | Public C header — opaque `RcNavMesh*` and `DtNavMeshQuery*` types                   |
| `src/recast_c_api.cpp` | Implementation — wraps `dtNavMesh`, `dtNavMeshQuery`, and the Recast build pipeline |

The wrapper exposes these public functions:

| Function                               | Description                                                 |
| -------------------------------------- | ----------------------------------------------------------- |
| `rcNavMeshBuild()`                     | Build a single-tile navmesh from a triangle soup            |
| `rcNavMeshSave()`                      | Serialize navmesh tile data to a malloc'd buffer            |
| `rcNavMeshLoad()`                      | Deserialize a single-tile navmesh from a binary blob        |
| `rcNavMeshLoadTiled()`                 | Deserialize a multi-tile navmesh from individual tile blobs |
| `rcNavMeshDestroy()`                   | Free a navmesh                                              |
| `rcQueryCreate()` / `rcQueryDestroy()` | Create/destroy a query handle                               |
| `rcQueryFindPath()`                    | Find a straight-path waypoint list between two points       |
| `rcQueryClosestPoint()`                | Snap a world position to the nearest navmesh surface        |

`rcNavMeshBuild()` accepts an optional `RcNavMeshConfig*` struct:

```c
typedef struct RcNavMeshConfig {
    float cellSize;            // 0.3
    float cellHeight;          // 0.2
    float agentHeight;         // 2.0
    float agentRadius;         // 0.6
    float agentMaxClimb;       // 0.9
    float agentMaxSlope;       // 45.0 (degrees)
    float regionMinSize;       // 50.0 (voxels)
    float regionMergeSize;     // 20.0 (voxels)
    float edgeMaxLen;          // 12.0
    float edgeMaxError;        // 1.3
    float detailSampleDist;    // 6.0
    float detailSampleMaxError;// 1.0
} RcNavMeshConfig;
```

Pass `NULL` to use the defaults shown above.

### 2. NavMesh System (`c-game/game/navmesh/NavMeshSystem.cpp`)

An ECS system that manages the runtime navmesh instance.

**Lifecycle:**

- **`added()`** — called when entering gameplay. Loads `models/combined.nav.dat` from the asset pack:
  1. Reads the file via `dataManagerRead()`
  2. Decompresses with ZSTD
  3. Parses the binary header (magic `"NAVM"`, version, bounds, tile count)
  4. Calls `rcNavMeshLoadTiled()` (v2) or `rcNavMeshLoad()` (v1)
  5. Creates a `DtNavMeshQuery` handle
- **`removed()`** — called when exiting gameplay. Destroys the query and navmesh.

**Public API:**

```cpp
// Find a path from start to end. Returns waypoint count (0 = no path).
uint32_t navMeshFindPath(engine::Scene* scene, const float* startPos,
                         const float* endPos, float* outPath, uint32_t maxPath);

// Snap a position to the nearest navmesh point. Returns 1 on success.
int navMeshClosestPoint(engine::Scene* scene, const float* pos, float* outPoint);

// Get the loaded navmesh (debug visualization). nullptr if not loaded.
RcNavMesh* navMeshGetMesh(void);
```

Note: `scene` parameters are kept for API consistency but are currently unused — the navmesh is global (single static instance).

### 3. Enemy AI Integration (`c-game/game/enemy/EnemySystem.cpp`)

Enemies use the navmesh during `ENEMY_STATE_CHASE` and `ENEMY_STATE_RETREAT`:

1. **Path recalculation** — every 0.5s or when all waypoints are consumed, `navMeshFindPath()` is called with the enemy's current position and the target (player) position.
2. **Waypoint following** — the enemy moves toward `pathWaypoints[pathCurrentWaypoint]` using its Jolt character controller. When within 1m of a waypoint, it advances to the next.
3. **Stuck detection** — if the enemy stays on the same waypoint for >1s across multiple recalculations, it snaps to the nearest valid navmesh position via `navMeshClosestPoint()` and skips the waypoint.
4. **Fallback** — if no path is found, the enemy moves in a straight line toward the target.

The `Enemy` component stores:

```c
vec3  pathWaypoints[64];
u32   pathWaypointCount;
u32   pathCurrentWaypoint;
float pathRecalcTimer;
float pathWaypointStuckTimer;
u32   pathPrevWaypointIdx;
```

### 4. NavMesh Builder Tool (`tools/navmesh-builder/main.cpp`)

A standalone C++ tool that bakes navmesh tiles from glTF/GLB collision geometry.

**Usage:**

```bash
navmesh-builder <input.glb> <output.nav> [--obstacles obs1.glb ...]
```

**Pipeline:**

1. **Parse glTF** — uses `cgltf` to load the model, with `meshoptimizer` decompression for gltfpack-compressed buffers.
2. **Collect triangles** — walks the node tree and collects primitives from nodes tagged with `"rigidBodyShape"` in their JSON extras. Transforms vertices by the node's world matrix.
3. **Calculate bounds** — computes AABB of all collected vertices. Optional clipping via `NAVMESH_BOUNDS` env var.
4. **Tile grid** — divides the AABB into tiles of `tileWorldSize = cellSize * tileSize` (default: 0.3 × 512 = 153.6m per tile).
5. **Per-tile build** (the Recast pipeline):
   - Clip triangles to tile AABB (with padding for border vertices)
   - `rcCreateHeightfield` — rasterize into a heightfield grid
   - `rcMarkWalkableTriangles` — mark triangles below max slope as walkable
   - `rcRasterizeTriangles` — fill the heightfield
   - `rcFilterLowHangingWalkableObstacles` / `rcFilterLedgeSpans` / `rcFilterWalkableLowHeightSpans` — remove unwalkable spans
   - `rcBuildCompactHeightfield` — compress the heightfield
   - `rcErodeWalkableArea` — shrink walkable area by agent radius
   - `rcBuildDistanceField` — prepare for watershed
   - `rcBuildRegions` — watershed region partitioning
   - `rcBuildContours` — extract region contours
   - `rcBuildPolyMesh` — build polygon mesh from contours
   - `rcBuildPolyMeshDetail` — add height detail mesh
   - `dtCreateNavMeshData` — serialize into a Detour tile blob
6. **Write output** — binary format with `"NAVM"` magic header.

**Configurable via environment variables:**

| Env Var                           | Default | Description                                    |
| --------------------------------- | ------- | ---------------------------------------------- |
| `NAVMESH_CELL_SIZE`               | 0.3     | Grid cell size in world units                  |
| `NAVMESH_CELL_HEIGHT`             | 0.2     | Grid cell height                               |
| `NAVMESH_AGENT_HEIGHT`            | 2.0     | Agent height                                   |
| `NAVMESH_AGENT_RADIUS`            | 0.6     | Agent radius                                   |
| `NAVMESH_AGENT_MAX_CLIMB`         | 0.9     | Max climbable step height                      |
| `NAVMESH_TILE_SIZE`               | 512     | Tiles are N×N cells                            |
| `NAVMESH_BOUNDS`                  | —       | `minX,minY,minZ,maxX,maxY,maxZ` to clip bounds |
| `NAVMESH_EDGE_MAX_LEN`            | 12.0    | Max contour edge length before subdivision     |
| `NAVMESH_EDGE_MAX_ERROR`          | 0.3     | Max deviation of simplified contour edges      |
| `NAVMESH_DETAIL_SAMPLE_DIST`      | 2.0     | Detail mesh surface sampling distance          |
| `NAVMESH_DETAIL_SAMPLE_MAX_ERROR` | 0.5     | Max detail mesh surface deviation              |
| `NAVMESH_MIN_POLYS`               | 1       | Minimum polygons per tile (filter artifacts)   |

### 5. NavMesh Tester Tool (`tools/navmesh-tester/main.cpp`)

A comprehensive navmesh diagnostics tool that loads a `.nav.dat` file and runs analysis.

**Usage:**

```bash
navmesh-tester <navmesh.nav.dat> [mode] [args...]
```

**Modes:**

| Mode                         | Description                                                |
| ---------------------------- | ---------------------------------------------------------- |
| `stats`                      | Tile/polygon statistics with heat map (default)            |
| `coverage [step]`            | Grid coverage probe — ASCII map + quantitative hit/miss    |
| `connectivity`               | Connected component analysis via BFS                       |
| `obj <output.obj>`           | Export navmesh polygons to Wavefront OBJ (view in Blender) |
| `path <sx sy sz> <ex ey ez>` | Find path between two points, report waypoints and length  |
| `probe <x y z>`              | Closest-point query + Y-sweep at a position                |
| `full [step]`                | Run stats + connectivity + coverage                        |

**Key diagnostics:**

- **Stats**: Per-tile polygon/vertex counts, Y range, heat map visualization, debug triangle estimate
- **Connectivity**: BFS over all polygon adjacency + tile links to find connected components. Reports component sizes, locations of disconnected islands, and warns if agents can't path between areas.
- **Coverage**: Probes a regular grid across the navmesh bounds at the median Y, produces an ASCII coverage map and quantitative hit/miss statistics.
- **OBJ export**: Dumps all navmesh polygons as triangulated faces. Open in Blender alongside the terrain model to visually verify coverage and alignment.

## Binary File Format (`.nav.dat`)

Files are ZSTD-compressed. The decompressed layout:

### Version 2 (tiled)

```
Offset  Size    Field
------  ----    -----
0       4       Magic "NAVM"
4       4       Version (2)
8       12      bmin[3] (float) — navmesh AABB min
20      12      bmax[3] (float) — navmesh AABB max
32      4       tileCount (u32)
36      4       tileWorldSize (float)
40      4       tile[0].size (u32)
44      N       tile[0].data (u8[])
...     ...     tile[1].size, tile[1].data, ...
```

### Version 1 (single tile)

```
Offset  Size    Field
------  ----    -----
0       4       Magic "NAVM"
4       4       Version (1)
8       12      bmin[3] (float)
20      12      bmax[3] (float)
32      4       tileSize (u32)
36      N       tileData (u8[])
```

## Build Integration

The game links three Recast-related libraries (from `cpp-thirdparty`):

```
libcrecast.a       — C wrapper (recast_c_api.cpp)
libRecast.a        — Recast baking library
libDetour.a        — Detour pathfinding library
```

Include path: `${thirdparty}/recast/wrapper/src`

The navmesh system is registered in `GameState.cpp` during gameplay enter, with priority `gameSystem.priority + 1` (after enemy system at 1200, navmesh at 1201).

### Building the third-party libraries

`cpp-thirdparty/recast/build.sh` compiles the Recast/Detour C++ sources into `libRecast.a` and `libDetour.a`, and the C wrapper into `libcrecast.a`. It produces both Linux and Windows builds.

### Building the standalone tools

- **navmesh-builder**: `tools/build-navmesh-builder.sh` — compiles `tools/navmesh-builder/main.cpp` against cgltf, meshoptimizer, and Recast/Detour.
- **navmesh-tester**: `tools/build-navmesh-tester.sh` — compiles `tools/navmesh-tester/main.cpp` against the C wrapper, Recast/Detour, and ZSTD.

Both scripts skip rebuilding if the output binary is newer than the source.

### NavMesh bake script

`scripts/build-navmesh.sh` orchestrates the full bake:

1. Decompresses all `.dat` files in `data/pak_1/models/` and `data/pak_1/models/terrain/` (skipping `*.nav.dat` / `*.jolt.dat`) into a temp directory as `.glb`.
2. Uses the first terrain GLB as the base, passing all scene GLBs as `--obstacles` to the builder.
3. Overrides `NAVMESH_AGENT_RADIUS=0.5` and `NAVMESH_BOUNDS="-1000,0,1000,300,900,2250"`.
4. Compresses the output with `zstd -10` and writes it to `data/pak_1/models/combined.nav.dat`.
5. Uses a stamp file (`scripts/.tmp/build-navmesh.stamp`) to skip rebuilding when inputs are unchanged.

## Current NavMesh Asset

The baked navmesh lives at:

```
c-game/data/pak_1/models/combined.nav.dat
```

It is loaded from the asset pack by `dataManagerRead()` which transparently handles `.pak` extraction.

## Pathfinding Details (in `rcQueryFindPath`)

The C API's `rcQueryFindPath` implements:

1. **Nearest polygon lookup** — `findNearestPoly` with half-extents `{16, 32, 16}` for both start and end.
2. **Polygon path** — `findPath` through the navmesh graph (up to 256 polygons).
3. **Partial result retry** — if `DT_PARTIAL_RESULT`, tries offset positions (±2, ±4 on X and Z axes) to find an alternate start polygon that yields a complete path.
4. **Straight path extraction** — `findStraightPath` converts the polygon path into corner waypoints.

The search filter includes all flags (`0xffff`) and excludes none.
