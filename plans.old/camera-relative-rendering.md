# Camera-Relative Rendering (Floating Origin)

## Problem

At coordinates like (-2330, 50, 3000), skinned animations jitter/stutter/shake.
gltfpack quantization reduces usable float32 range significantly — the effective
precision limit is much lower than the ~30,000 unit range of raw float32.

The root cause: vertex shaders compute `worldPos` with large absolute values,
then multiply by a `viewProjection` matrix that encodes an equally large camera
translation. The matrix multiply effectively subtracts two large numbers to get
a small clip-space result — catastrophic cancellation in float32.

## Solution

Shift the entire rendering coordinate system so the camera is always at the
origin. All world-space values on the GPU become **camera-relative** — small
numbers with full float32 precision.

## World Size

| Range from origin | Subtraction error | Status                                        |
| ----------------- | ----------------- | --------------------------------------------- |
| ±1,000            | 0.00012           | Perfect                                       |
| ±5,000            | 0.0006            | Perfect                                       |
| ±10,000           | 0.002             | Solid — no visible artifacts                  |
| ±50,000           | 0.008             | Borderline — subtle jitter on fine animations |

**Practical limit: ~20 km × 20 km per map** (~400 km²), assuming 1 unit = 1 meter.

The game uses an island/map system. Each island can be up to 20 km × 20 km.
Traveling between islands is a teleport with loading screen. Each island loads
as its own scene with fresh GPU buffers and coordinates near origin.

## Key Design Decision: Shader-Side Subtraction

The GPU transform buffer is **persistent and incrementally updated** — only
active/moving entities get their transforms written each frame. Static objects
(terrain, buildings, props) are uploaded once and sit untouched in the mapped
buffer. Lights use the same dirty-tracking pattern.

**CPU-side origin subtraction is ruled out** because changing the origin every
frame would invalidate every entry in the buffer, requiring a full re-upload
of all entities and destroying the incremental update advantage.

Instead, the origin is passed as a uniform in the CameraUBO. Shaders subtract
it from world positions **after** computing them, **before** the VP multiply.
The transform buffer, joint matrices, and light positions remain in true world
space on the GPU.

**No compute shaders needed.** The 1080 Ti is bandwidth-limited — an extra
compute dispatch for origin subtraction would waste more time on barriers than
it saves. A single `vec3` subtract per vertex in the shader is effectively free.

## Architecture

### What lives where

| Data                              | Storage                              | Origin handling                                          |
| --------------------------------- | ------------------------------------ | -------------------------------------------------------- |
| Transform buffer (GPU)            | True world space, incremental        | Shader subtracts `worldOrigin` after `transformVertex()` |
| Prev transform buffer (GPU)       | True world space                     | Shader subtracts `prevWorldOrigin` for motion vectors    |
| Joint matrices (GPU)              | True world space                     | Shader subtracts `worldOrigin` after `skinMatrix * pos`  |
| Prev joint matrices (GPU)         | True world space                     | Shader subtracts `prevWorldOrigin`                       |
| Light positions (SceneBuffer)     | True world space, dirty-tracked      | Fragment shader subtracts `worldOrigin` for L vector     |
| Shadow VP matrices                | Built in camera-relative space (CPU) | Shadow pass recomputes every frame — free to shift       |
| Camera view matrix                | Built with camera at origin          | `renderLocation = vec3(0)`                               |
| `worldOrigin` / `prevWorldOrigin` | New fields in CameraUBO              | Uploaded every frame (already happens)                   |

### What does NOT change

- ECS Transform components — true world space on CPU
- Animation system — local-space transforms, completely unaffected
- Physics — true world space on CPU
- Game logic / Lua — all positions remain in true world space
- Transform upload frequency — still incremental, only active entities
- Light upload frequency — still dirty-tracked
- GPU buffer layouts — no new buffers
- Mesh vertex data (position buffer) — stays in local/model space

## CameraUBO Changes

```c
// Two new fields added to CameraUbo:
vec4 worldOrigin;        // xyz = current frame origin (camera world pos)
vec4 prevWorldOrigin;    // xyz = previous frame origin (for motion vectors)
```

## Per-Frame Flow

```
1. Camera system computes interpolated camera world position
2. prevWorldOrigin = last frame's worldOrigin
3. worldOrigin = camera world position
4. Build view matrix: lookAt(vec3(0), direction, up)   // camera at origin
5. Build VP from origin-relative view matrix + projection
6. Set renderLocation = vec3(0, 0, 0)
7. Upload CameraUBO (includes worldOrigin, prevWorldOrigin)
8. Shadow pass builds cascade VP matrices from origin-relative camera frustum
9. All rendering proceeds — shaders subtract origin from world positions
```

## Shader Changes

### Vertex shaders — subtract origin after worldPos computation

```glsl
// Non-skinned:
worldPos = transformVertex(position, transform.pos.xyz, transform.rot, vec3(transform.pos.w));
worldPos -= sceneBuffer.cameras[0].worldOrigin.xyz;

// Skinned:
worldPos = (skinMatrix * vec4(position, 1.0)).xyz;
worldPos -= sceneBuffer.cameras[0].worldOrigin.xyz;
```

### Depth prepass — motion vectors use previous origin

```glsl
worldPos     -= sceneBuffer.cameras[0].worldOrigin.xyz;
prevWorldPos -= sceneBuffer.cameras[0].prevWorldOrigin.xyz;

clipCurrent = VP_noJitter * vec4(worldPos, 1.0);
clipPrev    = prevVP_noJitter * vec4(prevWorldPos, 1.0);
// Motion vectors remain correct — each VP/position pair uses its own consistent origin
```

### Culling compute shaders — shift bounding sphere centers

```glsl
worldCenter -= sceneBuffer.cameras[0].worldOrigin.xyz;
```

### Fragment shaders — light position adjustment

```glsl
vec3 lightPosRel = light.positionAndRange.xyz - sceneBuffer.cameras[0].worldOrigin.xyz;
// Use lightPosRel for L vector and distance calculations
```

### Shadow vertex shaders — same origin subtraction

```glsl
worldPos -= sceneBuffer.cameras[0].worldOrigin.xyz;
gl_Position = shadowViewProjection[cascade] * vec4(worldPos, 1.0);
// Shadow VP built in camera-relative space on CPU, so this is consistent
```

## File Change List

### C files

| File                                                       | Change                                              |
| ---------------------------------------------------------- | --------------------------------------------------- |
| `c-engine/ecs/system/camera/Camera.h`                      | Add `worldOrigin`, `prevWorldOrigin` to CameraUbo   |
| `c-engine/ecs/system/camera/CameraSystem.c`                | Set origin each frame, build view matrix at origin  |
| `c-engine/renderer/vulkan2/pass/shadow/VulkanShadowPass.c` | Build cascade matrices from origin-relative frustum |
| `c-engine/data/.../shaders/includes/globalset.shader`      | Add worldOrigin/prevWorldOrigin to Camera struct    |

### Vertex shaders (add `- worldOrigin`)

| #   | File                                 | Notes                                |
| --- | ------------------------------------ | ------------------------------------ |
| 1   | `meshlet/meshlet.vert`               |                                      |
| 2   | `meshlet/depth_prepass.vert`         | + prevWorldOrigin for motion vectors |
| 3   | `meshlet/depth_prepass_terrain.vert` |                                      |
| 4   | `triangle/triangle.vert`             |                                      |
| 5   | `triangle/triangle_depth.vert`       | + prevWorldOrigin for motion vectors |
| 6   | `shadow/shadow_meshlet.vert`         |                                      |
| 7   | `shadow/shadow_triangle.vert`        |                                      |
| 8   | `oit/oit_accumulate.vert`            |                                      |
| 9   | `vegetation/vegetation.vert`         |                                      |
| 10  | `terrain/terrain.tese`               |                                      |
| 11  | `terrain/bake.vert`                  |                                      |

### Culling compute shaders (shift bounding sphere center)

| #   | File                                      |
| --- | ----------------------------------------- |
| 12  | `meshlet/culling.comp`                    |
| 13  | `meshlet/culling_phase2.comp`             |
| 14  | `triangle/triangle_culling.comp`          |
| 15  | `triangle/triangle_culling_phase2.comp`   |
| 16  | `vegetation/vegetation_scatter_cull.comp` |

### Fragment shaders (light position adjustment)

| #   | File                                  |
| --- | ------------------------------------- |
| 17  | `meshlet/meshlet.frag`                |
| 18  | `meshlet/meshlet_terrain.frag`        |
| 19  | `triangle/triangle.frag`              |
| 20  | `terrain/terrain.frag`                |
| 21  | `oit/oit_accumulate.frag`             |
| 22  | `reflection/meshlet_reflection.frag`  |
| 23  | `reflection/triangle_reflection.frag` |
| 24  | `vegetation/vegetation.frag`          |

### Post-process / reconstruction shaders (reconstruct camera-relative worldPos)

| #   | File                                      | Notes                                                       |
| --- | ----------------------------------------- | ----------------------------------------------------------- |
| 25  | `composite/composite.comp`                | invVP reconstruction gives camera-relative pos — consistent |
| 26  | `ssr/ssr.comp`                            | Ray marching in camera-relative space                       |
| 27  | `fsr/sky_velocity.comp`                   | Sky velocity from depth reconstruction                      |
| 28  | `fsr/reactive.comp`                       |                                                             |
| 29  | `fsr/reflection_velocity.comp`            |                                                             |
| 30  | `grid/vertex.vert` + `grid/fragment.frag` | Debug grid                                                  |

## Motion Vector Correctness

Each frame's positions use that frame's origin:

- `worldPos_current` = true world pos − `origin_current`
- `prevWorldPos` = true prev world pos − `origin_previous`
- `VP_current` built with `origin_current` as camera
- `VP_prev` was built with `origin_previous` as camera

The clip-space positions are identical to what they would be without recentering,
but computed with much higher precision. Motion vectors remain correct because
each VP/transform pair uses its own consistent origin.

## Shadow Correctness

The shadow pass builds orthographic VP matrices from camera frustum corners.
With the camera at origin, frustum corners are near origin. The shadow pass
already uses `{0, 0, 0}` as the light view origin — this naturally aligns
with camera-relative geometry.

Shadow vertex shaders subtract `worldOrigin` from worldPos before the shadow
VP multiply, matching the space the cascade matrices were built in.

## Performance Impact (1080 Ti)

- **GPU**: One vec3 subtract per vertex, one per light per fragment — effectively zero
- **CPU**: Zero additional work — view matrix at origin instead of camera pos, same cost
- **Memory**: 32 bytes added to CameraUBO (two vec4s)
- **Bandwidth**: No additional buffer uploads
- **Transform uploads**: Unchanged — still incremental, only active entities

## Risks and Mitigations

| Risk                                         | Mitigation                                                      |
| -------------------------------------------- | --------------------------------------------------------------- |
| Missing a shader → objects at wrong position | Systematic grep for `transformVertex`, `skinMatrix`, `worldPos` |
| Shadow matrices inconsistent                 | Shadow pass recomputes from camera frustum every frame          |
| Depth reconstruction in post-process         | `invVP * clipPos` naturally gives camera-relative worldPos      |
| Skybox affected                              | Skybox already strips translation from view matrix — unaffected |
| FSR temporal stability                       | Motion vectors remain correct — no change needed                |
| Grid pass                                    | Reconstructs worldPos from depth — camera-relative, consistent  |

## Estimated Scope

- ~4 C files modified
- ~30 shader files modified (mostly one-line additions)
- ~200 lines of code total
- Zero new GPU buffers or compute passes
