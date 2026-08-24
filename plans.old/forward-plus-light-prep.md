# Forward+ Light Preparation Plan

Before implementing Forward+, we need better light handling: a clean GPU representation
and proper parsing of lights from `.glb` files via the SceneParser.

---

## Current State

### 1. `Light.h` — Confused/Dual Structure

There are two parallel structs that do similar things and don't align well:

- **`LightData`** (CPU-side): uses `vec3 location` (causes implicit padding), `vec4 direction`,
  `vec4 color` (w=intensity), `vec4 properties` (innerCone/outerCone/radius/castShadows).
  Fine conceptually but `vec3 location` will cause std430 alignment issues if ever uploaded directly.
- **`LightUbo`**: the `lightData[3][100]` 2D array indexed by type — **never actually used anywhere**
  (`rendererUploadLights` in `Renderer.c` is a stub, and there is no matching GPU buffer).
- **`DirectionalLightUbo`**: a completely separate, smaller struct that IS actually used — only for
  the one sun light.
- **`Light` component**: has `uboIndex` and `entityId` fields that serve no current purpose.
- **Header declares `extern Light sun`** but `LightSystem.c` has `static DirectionalLightUbo sun` —
  two different types with the same name.

### 2. `LightSystem.c` — Hardcoded, Does Nothing Dynamic

- Always creates one hardcoded entity with hardcoded directional light values.
- Never reads the `Light` component back to build any GPU upload — it writes a separate
  `DirectionalLightUbo sun` local variable.
- Has no logic to collect point/spot lights from the scene or upload them.

### 3. `SceneParser.c` — Lights Completely Ignored

- `parseNode()` checks `node->mesh` and `node->skin` but **never checks `node->light`**.
- No `parseLight()` function exists.
- cgltf fully supports `KHR_lights_punctual` (`cgltf_light` with `color[3]`, `intensity`,
  `type`, `range`, `spot_inner_cone_angle`, `spot_outer_cone_angle`).

### 4. GPU Buffer (`VulkanSceneBuffer` / `globalset.shader`) — Only One Directional Light

- The GLSL `SceneBuffer` only contains `DirectionalLight directionalLight` — a single sun.
  No array, no point/spot lights.
- `VulkanSceneBuffer` on the C side mirrors this exactly.
- **Forward+ needs a flat GPU array of all lights** (with a count) that the tiling compute
  shader can bin into screen-space tiles.

### 5. Upload Path — Stub

- `rendererUploadLights(LightUbo*)` in `Renderer.c` is an **empty function body**.
- `vulkanResourceUploadDirectionalLight` works fine but only covers the one sun.

---

## What Needs to Be Done

### Step 1 — Clean up `Light.h`: define a single `GpuLight` struct

Replace the fragmented `LightData` / `LightUbo` / `DirectionalLightUbo` with a
**single, std430-aligned GPU light struct** that covers all three types:

```c
// 48 bytes, fully vec4-aligned for std430
typedef struct GpuLight {
    vec4 positionAndRange;   // xyz: world position, w: range (0 = infinite for directional)
    vec4 directionAndType;   // xyz: direction, w: LightType (0=dir, 1=point, 2=spot)
    vec4 colorAndIntensity;  // rgb: color, w: intensity (candelas/lux)
    vec4 spotAngles;         // x: cos(innerCone), y: cos(outerCone), zw: unused
} GpuLight;

#define MAX_GPU_LIGHTS 1024

typedef struct LightUbo {
    ivec4 counts;                      // x: directional, y: point, z: spot, w: total
    GpuLight lights[MAX_GPU_LIGHTS];   // flat array, sorted: directionals first, then point, then spot
} LightUbo;
```

Keep `DirectionalLightUbo` and `ShadowUbo` as-is for now since they feed the existing
shadow system. Or consolidate the directional into the flat list and derive sun from
`lights[0]` — but that's a bigger shadow system refactor. Keep them separate for this step.

Clean up the `Light` component:

```c
typedef struct Light {
    enum LightType lightType;
    vec3 color;
    float intensity;
    float range;              // point/spot only
    float innerConeAngle;     // spot only (radians)
    float outerConeAngle;     // spot only (radians)
    bool castsShadows;
} Light;
```

### Step 2 — Add `GpuLight` + light array to `globalset.shader` and `VulkanSceneBuffer`

In `globalset.shader`, extend `SceneBuffer`:

```glsl
#define MAX_GPU_LIGHTS 1024

struct GpuLight {
    vec4 positionAndRange;   // xyz: world pos, w: range
    vec4 directionAndType;   // xyz: direction, w: type (0=dir,1=point,2=spot)
    vec4 colorAndIntensity;  // rgb: color, w: intensity
    vec4 spotAngles;         // x: cos(inner), y: cos(outer), zw: unused
};

layout(buffer_reference, std430) buffer SceneBuffer {
    Camera cameras[4];
    DirectionalLight directionalLight;  // keep for shadow pass compatibility
    ShadowData shadow;
    int time;
    int pad[3];
    ivec4 lightCounts;                  // x=dir, y=point, z=spot, w=total
    GpuLight lights[MAX_GPU_LIGHTS];
};
```

Mirror that in `VulkanSceneBuffer` in `VulkanResourceManager.c`.

### Step 3 — Add `parseLight()` to `SceneParser.c`

In `parseNode()`, after the mesh check, add:

```c
if (node->light) {
    parseLight(scene, node, entity);
}
```

`parseLight()` reads `cgltf_light` fields and creates a `Light` component.
Direction is derived from the node's `Transform` at runtime, not baked in.

```c
void parseLight(Scene* scene, cgltf_node* node, u32 entity) {
    cgltf_light* l = node->light;
    Light* light   = createComponent(scene, Light, entity);
    glm_vec3_copy(l->color, light->color);
    light->intensity = l->intensity;
    light->range     = l->range;
    switch (l->type) {
        case cgltf_light_type_directional: light->lightType = LIGHT_DIRECTIONAL; break;
        case cgltf_light_type_point:       light->lightType = LIGHT_POINT;       break;
        case cgltf_light_type_spot:
            light->lightType      = LIGHT_SPOT;
            light->innerConeAngle = l->spot_inner_cone_angle;
            light->outerConeAngle = l->spot_outer_cone_angle;
            break;
        default: break;
    }
}
```

### Step 4 — Fix `LightSystem.c`: push-based uploads (no polling)

Do **not** loop all lights each frame to check for changes. Follow the same pattern as
transforms: **push-based notification**. Whoever changes a light's properties or moves
its entity calls `lightMarkDirty(scene, entity)` directly — the system never polls.

This mirrors `transformActivate()`: when a transform changes, the caller adds the entity
to `scene->activeEntities`. The transform system only loops that small active set.

For lights:
- Expose `lightMarkDirty(scene, entity)` — adds the entity to a small per-scene dirty list.
- `LightSystem.update()` only loops that dirty list (typically tiny — most lights are static).
  For each dirty light: read `WorldTransform` for position/direction, build a `GpuLight`,
  push to the upload queue. Clear the dirty list after.
- On `createComponent(scene, Light, entity)` — automatically mark dirty (initial upload).
- Always upload `lightCounts` (cheap `ivec4`) since lights can be added/removed any frame.
- For directional lights that are dirty, also rebuild the `DirectionalLightUbo` (shadow
  pass compatibility).
- Fix the naming collision: remove `extern Light sun` from the header (it shadows the
  component type name).

Note: the **tiling pass** (Forward+ compute step) still runs every frame because the camera
moves and tile→light assignments must be rebuilt. But that reads from the already-uploaded
persistent light buffer — it does not trigger a re-upload.

### Step 5 — Wire up the GPU upload

- The GPU light buffer is **persistent** (device-local or persistently mapped), not rebuilt
  each frame. Only dirty entries are patched, same as `vulkanSceneFlushTransforms`.
- Implement `rendererUploadLight(GpuLight* light, u32 index)` for per-light slot patching.
- `lightCounts` is uploaded cheaply each frame via the existing `UploadQueue` mechanism.
- `vulkanResourceUpdate()` flushes the count into `SceneBuffer.lightCounts`.

---

## Summary Table

| Area | Problem | Fix |
|---|---|---|
| `Light.h` | Split structs, `LightUbo` never used, `vec3` alignment, naming collision | Unified `GpuLight`, clean `Light` component |
| `LightSystem.c` | Hardcoded sun, no scene collection, stub upload | Collect all `Light` + `Transform` components, build + upload `LightUbo` |
| `SceneParser.c` | `node->light` never read | Add `parseLight()`, call from `parseNode()` |
| `globalset.shader` | Only one `DirectionalLight` in `SceneBuffer` | Add `ivec4 lightCounts` + `GpuLight lights[MAX_GPU_LIGHTS]` |
| `VulkanSceneBuffer` (C) | Mirrors shader, same gap | Add matching fields |
| `rendererUploadLights` | Empty stub | Full implementation through resource manager |
