# Screen-Space Decal System Plan

## Goal

Add a reusable screen-space decal / ground-overlay system that can project textures onto any visible scene surface, not only terrain. Primary use cases:

- Ground AoE warning circles and spell effects for players/creatures
- Decals on castle floors, bridges, buildings, terrain, and large props
- Static/semi-static world overlays such as scorch marks, blood, mud, road/trail markings
- Later: Azgaar roads rendered via route decal segments

This should be a renderer feature, not Azgaar-specific.

## Why screen-space decals

Road meshes are likely to fight the Azgaar terrain shape: cells are mostly flat, borders are sloped, and ribbons would float/clip/z-fight unless heavily subdivided. A terrain splatmap would be good for static roads, but it would not solve spell indicators on non-terrain floors.

Screen-space decals project against the depth/normal buffers, so they can work on:

- Azgaar terrain
- normal terrain chunks
- castle floors and walls if desired
- large meshes and props
- boss arenas not made from terrain

## Renderer placement

Current Vulkan pass order in `c-engine/renderer/vulkan/Vulkan.c` includes:

```txt
culling/depth/hiz/shadow/contact/light_culling
terrain
azgaar_terrain
scene
skybox
OIT
SSR
volumetric
composite
FSR/bloom/final/UI/debug
```

Initial decal pass should be added after opaque geometry is available and before final/composite presentation. There are two possible paths:

### Option A: pre-lighting / G-buffer modification

Decals write into scene material buffers before lighting/composite.

Pros:
- Best physical integration: albedo/normal/roughness/emissive can affect lighting.
- Roads can look like part of terrain.

Cons:
- Requires understanding/modifying the renderer's G-buffer layout.
- More risk for first implementation.

### Option B: post-opaque color overlay

Decals blend into the current HDR color target using depth reconstruction.

Pros:
- Much simpler first version.
- Perfect for warning circles, telegraphs, magic effects, UI-like ground overlays.
- Can be implemented without changing material lighting.

Cons:
- Roads/spell marks are not truly lit by the scene unless shader approximates lighting.
- Pure color overlay is less integrated for permanent dirt/stone roads.

Recommended staged approach:

1. Implement **Option B** first: color/alpha/emissive overlay decals.
2. Later add optional **G-buffer/material decals** if roads or stains need lighting integration.

## Core design

Add a renderer-level decal API and a Vulkan decal pass.

Suggested files:

```txt
c-engine/renderer/decal/Decal.h
c-engine/renderer/decal/Decal.c
c-engine/renderer/vulkan/pass/decal/VulkanDecalPass.h
c-engine/renderer/vulkan/pass/decal/VulkanDecalPass.c
c-engine/data/pak_0_engine/shaders/pass/decal/decal.vert
c-engine/data/pak_0_engine/shaders/pass/decal/decal.frag
```

Register the pass in `Vulkan.c` after `vulkanScenePass` for the first overlay version, or after all opaque depth-writing passes but before `vulkanCompositePass` depending on the actual color/depth target ownership.

## Decal representation

Start with box/projector decals. The decal volume is an oriented box; fragments outside the box are discarded.

```c
typedef enum DecalProjectionAxis {
    DECAL_PROJECT_Y_DOWN, // ground/floor decals
    DECAL_PROJECT_Z_FORWARD,
    DECAL_PROJECT_CUSTOM,
} DecalProjectionAxis;

typedef enum DecalFlags {
    DECAL_FLAG_NONE          = 0,
    DECAL_FLAG_GROUND_ONLY   = 1u << 0, // reject steep normals
    DECAL_FLAG_EMISSIVE      = 1u << 1,
    DECAL_FLAG_DEPTH_FADE    = 1u << 2,
    DECAL_FLAG_WORLD_UV      = 1u << 3,
} DecalFlags;

typedef struct DecalInstance {
    vec3 position;
    vec3 halfExtents;   // x/y/z local box radius; for ground decals y is projection height
    versor rotation;
    vec4 color;
    u32 textureId;
    u32 flags;
    float opacity;
    float normalThreshold; // e.g. 0.35..0.75 for ground-only
    float edgeFeather;
    float uvScale[2];
    float time;
} DecalInstance;
```

For gameplay, expose helpers like:

```c
u32 decalAdd(const DecalInstance* decal);
void decalRemove(u32 handle);
void decalClearTransient(void);
void decalSubmitTransient(const DecalInstance* decal);
```

Use two categories:

- **Persistent decals**: roads, stains, map annotations.
- **Transient frame decals**: spell telegraphs, targeting indicators, debug overlays.

For first version, a frame-submitted transient array is enough. Persistent storage can come next.

## Shader algorithm

For each decal instance, draw a cube/box mesh or instanced unit cube.

Fragment shader:

1. Reconstruct world position from screen UV + depth.
2. Transform world position into decal local space using inverse decal matrix.
3. Discard if local position is outside `[-1, 1]` bounds.
4. Compute decal UV from local X/Z for ground decals.
5. Sample decal texture.
6. Apply edge feathering.
7. Optionally sample normal buffer and reject steep surfaces.
8. Blend into HDR color target.

Pseudo:

```glsl
vec3 worldPos = reconstructWorldPosition(screenUv, depth);
vec3 local = (decalInvModel * vec4(worldPos, 1.0)).xyz;
if (any(greaterThan(abs(local), vec3(1.0)))) discard;

vec2 uv = local.xz * 0.5 + 0.5;
vec4 tex = texture(decalTexture, uv * uvScale);
float edge = computeBoxEdgeFade(local, edgeFeather);
float alpha = tex.a * decal.opacity * edge;

if (groundOnly) {
    vec3 n = sampleWorldNormal(screenUv);
    if (n.y < normalThreshold) discard;
}

outColor = vec4(tex.rgb * decal.color.rgb, alpha * decal.color.a);
```

## Depth/normal inputs

Need to identify existing renderer resources for:

- depth buffer after terrain + scene opaque rendering
- camera inverse projection/view matrices
- optional normal buffer / G-buffer normal target
- HDR color target to blend into

If normal buffer access is inconvenient, first version can rely on only depth and a decal projection height. Ground-only rejection can be added once normal access is wired.

## Blending modes

Start with:

- alpha blend: `srcAlpha, oneMinusSrcAlpha`
- additive/emissive: `one, one`

Later add:

- multiply/darken for dirt roads and scorch marks
- normal decal mode
- roughness/albedo G-buffer mode

## Culling and performance

Initial version can brute-force hundreds/thousands of decals if simple enough, but road decals may grow large. Add incrementally:

1. CPU frustum cull decal AABBs.
2. Sort by texture/blend mode.
3. Instance rendering with dynamic/storage buffer of `DecalInstanceGpu`.
4. Optional distance culling / LOD for roads.
5. Optional tile/cluster decal list if many dynamic effects become expensive.

## Constraints and edge cases

- Decals can project onto vertical surfaces unless normal filtering is used.
- Large decals with thin projection height can miss tall floor variations; road/spell decals should have enough vertical half extent.
- Overlapping decals need ordering or blend mode rules.
- Transparent/OIT surfaces may not receive decals in first version.
- Animated/skinned characters may receive decals unless normal/layer/object filtering is added. For warning circles, this may be acceptable or even useful; for roads, likely not.

## Layer/object filtering later

Eventually support masks:

```c
u32 receiverMask;
u32 decalMask;
```

This allows:

- roads only on world/static geometry
- AoE warnings on floors but not characters
- blood on characters/props if desired

First version can skip this.

## Implementation phases

### Phase 1: minimal visual decal pass

- Add decal data API.
- Add Vulkan decal pass.
- Draw instanced oriented boxes.
- Reconstruct world position from depth.
- Blend textured decals into HDR color.
- Test with one debug circle on terrain and one on a mesh floor.

### Phase 2: gameplay-friendly ground decals

- Add transient decal submission for spell AoE warnings.
- Add rotation, scale, color, opacity, lifetime/fade.
- Add edge feathering.
- Add texture atlas or texture array support.

### Phase 3: normal filtering and better integration

- Sample normal buffer if available.
- Add ground-only threshold.
- Add depth fade / slope fade.
- Add blend modes: alpha, additive, multiply/darken.

### Phase 4: road-scale performance

- Add persistent decal storage.
- Add CPU culling.
- Add batching by texture/blend mode.
- Stress test with Azgaar road/trail segment decals.

### Phase 5: material/G-buffer decals, optional

If roads/stains need proper lighting:

- Add G-buffer write mode for albedo/normal/roughness.
- Keep color-overlay mode for spell telegraphs.

## Validation

Use project workflow:

```bash
./scripts/build.sh
./scripts/run.sh play screenshot /tmp/decal-test.png
```

For runtime logs:

```bash
./scripts/run.sh play log 5000
```

Do not run `c-game` directly.
