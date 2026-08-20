# Flow Field Pathfinding Implementation Plan

_Breadth-first search flow field for enemy navigation around obstacles_

---

## Overview

A **flow field** is a grid where each cell stores a direction vector pointing toward the goal. Enemies simply read the direction at their current grid cell and follow it. The field is computed with a **breadth-first search (BFS)** from the player's position, propagating outward through walkable cells.

### Why flow fields for this game?

- **One BFS per frame** (or every N frames), shared by all enemies — no per-enemy A\*
- **Naturally handles dynamic obstacles** — rebuild the field and all enemies adapt instantly
- **No nav mesh baking** needed — walkability is derived from Jolt physics bodies at runtime
- **Simple to integrate** — replaces the direct-to-target velocity in `enemyStateChase` with the flow direction
- **Works with existing Jolt CharacterVirtual** — the character controller still handles collision sliding, stairs, gravity

### Limitations

- Grid resolution is fixed — too coarse and paths look blocky, too fine and BFS is slow
- Only handles flat (XZ) navigation — no multi-level buildings or vertical routing
- BFS is O(grid cells) per update — fine for a 512×512 grid (~262K cells) on modern CPU

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     FlowFieldSystem (ECS)                    │
│                                                             │
│  added():    Allocate grid, subscribe to scene events       │
│  update():   1. Populate walkability from Physics bodies     │
│              2. BFS from player position                    │
│              3. Compute flow directions                     │
│  removed():  Free grid                                      │
│                                                             │
│  flowFieldGetDirection(pos) → vec2 (XZ direction)           │
│  flowFieldIsValid() → bool                                  │
└─────────────────────────────────────────────────────────────┘
                              ▲
                              │ uses
                              │
┌─────────────────────────────┴──────────────────────────────┐
│                     EnemySystem (ENEMY_STATE_CHASE)          │
│                                                             │
│  enemyStateChase():                                          │
│    1. Query flowFieldGetDirection(enemyPos)                  │
│    2. Use flow direction as desiredVel (not direct-to-target)│
│    3. Fall back to direct chase if field is invalid          │
└─────────────────────────────────────────────────────────────┘
```

---

## Grid Design

### Cell structure

Each cell stores:
- **`distance`** — Manhattan distance from goal (player), `UINT32_MAX` = unreachable
- **`walkable`** — whether the cell is traversable

The **flow direction** is computed on-the-fly by comparing distances with neighboring cells (no need to store direction vectors — saves 75% memory).

### Grid dimensions

| Parameter | Value | Rationale |
|---|---|---|
| Grid size | 512 × 512 cells | Covers ~2560×2560 world units (5 units/cell) |
| Cell size | 5.0 world units | Roughly 2× enemy capsule diameter — fine enough for smooth paths |
| World origin | Grid center maps to world (0, 0) | Simple world↔grid conversion |
| Memory | 512×512 × (4 + 1) bytes ≈ 1.3 MB | distance (u32) + walkable (u8) per cell |

### Grid origin and bounds

The grid covers the world area `[-1280, -1280]` to `[1280, 1280]` in XZ. Cells outside this range default to non-walkable. The grid origin (cell 0,0) maps to world `(-1280, -1280)`.

```c
#define FLOW_GRID_SIZE 512
#define FLOW_CELL_SIZE 5.0f
#define FLOW_WORLD_HALF_EXTENT (FLOW_GRID_SIZE * FLOW_CELL_SIZE * 0.5f)  // 1280.0f
```

### World-to-grid conversion

```c
static inline u32 worldToGridX(float wx) {
    return (u32)((wx + FLOW_WORLD_HALF_EXTENT) / FLOW_CELL_SIZE);
}
static inline u32 worldToGridZ(float wz) {
    return (u32)((wz + FLOW_WORLD_HALF_EXTENT) / FLOW_CELL_SIZE);
}
static inline bool gridInBounds(u32 gx, u32 gz) {
    return gx < FLOW_GRID_SIZE && gz < FLOW_GRID_SIZE;
}
```

---

## Walkability Population

### How it works

1. **Reset** all cells to walkable
2. **Iterate** all `Physics` components in all loaded scenes
3. For each static physics body, **compute its AABB** in world space
4. **Mark cells** overlapping the AABB as non-walkable
5. **Refine** with raycasts: for cells on the boundary of obstacles, raycast down to verify if the center is truly blocked (handles concave shapes, archways, etc.)

### Getting AABBs from Physics components

The `Physics` component stores either a `JoltMesh*` or `JoltBody*`. We need to query the world-space AABB. Two approaches:

**Option A (preferred): Add `joltBodyGetAABB()` to the Jolt C wrapper**

Add a function to `jolt_c_api.h/.cpp` that returns the world AABB of a body:

```c
// jolt_c_api.h
char joltBodyGetAABB(uint32_t bodyId, float* outMin, float* outMax);
```

Implementation in `jolt_c_api.cpp`:
```cpp
char joltBodyGetAABB(uint32_t bodyId, float* outMin, float* outMax) {
    BodyID id(bodyId);
    if (!joltSystem->bodyInterface->IsAdded(id)) return 0;
    BodyLockRead lock(joltSystem->physicsSystem->GetBodyLockInterface(), id);
    if (!lock.Succeeded()) return 0;
    const Body& body = lock.GetBody();
    AABox aabb = body.GetWorldSpaceBounds();
    outMin[0] = (float)aabb.mMin.GetX();
    outMin[1] = (float)aabb.mMin.GetY();
    outMin[2] = (float)aabb.mMin.GetZ();
    outMax[0] = (float)aabb.mMax.GetX();
    outMax[1] = (float)aabb.mMax.GetY();
    outMax[2] = (float)aabb.mMax.GetZ();
    return 1;
}
```

**Option B (no wrapper change): Use Transform + AABB from glTF data**

The scene parser stores the original AABB in the `rigidBodyAABB` glTF extra. However, this data is not retained after body creation. We could store it in the `Physics` component, but that requires modifying `PhysicsComponent.h` and the scene parser.

**Recommendation: Option A** — one small addition to the Jolt wrapper, clean and robust.

### Alternative: Raycast-based walkability

If adding `joltBodyGetAABB()` is undesirable, we can populate walkability purely with raycasts:

1. For each grid cell, compute the world position of the cell center
2. Raycast down from `cellCenter.y + 100` to `cellCenter.y - 10` using `joltCastRay()`
3. If the ray hits something close to the cell center (within half cell size), mark non-walkable
4. Additionally raycast upward to check ceiling height (optional, for tunnels)

This is simpler to implement but slower: 512×512 = 262K raycasts per update. Still feasible if done every 2-3 frames (~87K raycasts/frame).

**Recommendation: Use AABB for initial marking, then raycast-refine boundary cells only.** This gives us the best of both: fast initial population with accurate edge detection.

---

## BFS Algorithm

### Data structures

```c
// Grid cell
typedef struct FlowCell {
    u32 distance;   // UINT32_MAX = unvisited/unreachable
    u8 walkable;    // 1 = walkable, 0 = blocked
} FlowCell;

// BFS queue (circular buffer)
typedef struct {
    u32* data;      // packed grid indices (gx | (gz << 16))
    u32 head;
    u32 tail;
    u32 capacity;
} FlowQueue;
```

### BFS pseudocode

```
function bfsFlowField(playerPos):
    gridX = worldToGridX(playerPos.x)
    gridZ = worldToGridZ(playerPos.z)

    clear all distances to UINT32_MAX
    enqueue(gridX, gridZ)
    grid[gridX][gridZ].distance = 0

    while queue not empty:
        (cx, cz) = dequeue()
        currentDist = grid[cx][cz].distance

        for each neighbor (nx, nz) in 4 directions:
            if not gridInBounds(nx, nz): continue
            if not grid[nx][nz].walkable: continue
            if grid[nx][nz].distance <= currentDist + 1: continue

            grid[nx][nz].distance = currentDist + 1
            enqueue(nx, nz)
```

### Neighbor order (for deterministic behavior)

Check neighbors in order: +X, -X, +Z, -Z. This ensures a consistent tie-breaking direction when distances are equal.

### Queue sizing

Maximum queue size = total grid cells = 512×512 = 262,144. Each entry is a packed u32 (16 bits X + 16 bits Z).

Queue memory: 262,144 × 4 bytes ≈ 1 MB.

---

## Flow Direction Query

### On-the-fly direction computation

Given an enemy at grid cell `(gx, gz)`, find the neighbor with the **lowest distance**:

```c
vec2 flowFieldGetDirection(vec3 pos) {
    u32 gx = worldToGridX(pos.x);
    u32 gz = worldToGridZ(pos.z);

    if (!gridInBounds(gx, gz)) return (vec2){0, 0};

    u32 currentDist = grid[gx][gz].distance;
    if (currentDist == UINT32_MAX) return (vec2){0, 0};  // unreachable

    // Check 4 neighbors, find lowest distance
    u32 bestDist = currentDist;
    vec2 bestDir = {0, 0};

    int deltas[4][2] = {{1,0}, {-1,0}, {0,1}, {0,-1}};
    for (int i = 0; i < 4; i++) {
        u32 nx = gx + deltas[i][0];
        u32 nz = gz + deltas[i][1];
        if (!gridInBounds(nx, nz)) continue;
        u32 nd = grid[nx][nz].distance;
        if (nd < bestDist) {
            bestDist = nd;
            bestDir[0] = (float)deltas[i][0];
            bestDir[1] = (float)deltas[i][1];
        }
    }

    return bestDir;  // normalized XZ direction (or {0,0} if at goal)
}
```

### Diagonal support (optional, for smoother paths)

Check all 8 neighbors instead of 4. Diagonal directions get `√2` cost in BFS. This produces smoother paths but is slightly more complex.

**Recommendation: Start with 4-directional, add 8-directional if paths look too blocky.**

---

## Integration with EnemySystem

### Changes to `enemyStateChase()`

Replace the direct-to-target velocity computation:

```c
// OLD:
vec3 dir;
glm_vec3_sub(targetPos, pos, dir);
dir[1] = 0.0f;
glm_vec3_normalize(dir);
glm_vec3_scale(dir, enemy->moveSpeed, desiredVel);

// NEW:
vec2 flowDir = flowFieldGetDirection(pos);
if (flowDir[0] != 0.0f || flowDir[1] != 0.0f) {
    // Flow field says go this way
    glm_vec3_normalize(flowDir);  // normalize the 2D direction
    desiredVel[0] = flowDir[0] * enemy->moveSpeed;
    desiredVel[1] = 0.0f;
    desiredVel[2] = flowDir[1] * enemy->moveSpeed;
} else {
    // Fallback: direct chase (field invalid or unreachable)
    vec3 dir;
    glm_vec3_sub(targetPos, pos, dir);
    dir[1] = 0.0f;
    glm_vec3_normalize(dir);
    glm_vec3_scale(dir, enemy->moveSpeed, desiredVel);
}
```

### Integration with other states

- **`ENEMY_STATE_IDLE` (patrol):** Keep as-is. Patrol points are hand-authored and don't need pathfinding.
- **`ENEMY_STATE_RETREAT:** Use the flow field in **reverse** — compute a flee point behind the enemy (away from player), then follow the flow toward that point. Or simply negate the flow direction.
- **`ENEMY_STATE_ATTACK`:** No movement, no change needed.

For retreat, the simplest approach: compute a point behind the enemy (relative to player), then use `flowFieldGetDirection()` toward that point. This requires a second BFS from the flee point, which doubles the cost.

**Better approach for retreat:** Negate the flow direction. If the flow field points toward the player, going opposite naturally retreats while still navigating around obstacles.

```c
// In enemyStateRetreat:
vec2 flowDir = flowFieldGetDirection(pos);
if (flowDir[0] != 0.0f || flowDir[1] != 0.0f) {
    desiredVel[0] = -flowDir[0] * enemy->moveSpeed * 0.75f;
    desiredVel[1] = 0.0f;
    desiredVel[2] = -flowDir[1] * enemy->moveSpeed * 0.75f;
}
```

---

## Update Frequency

### Options

| Frequency | Pros | Cons |
|---|---|---|
| Every frame | Always up-to-date | ~2-5ms CPU per frame for 512×512 grid |
| Every 2 frames | Half CPU cost | Player can move ~0.5 cells between updates |
| Every N frames (configurable) | Tunable | Stale paths during fast player movement |
| Only when player moves > threshold | Minimal wasted updates | Burst cost when player teleports |

**Recommendation: Every 2 frames** as default. Expose a compile-time define or runtime config:

```c
#define FLOW_UPDATE_INTERVAL 2  // frames between BFS updates
```

### Adaptive update

Trigger a full rebuild when:
- The player moves more than `FLOW_REBUILD_DISTANCE` (e.g., 20 world units) since last update
- A scene is loaded/unloaded (subscribe to `EVENT_SCENE_LOADED` / `EVENT_SCENE_UNLOADED`)

Track the player's last BFS position and skip the BFS if the player hasn't moved enough.

---

## Debug Visualization

### What to visualize

1. **Grid overlay** — wireframe grid on the ground plane (toggle with a key)
2. **Walkability heatmap** — color cells green (walkable) / red (blocked)
3. **Flow arrows** — small arrows or lines showing direction at each cell
4. **BFS frontier** — highlight the current BFS wavefront

### Implementation

Use the existing debug physics pass pattern (`VulkanDebugPhysicsPass`). Add debug lines via `joltGetDebugLines` or a dedicated debug line buffer.

Simpler approach: use the existing `VulkanDebugPhysicsPass` line buffer. The flow field system populates `JoltDebugLine` entries:

```c
// In FlowFieldSystem update(), after BFS:
if (debugEnabled) {
    for each cell:
        if cell.walkable:
            add green line from cell center to cell center + (0, 0.1, 0)
        else:
            add red line
        if cell.distance != UINT32_MAX:
            add arrow line showing flow direction
}
```

Or use a simpler approach: render a few key lines (player position, enemy paths, obstacle boundaries) rather than the full grid.

---

## New Files

| File | Purpose |
|---|---|
| `c-engine/ecs/system/pathfinding/FlowField.h` | Header — grid constants, public API |
| `c-engine/ecs/system/pathfinding/FlowField.c` | Flow field system (BFS, walkability, direction query) |

### Modified files

| File | Change |
|---|---|
| `c-engine/ecs/system/physics/PhysicsComponent.h` | No change (use existing `Physics` component for iteration) |
| `c-game/game/enemy/EnemySystem.c` | Integrate flow field direction in chase/retreat states |
| `c-game/game/gameState/GameState.c` | Add `FlowField` system when gameplay starts |
| `/home/enes/Projects/c/cpp-thirdparty/jolt/wrapper/src/jolt_c_api.h` | Add `joltBodyGetAABB()` |
| `/home/enes/Projects/c/cpp-thirdparty/jolt/wrapper/src/jolt_c_api.cpp` | Implement `joltBodyGetAABB()` |

CMake uses `GLOB_RECURSE` so no `CMakeLists.txt` changes are needed.

---

## Detailed Implementation Steps

### Step 1: Add `joltBodyGetAABB()` to Jolt C wrapper

**File:** `/home/enes/Projects/c/cpp-thirdparty/jolt/wrapper/src/jolt_c_api.h`

Add declaration:
```c
/// Get the world-space AABB of a body.
/// bodyId is the opaque BodyID index (from JoltOverlapHit or internal tracking).
/// Returns 1 on success, 0 if body not found.
char joltBodyGetAABB(uint32_t bodyId, float* outMin, float* outMax);
```

**File:** `/home/enes/Projects/c/cpp-thirdparty/jolt/wrapper/src/jolt_c_api.cpp`

Add implementation (see Walkability Population section above).

Rebuild the Jolt wrapper:
```bash
cd /home/enes/Projects/c/cpp-thirdparty/jolt/wrapper && ./build.sh
```

### Step 2: Create `FlowField.h`

**File:** `c-engine/ecs/system/pathfinding/FlowField.h`

```c
#pragma once

#include "ecs/system/System.h"

extern struct System flowFieldSystem;

/// Initialize the flow field system. Call once at startup.
void flowFieldInit(void);

/// Destroy the flow field system.
void flowFieldDestroy(void);

/// Get the flow direction at a world position.
/// Returns a vec2 (XZ) direction, normalized. Returns {0, 0} if unreachable.
void flowFieldGetDirection(vec3 pos, vec2 outDir);

/// Check if the flow field has a valid BFS result.
bool flowFieldIsValid(void);

/// Get the grid distance at a world position (for debugging).
u32 flowFieldGetDistance(vec3 pos);

/// Toggle debug visualization.
void flowFieldSetDebug(bool enabled);
bool flowFieldIsDebug(void);
```

### Step 3: Create `FlowField.c`

**File:** `c-engine/ecs/system/pathfinding/FlowField.c`

Key sections:

1. **Grid allocation** in `added()`:
   - Allocate `FlowCell[FLOW_GRID_SIZE][FLOW_GRID_SIZE]`
   - Allocate BFS queue
   - Subscribe to scene events for rebuild triggers

2. **`update()`**:
   - Check update interval counter
   - Get player position from `combatGetPlayerEntity()`
   - If player moved less than threshold since last BFS, skip
   - Populate walkability (reset → mark obstacles → raycast refine)
   - Run BFS from player grid position

3. **`flowFieldGetDirection()`**:
   - Convert world pos to grid coords
   - Check 4 neighbors, find lowest distance
   - Return normalized direction

4. **`removed()`**:
   - Free grid and queue memory

Estimated size: ~400 lines.

### Step 4: Integrate into EnemySystem

**File:** `c-game/game/enemy/EnemySystem.c`

Add `#include "ecs/system/pathfinding/FlowField.h"` at the top.

In `enemyStateChase()`, replace the direct velocity computation with flow field query (see Integration section above).

In `enemyStateRetreat()`, negate the flow direction.

Estimated changes: ~20 lines modified.

### Step 5: Register the system

**File:** `c-game/game/gameState/GameState.c`

Add `#include "ecs/system/pathfinding/FlowField.h"` and register in the gameplay state:

```c
// In the gameplay state entry (around line 190):
systemAdd(gameSystem.priority + 1, &flowFieldSystem);
systemAdd(gameSystem.priority + 2, &enemySystem);  // enemy runs after flow field
```

And remove in the gameplay state exit:
```c
systemRemove(&flowFieldSystem);
```

Estimated changes: ~5 lines.

### Step 6: Build and test

```bash
./scripts/build.sh
./scripts/run.sh play log 5000
```

Verify:
1. Enemies no longer get stuck on cube obstacles
2. Enemies navigate around obstacles to reach the player
3. Performance is acceptable (check `build/c-game/data/game.log` for frame times)
4. Debug visualization shows correct walkability and flow directions

---

## Performance Budget

| Operation | Cost (512×512 grid) | Notes |
|---|---|---|
| Grid reset (clear distances) | ~1 ms | `memset` over 1 MB |
| Walkability population | ~0.5 ms | Iterate Physics components (~100 bodies) |
| Raycast refinement | ~1-2 ms | Only boundary cells (~1000-5000 raycasts) |
| BFS | ~1-2 ms | Visit each walkable cell once |
| **Total per update** | **~3-6 ms** | Every 2 frames = **1.5-3 ms/frame average** |

On a modern CPU this should be well under 5ms. If needed:
- Reduce grid size to 256×256 (10× faster, coarser paths)
- Increase update interval to 4 frames
- Skip raycast refinement (use AABB-only walkability)

---

## Tunable Parameters

| Parameter | Default | Range | Notes |
|---|---|---|---|
| `FLOW_GRID_SIZE` | 512 | 128-1024 | Grid resolution |
| `FLOW_CELL_SIZE` | 5.0 | 2.0-10.0 | World units per cell |
| `FLOW_UPDATE_INTERVAL` | 2 | 1-8 | Frames between BFS |
| `FLOW_REBUILD_DISTANCE` | 20.0 | 5.0-50.0 | Player movement threshold for rebuild |
| `FLOW_DEBUG_ENABLED` | false | bool | Show grid overlay |

These start as `#define` constants. Can be exposed to a debug GUI later.

---

## Edge Cases

| Case | Handling |
|---|---|
| Player is inside an obstacle | BFS starts from nearest walkable cell (search outward from player grid pos) |
| Enemy is on a non-walkable cell | Fall back to direct chase (enemy might be on a ledge or inside geometry) |
| No path exists (player unreachable) | `flowFieldGetDirection()` returns `{0, 0}`, enemy falls back to direct chase |
| Player moves very fast (teleport) | Distance-based rebuild threshold catches this |
| Scene changes (new level) | Subscribe to scene events, rebuild walkability on scene load/unload |
| Many enemies at same location | Flow field is shared — no extra cost per enemy |
| Enemy on different terrain height | Grid is XZ-only — height doesn't affect walkability (character controller handles Y) |

---

## Future Enhancements

1. **8-directional BFS** — smoother paths with diagonal movement
2. **Multi-goal flow fields** — separate fields for patrol points, loot items, etc.
3. **Dynamic obstacle exclusion** — mark other enemies as temporary obstacles for separation
4. **Nav mesh fallback** — for indoor areas with multiple floors, use hand-authored nav meshes
5. **GPU-accelerated BFS** — compute shader for the BFS on supported hardware
6. **Flow field blending** — blend between flow direction and direct chase when close to player (avoids "orbiting" behavior)
7. **Per-enemy flow fields** — for bosses or special enemies that need independent pathfinding

---

## Estimated File Sizes

| File | ~Lines |
|---|---|
| `FlowField.h` | ~30 |
| `FlowField.c` | ~400 |
| `EnemySystem.c` changes | ~20 |
| `GameState.c` changes | ~5 |
| `jolt_c_api.h` changes | ~5 |
| `jolt_c_api.cpp` changes | ~15 |

Total: ~475 new/modified lines.

---

## Implementation Order Summary

1. **Add `joltBodyGetAABB()`** to Jolt C wrapper → rebuild wrapper
2. **Create `FlowField.h`** — constants, public API
3. **Create `FlowField.c`** — grid, BFS, walkability, direction query
4. **Register `flowFieldSystem`** in `GameState.c`
5. **Integrate into `EnemySystem.c`** — chase and retreat states
6. **Build and test** — `./scripts/build.sh && ./scripts/run.sh play log 5000`
7. **Add debug visualization** — toggle with a key, verify grid correctness
8. **Tune parameters** — grid size, cell size, update interval
9. **Screenshot verification** — `./scripts/run.sh play screenshot /tmp/flowfield.png`

### Verification checklist

- [ ] Build succeeds with no warnings
- [ ] Enemies navigate around cube obstacles
- [ ] Enemies still chase player directly when no obstacles block
- [ ] Retreat state works (enemies run away while avoiding obstacles)
- [ ] No performance regression (check frame times in game.log)
- [ ] Debug visualization shows correct walkability
- [ ] Edge cases handled (player inside obstacle, unreachable player)
