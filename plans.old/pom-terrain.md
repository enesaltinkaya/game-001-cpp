# Parallax Occlusion Mapping (POM) for Terrain Textures

## Overview

Add POM to terrain detail textures so that displacement data (already stored in
the normal-map alpha channel) produces visible depth/relief at close range
without adding geometry. The effect fades out with distance to save performance.

## Current State

### Texture Packing
- **Albedo texture**: `RGB = albedo`, `A = roughness`
- **Normal texture**: `RG = tangent-space normal XY`, `B = AO`, `A = displacement (height)`
- Displacement is already read and used for:
  - Height-based splat blending (`heightBlendFactor()`)
  - Micro-shadow calculation (`microShadow()`)
  - Written to `outMaterial.z` for potential future use

### Rendering Paths (both need POM)
1. **Meshlet path** — `meshlet_terrain.frag` (primary, used for the meshlet-rendered terrain mesh)
2. **Tessellation path** — `terrain/terrain.frag` (tessellated heightmap terrain)
3. **Depth prepass** — `depth_prepass_terrain.frag/.vert` (depth + velocity only, no POM needed)

### Key Shader Inputs Available
- `inWorldPos` — camera-relative world position (perspective-interpolated)
- `inNormal` — world-space geometric normal
- `inTangent` — world-space tangent + handedness in `.w`
- `inUV` — mesh UV coordinates `[0,1]`
- View direction derivable from `sceneBuffer.cameras[0].position.xyz - inWorldPos`

### Terrain Texture Tiling
- `SPLAT_DETAIL_TILE = 1024.0` (meshlet terrain) / `512.0` (tess terrain)
- `DEFAULT_GRASS_DETAIL_TILE = 2048.0`
- `DEFAULT_CLIFF_DETAIL_TILE = 32.0` (triplanar, world-space `CLIFF_TRIPLANAR_SCALE`)
- Terrain UVs span a 4096m world, so detail textures repeat at very high frequency
- Tiled UV = `terrainUV * TILE_FACTOR`

---

## Implementation Plan

### Phase 1: POM Core Function (Shared Include)

**File**: `c-engine/data/pak_0_engine/shaders/includes/pom.shader`

Create a reusable POM include with:

```glsl
#ifndef POM_SHADER
#define POM_SHADER

// Tuning constants
#define POM_HEIGHT_SCALE      0.04   // world-space height scale (meters of apparent depth)
#define POM_MIN_STEPS         8      // minimum ray-march steps (distant / head-on)
#define POM_MAX_STEPS         32     // maximum ray-march steps (close / grazing)
#define POM_FADE_START        30.0   // distance (m) where POM begins fading
#define POM_FADE_END          60.0   // distance (m) where POM is fully off
#define POM_LOD_BIAS          0.0    // mip bias for height samples inside the loop

// Returns the parallax-offset UV and the final height for shadow/AO use.
// viewDirTS must be in tangent space (unnormalized is fine, will be normalized internally).
// uv is the initial tiled UV.
// heightScale is the POM depth in UV space.
//
// texIndex/samplerIndex identify the normal-map texture (height in .a channel).
// Returns: vec3(offsetU, offsetV, finalHeight)
vec3 parallaxOcclusionMap(uint texIndex, uint samplerIndex,
                          vec2 uv, vec3 viewDirTS, float heightScale,
                          float fadeFactor);

// Simpler variant that just returns the offset UV (most common use).
vec2 pomOffsetUV(uint texIndex, uint samplerIndex,
                 vec2 uv, vec3 viewDirTS, float heightScale,
                 float fadeFactor);
#endif
```

#### Algorithm Details

1. **Normalize view direction in tangent space**, then compute step count based
   on `abs(viewDirTS.z)` — more steps at grazing angles.
2. **Linear ray-march** from the surface downward in height, sampling the
   displacement texture (normal map `.a`) at each step. Step along UV by
   `viewDirTS.xy / viewDirTS.z * heightScale / numSteps`.
3. **Binary refinement** (3–4 iterations) once a crossing is found, to get
   sub-step precision without doubling the linear step count.
4. **Fade factor** (`fadeFactor`): linearly blend between the original UV and
   the POM-offset UV. Computed by the caller based on camera distance.
5. **Use `textureGrad()`** inside the loop with pre-computed `dFdx`/`dFdy` of
   the original UV — this avoids mip artifacts from the offset UV diverging
   across the quad. Compute gradients once before the loop.

#### Key Consideration: Height Scale in UV Space
Terrain detail textures tile at very high frequency. The POM `heightScale`
parameter needs to be expressed relative to the tiled UV step, not the raw
terrain UV. Since the view direction is computed in tangent space (which maps
to world space), and the tiled UVs tile over `TILE_FACTOR` repetitions of the
terrain UV, the height scale in tiled-UV space is:

```
heightScaleUV = POM_HEIGHT_SCALE * TILE_FACTOR / terrainWorldSize
```

But it's simpler to compute `viewDirTS` in the tangent space that corresponds
to the **tiled UV** (the TBN already maps world→tangent, and the tiled UV is
just a scaled version), so `heightScale` can stay as a simple ratio of
depth-per-UV-unit.

Actually, the cleanest approach: express `heightScale` as **world-space meters
of depth** and convert to tiled-UV-space in the caller:
```
float uvPerMeter = TILE_FACTOR / terrainWorldSize;  // e.g. 1024/4096 = 0.25
float heightScaleUV = POM_HEIGHT_SCALE * uvPerMeter;
```

But since the TBN is built from the **mesh UV** (not tiled UV), and
`viewDirTS` is derived from that TBN, the view direction in tangent space
corresponds to mesh UV rate. The tiled UV = meshUV * TILE_FACTOR. So the
height offset in tiled UV space is:
```
tiledOffset = meshUVOffset * TILE_FACTOR
```
This means we should either:
- (a) Compute `viewDirTS` using a TBN aligned to the tiled UV, or
- (b) Scale `heightScale` by `1.0 / TILE_FACTOR`

Option (b) is simpler. The POM function works in whatever UV space the caller
provides, and the caller passes the correct scale.

**Recommended approach**: Have the POM function operate on the tiled UV space
directly. The caller computes `viewDirTS` using the world-space TBN (which is
already built from mesh UV). Since tiled UV = meshUV × TILE_FACTOR, the view
direction in tiled-UV-space is the same direction but the height scale changes:
```
heightScaleTiled = POM_HEIGHT_SCALE_WORLD / (terrainSize / TILE_FACTOR)
                 = POM_HEIGHT_SCALE_WORLD * TILE_FACTOR / terrainSize
```
For `POM_HEIGHT_SCALE_WORLD = 0.15m`, `TILE_FACTOR = 1024`, `terrainSize = 4096m`:
```
heightScaleTiled = 0.15 * 1024 / 4096 = 0.0375
```
This is the ratio passed to the POM function.

### Phase 2: Integrate into `meshlet_terrain.frag`

#### 2a. Compute POM Parameters (before `sampleSplatTerrain`)

```glsl
// Build TBN for POM (same as used later for normal mapping)
mat3 TBN = buildTBN(geometryNormal, inTangent, inUV, inWorldPos);

// View direction in tangent space
vec3 V_world = normalize(sceneBuffer.cameras[0].position.xyz - inWorldPos);
vec3 viewDirTS = normalize(transpose(TBN) * V_world);

// Distance fade
float dist = length(sceneBuffer.cameras[0].position.xyz - inWorldPos);
float pomFade = 1.0 - smoothstep(POM_FADE_START, POM_FADE_END, dist);
```

#### 2b. Modify `sampleSplatTerrain` Signature

Add parameters for POM:
```glsl
SplatResult sampleSplatTerrain(vec2 meshUV, vec3 worldPos, Material material,
                                vec3 worldNormal,
                                vec3 viewDirTS,   // NEW
                                float pomFade);   // NEW
```

#### 2c. Restructure `sampleSplatTerrain` for POM

The terrain has **three distinct UV spaces** with different tiling rates:

| Layer         | UV expression                        | Tile rate |
|---------------|--------------------------------------|-----------|
| Grass         | `terrainUV × 2048`                   | 2048      |
| Cliff         | triplanar world-space                | ~32/4096  |
| Splat painted | `terrainUV × 1024`                   | 1024      |

A single POM pass can't serve all three UV spaces. The key insight is that
the height-based blending (`blendSplatResultsHeight`) makes transitions
sharp — at any given fragment one UV space dominates. The strategy:

**Determine the dominant layer, POM in that layer's UV space, apply offset
to all samples in that space.**

##### Restructured function flow

```
sampleSplatTerrain(meshUV, worldPos, material, worldNormal, viewDirTS, pomFade):
  1. Compute terrainUV, slope, cliffBlend
  2. Compute tiledUV (×1024) and tiledUVGrass (×2048)
  3. Pre-compute UV gradients for both spaces:
       dUVdx_splat  = dFdx(tiledUV)
       dUVdy_splat  = dFdy(tiledUV)
       dUVdx_grass  = dFdx(tiledUVGrass)
       dUVdy_grass  = dFdy(tiledUVGrass)
  4. Sample UDIM splat weights (cheap, one tex per group)
  5. Find dominant splat layer (highest weight) and its normal tex index
  6. Decide POM source and run ray-march:
       if (rawTotalWeight > 0.5 && dominantSplatNormalIdx != 0):
         // Splat dominates → POM in splat UV space
         tiledUV = pomOffsetUV(dominantSplatNormalIdx, SAMPLER_LINEAR,
                               tiledUV, viewDirTS, hScaleSplat,
                               pomFade, dUVdx_splat, dUVdy_splat)
       else if (cliffBlend < 0.5 && grassNormalIndex != 0):
         // Grass dominates → POM in grass UV space
         tiledUVGrass = pomOffsetUV(grassNormalIndex, SAMPLER_LINEAR,
                                     tiledUVGrass, viewDirTS, hScaleGrass,
                                     pomFade, dUVdx_grass, dUVdy_grass)
       else:
         // Cliff dominates (triplanar) → skip POM
  7. Sample grass layer at (possibly offset) tiledUVGrass
  8. Sample cliff layer (triplanar, no POM offset)
  9. Blend grass + cliff via height blend
  10. Sample splat layers at (possibly offset) tiledUV
  11. Blend splat over base via height blend
  12. Return final SplatResult
```

##### Height scale per UV space

```glsl
// Splat layers (×1024): one UV unit = 4096/1024 = 4m
// POM_HEIGHT_SCALE_WORLD = 0.15m → hScale = 0.15 / 4.0 = 0.0375
float hScaleSplat = POM_HEIGHT_SCALE_WORLD * SPLAT_DETAIL_TILE / 4096.0;

// Grass (×2048): one UV unit = 4096/2048 = 2m
// → hScale = 0.15 / 2.0 = 0.075
float hScaleGrass = POM_HEIGHT_SCALE_WORLD * DEFAULT_GRASS_DETAIL_TILE / 4096.0;
```

##### Why this works

- Where **splat paint is strong** (weight ≥ 0.5): the splat layer fully
  overrides the base. POM traces the dominant splat material's displacement.
  The UV offset applies to ALL splat layer samples (they all share
  `tiledUV = terrainUV × 1024`). Grass/cliff base is invisible.

- Where **splat paint is absent**: grass dominates on flat ground. POM traces
  grass displacement in its own UV space (×2048). The grass layer gets the
  offset. Cliff (triplanar) gets no POM — acceptable since cliffs are steep
  and usually viewed from further away.

- **Transition zone** (splat weight 0.3–0.7): the height-based blending
  already makes this narrow. One side's POM drives the parallax; the other
  side contributes little visually. Minor discontinuity here is imperceptible.

##### Cliff triplanar (skip POM in V1)

POM on triplanar requires 3 ray-marches (one per axis) or dominant-axis
selection. Skip for V1. Cliffs are steep surfaces where parallax displacement
is less noticeable. Consider dominant-axis POM for V2.

##### Splat layers: shared UV offset

All splat layers use the same `tiledUV` (×1024). The POM offset computed from
the dominant layer's displacement applies to every splat layer equally. This
is an approximation — ideally each layer would have its own parallax — but
since the height-blending already makes one layer dominant at each fragment,
the visual error is negligible.

```glsl
// After POM offset is applied to tiledUV:
for (uint ch = 0; ch < MAX_SPLAT_CHANNELS; ch++) {
    // All layers sample at the SAME POM-offset tiledUV
    vec4 albedoSample = sampleMaterialTextureGrad(albedoIdx, SAMPLER_LINEAR,
                                                   tiledUV, dUVdx_splat, dUVdy_splat);
    vec4 normalSample = sampleMaterialTextureGrad(normalIdx, SAMPLER_LINEAR,
                                                   tiledUV, dUVdx_splat, dUVdy_splat);
    ...
}
```

#### 2d. Depth-Correct Silhouettes (Optional Enhancement)

POM doesn't modify `gl_FragDepth` by default, which means silhouette edges
will show flat geometry. For terrain this is usually fine because:
- Terrain triangles are large and mostly horizontal
- POM depth is small (a few cm)
- Writing `gl_FragDepth` disables early-Z, hurting performance

**Recommendation**: Do NOT write `gl_FragDepth` in V1. Re-evaluate if
silhouette artifacts are visible.

### Phase 3: Integrate into `terrain/terrain.frag` (Tessellation Path)

Same logic as Phase 2, adapted for the tessellation terrain shader. The tiling
factor is `512.0` instead of `1024.0`, so:
```
heightScaleTiled = POM_HEIGHT_SCALE * 512.0 / 4096.0
```

The tessellation path already has a slightly different code structure but the
same `sampleSplatTerrain` function. Add the same `viewDirTS` + `pomFade`
parameters.

### Phase 4: Self-Shadowing Enhancement (Optional, V2)

POM self-shadowing traces a second ray from the POM intersection point toward
the light in tangent space. If it hits a higher point on the heightfield, the
fragment is in POM shadow.

```glsl
float pomSelfShadow(uint texIndex, uint samplerIndex,
                    vec2 uv, float currentHeight,
                    vec3 lightDirTS, float heightScale) {
    // March from current POM point toward light
    // If any sample is above the ray, shadow = 0
    // Use fewer steps (4-8) since this is a binary yes/no
}
```

This would replace or augment the existing `microShadow()` function. The
current `microShadow` is an approximation; POM self-shadow would be more
accurate but more expensive.

**Recommendation**: Keep `microShadow()` for V1. Add POM self-shadow in V2 if
visual quality warrants it.

### Phase 5: Distance LOD for Step Count

The POM function should reduce step count with distance:
```glsl
int numSteps = int(mix(float(POM_MAX_STEPS), float(POM_MIN_STEPS),
                       smoothstep(0.0, POM_FADE_START, dist)));
```

This naturally reduces cost at medium distance before the fade-out disables
POM entirely.

---

## Texture Gradient Handling (Critical)

Inside the POM loop, we march along UVs that diverge from the original
quad-coherent UVs. Using `texture()` inside the loop would compute incorrect
mip levels (and may cause artifacts from non-uniform control flow in the
driver's gradient computation).

**Solution**: Compute `dFdx`/`dFdy` of the tiled UV **before** the POM loop,
then use `textureGrad()` inside:

```glsl
vec2 dUVdx = dFdx(tiledUV);
vec2 dUVdy = dFdy(tiledUV);

// Inside POM loop:
float h = textureGrad(
    sampler2D(textures[nonuniformEXT(texIndex)], samplers[nonuniformEXT(samplerIndex)]),
    currentUV, dUVdx, dUVdy).a;
```

After POM, the offset UV is used with `textureGrad()` for the final albedo +
normal samples as well (to maintain correct mip selection).

**Important**: This requires changing `sampleMaterialTexture()` calls to use
`textureGrad()` variants when POM is active. Add an overload:
```glsl
vec4 sampleMaterialTextureGrad(uint texIndex, uint samplerIndex,
                                vec2 uv, vec2 ddx, vec2 ddy) {
    return textureGrad(
        sampler2D(textures[nonuniformEXT(texIndex)],
                  samplers[nonuniformEXT(samplerIndex)]),
        uv, ddx, ddy);
}
```

---

## Performance Budget & Mitigation

### Cost Estimate
- POM loop: ~16-32 texture samples (height channel only) per fragment
- At 1080p terrain coverage ~40%, that's ~830K fragments
- Each step is one `textureGrad()` call (the `.a` channel fetch is free when
  the full texel is already cached)
- Estimated cost: **0.3–0.8ms** on mid-range GPU at 1080p

### Mitigations
1. **Distance fade** (`POM_FADE_START=30m`, `POM_FADE_END=60m`) — POM only
   runs on nearby terrain. At 60m+ distance, parallax is imperceptible anyway.
2. **Adaptive step count** — fewer steps at distance and when viewing head-on
   (`viewDirTS.z` close to 1).
3. **Skip POM on cliff triplanar** — avoids 3× cost for steep surfaces.
4. **Single POM pass** for the dominant layer — one ray-march per fragment,
   not per splat layer. The dominant layer is determined by splat weight
   (if splat is strong) or slope (grass vs cliff fallback).
5. **Shared UV offset for all splat layers** — the POM offset from the
   dominant splat layer is reused by all other layers in the same UV space.
6. **No `gl_FragDepth` write** — keeps early-Z enabled.
7. **`textureGrad()` everywhere post-POM** — pre-computed gradients avoid
   mip artifacts and enable the driver to batch texel fetches efficiently.
8. **Consider making POM togglable** via a UBO flag or push constant, so it
   can be disabled on low-end hardware.

---

## File Changes Summary

| File | Change |
|------|--------|
| `shaders/includes/pom.shader` | **NEW** — POM ray-march functions |
| `shaders/pass/meshlet/meshlet_terrain.frag` | Add POM integration in `sampleSplatTerrain` |
| `shaders/pass/terrain/terrain.frag` | Add POM integration (same approach) |
| `shaders/pass/meshlet/meshlet.frag` | Add POM for splatmap path (optional, lower priority) |
| `shaders/includes/globalset.shader` | (No changes needed — height data already in textures) |
| `Renderer.h` / `TerrainBuffer` | (No changes — POM params can be `#define`s in shader) |
| `scripts/build.sh` | No changes needed (shader compilation already handles includes) |

### Files NOT Changed
- `depth_prepass_terrain.frag` — depth prepass doesn't need POM (no color output)
- `VulkanTerrainPass.c` — no C-side changes needed for V1 (POM params are shader constants)
- Shadow shaders — terrain shadow maps don't need POM

---

## Testing Plan

1. **Build**: `./scripts/build.sh` (compiles shaders)
2. **Visual check**: `./scripts/run.sh screenshot` — verify terrain close-up
   shows parallax depth on rock/grass textures
3. **Performance check**: Compare GPU frame times with/without POM (toggle via
   `#define POM_ENABLED 1/0`)
4. **Edge cases**:
   - Terrain at extreme grazing angles (should fade out gracefully)
   - Splat→grass transition zone (POM source switches between splat UV and
     grass UV — verify no visible pop/seam at the threshold)
   - Multiple splat layers painted at equal weight (dominant layer selection
     should be stable, not flickering between layers)
   - Camera very close to ground (maximum step count path)
   - Cliff surfaces (should NOT have POM in V1, only flat/moderate slopes)
   - Splat-painted cliff faces (POM skipped due to triplanar — should look
     acceptable without parallax)

---

## Implementation Order

1. Create `pom.shader` with the core POM function
2. Integrate into `meshlet_terrain.frag` (primary terrain path)
3. Build + screenshot test
4. Tune `POM_HEIGHT_SCALE`, step counts, fade distances
5. Port to `terrain/terrain.frag` (tessellation path)
6. (Optional) Add POM to `meshlet.frag` splatmap branch
7. (V2) POM self-shadowing
8. (V2) POM on cliff triplanar (dominant-axis only)
