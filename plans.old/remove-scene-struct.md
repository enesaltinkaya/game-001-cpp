# Plan: Remove Scene Struct, Flatten to Global ECS

## Problem

Currently every `.glb` file creates its own `Scene` struct, and each Scene has
its own component sparse sets, entity counter, name-to-entity map, active
entities, and GPU backend (`VulkanScene`). This causes:

1. **Cross-scene references are painful** — animations in `animations.dat` can't
   easily target entities in `eve.dat` because entity IDs and name lookups are
   scene-local.
2. **Duplicate GPU buffer sets** — every Scene gets its own position buffer,
   attribute buffer, index buffer, meshlet buffers, transform buffers, culling
   buffers, joint buffers, etc. Every render pass loops `ecs.scenes` and issues
   separate draw calls per scene.
3. **Entity IDs collide** — entity 1 in scene A is different from entity 1 in
   scene B, but systems like animation/physics need to search across scenes.
4. **Streaming will multiply the problem** — open-world regions would each
   create yet another Scene with yet another full set of GPU buffers.

## Goal

A single flat ECS world with global entity IDs, global component pools, and a
single set of GPU geometry/culling buffers. File-of-origin is tracked only for
loading/unloading purposes via a lightweight `Region` handle.

---

## Architecture Overview

### Before (current)

```
ecs.scenes[] ──► Scene A (eve.dat)
                   ├── components[] (sparse sets)
                   ├── entityCounter
                   ├── nameToEntity
                   ├── activeEntities
                   ├── VulkanScene (positions, indices, meshlets, transforms, culling...)

              ──► Scene B (animations.dat)
                   ├── components[] (sparse sets)
                   ├── entityCounter
                   ├── nameToEntity
                   ├── activeEntities
                   ├── VulkanScene (positions, indices, meshlets, transforms, culling...)
```

### After (target)

```
ecs.world ──► single World
                ├── components[] (sparse sets)    ← one set of pools
                ├── entityCounter                 ← one counter
                ├── nameToEntity                  ← one global map
                ├── activeEntities                ← one map
                ├── VulkanWorld                   ← one set of GPU buffers
                │     ├── positions, indices, meshlets...
                │     ├── transforms, culling...
                │     └── joint buffers
                └── regions[]                     ← lightweight tracking
                      ├── Region "eve.dat"     { entityList, meshHandles }
                      └── Region "terrain.dat" { entityList, meshHandles }
```

---

## Phase 1: Introduce Global World + Region Tracking

**Files to create/modify:**

### 1.1 New: `c-engine/ecs/World.h`

```c
typedef struct Region {
    String name;                  // source file identifier
    Array(u32) entities;          // all entities loaded from this file
    // future: bounding box, load radius, etc.
} Region;
```

### 1.2 Modify: `c-engine/ecs/Ecs.h`

Replace `Array(Scene*) scenes` + `Scene* defaultScene` with a single `Scene world`:

```c
typedef struct Ecs {
    Array(System*) systems;
    Scene world;                 // single global scene (replaces scenes[] + defaultScene)
    Array(Region*) regions;      // tracking for load/unload
    // ...stats fields unchanged
} Ecs;
```

### 1.3 Modify: `c-engine/ecs/Ecs.c`

- `ecsInit()`: initialize `ecs.world` inline (no malloc), set up its sparse sets
- `ecsDestroy()`: destroy `ecs.world`, free all regions
- Remove `ecs.defaultScene` and `ecs.scenes`

### 1.4 Modify: `c-engine/ecs/scene/Scene.h` / `Scene.c`

The `Scene` struct itself is fine as a "component container" — it just becomes
singular. No structural changes to Scene.h needed at first, just usage changes.

---

## Phase 2: Migrate All Systems to Use `ecs.world`

Every system currently does one of:

- `ecs.defaultScene` — for camera, lights, physics raycasts
- `foreach(scene, ecs.scenes)` — for transform updates, animation, rendering

All of these become `&ecs.world`.

### 2.1 Systems that use `ecs.defaultScene` → `&ecs.world`

| File              | Current Usage                                       |
| ----------------- | --------------------------------------------------- |
| `CameraSystem.c`  | `scene = ecs.defaultScene`                          |
| `FlyingCamera.c`  | `getComponent(ecs.defaultScene, ...)` (many places) |
| `LightSystem.c`   | `Scene *scene = ecs.defaultScene`                   |
| `PhysicsSystem.c` | `getComponent(ecs.defaultScene, ...)`               |

**Change:** simple find-replace of `ecs.defaultScene` → `&ecs.world`

### 2.2 Systems that iterate `ecs.scenes` → just use `&ecs.world`

| File                | Current Pattern                                     |
| ------------------- | --------------------------------------------------- |
| `TransformSystem.c` | `foreach(scene, ecs.scenes)` in update + postUpdate |
| `AnimationSystem.c` | `for(s=0; s<arraySize(ecs.scenes); s++)`            |
| `Ecs.c`             | `foreach(scene, ecs.scenes) sceneDestroy(scene)`    |

**Change:** remove loops, operate on `&ecs.world` directly.

### 2.3 `AnimationSystem.c` — `findSceneForEntity()`

This function searches across scenes to find which one owns an entity. With a
single world it becomes trivial:

```c
static Scene* findSceneForEntity(u32 entity) {
    return &ecs.world;
}
```

Or better: remove the function entirely, callers just use `&ecs.world`.

---

## Phase 3: Migrate SceneParser to Load Into Global World

### 3.1 Modify: `c-engine/ecs/scene/SceneParser.c`

Currently `sceneLoadOffThread()`:

1. Allocates a new `Scene*`
2. Parses all nodes into that scene
3. Calls `vulkanSceneCreate(scene)` — creates GPU buffers for this scene
4. Calls `futureTaskAdd(sceneLoadMainThread)` which does `arrayPut(ecs.scenes, scene)`

**New flow:**

1. Parse into `&ecs.world` directly (entities get global IDs)
2. Track created entity IDs in a `Region` struct
3. Register meshes/geometry with the global GPU buffers
4. Add the `Region` to `ecs.regions` on the main thread

**Thread safety concern:** currently the off-thread parsing creates a completely
independent Scene, so there's no contention. With a single world, `createEntity`
must be thread-safe (or entity ID allocation must be pre-reserved).

**Solution options:**

- **Option A:** Pre-allocate an entity ID range on the main thread before
  dispatching the work (`u32 firstEntity = atomicAdd(&ecs.world.entityCounter, estimatedCount)`).
  The off-thread parser uses entity IDs in `[firstEntity, firstEntity+count)`.
- **Option B:** Keep parsing into a temporary local Scene on the worker thread,
  then merge into `ecs.world` on the main thread (remap entity IDs). This is
  safer but requires remapping all component references.
- **Option C (recommended for now):** Do the component creation on the main
  thread via `futureTask`. Parse the glb data (cgltf, meshopt decode) off-thread,
  but create entities/components on the main thread. This matches the current
  pattern where `sceneLoadMainThread` already runs on the main thread.

**Recommended: Option B** — it's closest to the current code. The off-thread
parser creates a temporary scratch Scene, then `sceneLoadMainThread` merges
entities into `ecs.world` with remapped IDs. This avoids thread-safety issues
and keeps the off-thread parsing fast.

### 3.2 Entity ID Remapping (for Option B)

When merging temp scene into world:

```c
void sceneMergeIntoWorld(Scene* tempScene, Region* region) {
    // 1. For each entity in tempScene, create a new entity in ecs.world
    // 2. Copy all components, remapping entity references
    //    (Family.parent, Family.children, Mesh.instances[].entity,
    //     Skin.joints[], nameToEntity values)
    // 3. Track new entity IDs in region->entities
}
```

---

## Phase 4: Unify GPU Buffers (VulkanScene → VulkanWorld)

This is the biggest change. Currently each Scene gets its own GPU buffers.

### 4.1 Single Global VulkanWorld

Instead of per-scene buffers, maintain one set of global buffers that grow
as regions are loaded:

```c
typedef struct VulkanWorld {
    // Same buffers as current VulkanScene, but singular
    VulkanBuffer positionBuffer;
    VulkanBuffer attributeBuffer;
    VulkanBuffer indexBuffer;
    VulkanBuffer meshletBuffer;
    // ... etc

    // Allocation tracking
    u32 positionCapacity, positionUsed;
    u32 indexCapacity, indexUsed;
    u32 meshletCapacity, meshletUsed;
    u32 meshletInstanceCapacity, meshletInstanceUsed;
    u32 triangleInstanceCapacity, triangleInstanceUsed;
} VulkanWorld;
```

### 4.2 Append-Only Geometry Upload

When a new region loads:

1. Check if existing buffers have enough capacity
2. If not, reallocate (create new larger buffer, copy old data, swap)
3. Append new geometry data at the current offsets
4. Record the offset ranges in the Region for later removal

### 4.3 Region Unloading

When a region is unloaded:

- Mark its meshlet instances as inactive (zero out or use a free list)
- Don't compact GPU buffers immediately (fragmentation is acceptable for now)
- Periodically defragment if needed (future optimization)

### 4.4 Render Passes

All render passes currently loop `ecs.scenes`. With a single VulkanWorld:

- Remove the scene loop
- Bind the single set of buffers
- Single dispatch/draw for all geometry

**Files affected (all render passes that loop scenes):**

- `VulkanMeshletCullingPass.c`
- `VulkanMeshletRenderPass.c`
- `VulkanDepthPass.c`
- `VulkanTriangleRenderPass.c`
- `VulkanShadowPass.c`
- `VulkanOitAccumulatePass.c`
- `VulkanPhase2OcclusionPass.c`
- `VulkanReflectionPass.c`
- `VulkanResourceManager.c` (flush transforms/joints)

---

## Phase 5: Clean Up Scene Struct

### 5.1 Remove Scene from Public APIs

Functions like `transformActivate(scene, entity)` currently take a `Scene*`.
Since there's only one world now, these can drop the scene parameter:

```c
// Before
void transformActivate(Scene* scene, u32 entity);
// After
void transformActivate(u32 entity);
```

This is a large API change touching many files. Can be done incrementally:

- First: make everything work with `&ecs.world` passed explicitly
- Later: remove the `Scene*` parameter from all functions

### 5.2 Keep Scene as Internal Container

The `Scene` struct itself (sparse sets, entity counter, active entities) is
still useful as the "world container." Just rename it or keep it — the key
change is that there's only one instance.

---

## Execution Order

### Step 1 (minimal, safe)

- Add `Scene world` to Ecs
- Make `defaultScene` point to `&ecs.world`
- Make camera, lights, physics use `&ecs.world`
- Keep `ecs.scenes[]` working as before for loaded files

### Step 2 (merge loaded scenes into world)

- Modify SceneParser to merge into `ecs.world` instead of creating separate scenes
- Implement entity ID remapping
- Add Region tracking
- Remove `ecs.scenes[]`

### Step 3 (unify GPU buffers)

- Create single VulkanWorld with growing buffers
- Modify SceneParser to append geometry to global buffers
- Modify all render passes to use single buffer set
- Remove per-scene VulkanScene

### Step 4 (clean up API)

- Remove `Scene*` parameter from component/transform/animation APIs
- Simplify animation's `findSceneForEntity`
- Clean up any remaining multi-scene iteration patterns

---

## Risk Assessment

| Risk                                         | Mitigation                                                     |
| -------------------------------------------- | -------------------------------------------------------------- |
| Thread safety when loading into shared world | Use Option B: parse off-thread into temp, merge on main thread |
| GPU buffer reallocation during rendering     | Double-buffer or defer realloc to frame boundaries             |
| Entity ID remapping bugs                     | Thorough testing of Family, Skin, Mesh instance references     |
| Large number of files to modify              | Incremental steps, each step is independently testable         |
| GPU buffer fragmentation from unloading      | Acceptable initially, add defrag pass later                    |

## Files Summary (approximate change count)

| Category      | Files         | Scope                                                      |
| ------------- | ------------- | ---------------------------------------------------------- |
| Core ECS      | 4             | Ecs.h/c, Scene.h/c                                         |
| Scene loading | 2             | SceneParser.c, new merge logic                             |
| Systems       | 6             | Transform, Animation, Camera, FlyingCamera, Light, Physics |
| GPU backend   | 2             | VulkanScene.c/h → VulkanWorld                              |
| Render passes | 8             | All passes that loop ecs.scenes                            |
| Game code     | 2             | Game.c, CameraGui.c                                        |
| **Total**     | **~24 files** |                                                            |
