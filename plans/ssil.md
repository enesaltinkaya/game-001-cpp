# Plan: Extend GTAO → GTAO+SSIL (Screen-Space Indirect Lighting)

## Goal

Add color bleed to the existing GTAO pass so nearby surfaces bounce colored
indirect light onto each other. This turns "dark corners" into "colored
corners" — e.g. a red wall bleeding light onto a white floor next to it.

## How SSIL Works (in our context)

At every horizon-search sample in GTAO, we already read depth to compute
occlusion. SSIL adds **one extra texture read** per tap: sample the
**previous frame's lit scene color** at that screen position, weighted by the
same occlusion geometry. The result is accumulated as an RGB color alongside
the scalar AO.

## Frame Synchronization

`FRAMES_IN_FLIGHT = 2`, but `sceneColor` is a **single image** (not
double-buffered). We cannot read it while the scene pass is writing to it.

**Solution**: allocate a dedicated `prevSceneColor` image. At the end of
each frame (after the scene pass is done and the pipeline barrier has fired),
**blit/copy sceneColor → prevSceneColor**. The GTAO shader then reads from
`prevSceneColor`, which is guaranteed complete.

Alternative: allocate `sceneColor` as `sceneColor[FRAMES_IN_FLIGHT]` so the
previous frame's buffer is still intact. This is a bigger refactor and
affects every pass that touches scene color — not worth it for this feature
alone.

---

## Changes

### 1. New resource: `prevSceneColor` + mip chain

**File**: `c-engine/renderer/vulkan/pass/gtao/VulkanGtaoPass.c`

Allocate a `prevSceneColor` image with mipmap support:

- Format: same as `sceneColor` (likely `R16G16B16A16_SFLOAT`)
- Usage: `STORAGE_BIT | SAMPLED_BIT | TRANSFER_DST_BIT | TRANSFER_SRC_BIT`
- Mip levels: 6–7 (for sampling at mip 5 = ~64px blur)
- Lifecycle: persists across frames, like `gtaoHistoryImages[]`

Add a `prevSceneColorValid` flag (0 until first copy completes).

### 2. Copy scene color after scene pass

**File**: `c-engine/renderer/vulkan/pass/gtao/VulkanGtaoPass.c` (or a shared
location if preferred)

After the scene pass finishes and a barrier ensures writes are visible,
issue:

```
vkCmdBlitImage(sceneColor → prevSceneColor)   // full-res copy
vkCmdGenerateMipmaps(prevSceneColor)           // downsample chain
```

The blit doubles as a resolve if sceneColor is MSAA. Mip generation can use
`vkCmdBlitImage` with linear filtering per mip level (standard Vulkan pattern).

**Timing**: This copy should happen early in the GTAO pass, right before
the raw GTAO dispatch. The scene pass has already completed by then (it
runs earlier in the frame).

### 3. Add reprojection matrix to camera UBO

**File**: `c-engine/ecs/system/camera/Camera.h` — `CameraUbo` struct

Add:

```c
mat4 reprojection; // prevViewProjection * invCurrentViewProjection
```

**File**: `c-engine/ecs/system/camera/CameraSystem.c`

Compute each frame:

```c
camera->cameraUbo.reprojection = mat4_mul(
    camera->cameraUbo.prevViewProjection,
    mat4_inverse(camera->cameraUbo.viewProjection));
```

This is the same pattern used for velocity — `prevViewProjection` is already
available in `CameraUbo`.

### 4. Modify `gtao.comp` — accumulate color

**File**: `c-engine/data/pak_0_engine/shaders/pass/gtao/gtao.comp`

Changes:

a) **New push constants**:

```glsl
uint prevSceneColorIndex;  // texture index of prevSceneColor
uint ssilEnabled;          // 0 or 1
```

b) **New output format**: change from `R16_SFLOAT` to `R16G16B16A16_SFLOAT`.

- `rgb` = SSIL color bleed
- `a` = scalar AO (same as before)

c) **Per-tap color accumulation** — inside the horizon search loop, after
computing the sample's view-space position:

```glsl
if (ssilEnabled != 0u) {
    // Reproject sample UV to previous frame
    vec2 sampleUv = (vec2(sampleCoord) + 0.5) / vec2(pc.width, pc.height);
    vec4 prevNdc  = sceneBuffer.cameras[0].reprojection * vec4(sampleUv * 2.0 - 1.0, linZ_normalized, 1.0);
    vec2 prevUv   = (prevNdc.xy / prevNdc.w) * 0.5 + 0.5;

    if (prevUv.x >= 0.0 && prevUv.x <= 1.0 && prevUv.y >= 0.0 && prevUv.y <= 1.0) {
        // Sample at mip 5 for ~64px blur (anti-firefly, soft indirect)
        vec3 bouncedColor = textureLod(
            sampler2D(textures[nonuniformEXT(pc.prevSceneColorIndex)], samplers[SAMPLER_CLAMP_LINEAR]),
            prevUv, 5.0).rgb;

        // Anti-firefly: tonemap before averaging
        bouncedColor /= 1.0 + dot(bouncedColor, vec3(0.299, 0.587, 0.114));

        // Weight by occlusion (same weight as AO accumulation)
        float occWeight = falloff * /* existing weight logic */;
        colorSum += bouncedColor * occWeight;
    }
}
```

d) **Output**:

```glsl
vec3 ssilColor = (colorWeight > 0.0) ? colorSum / colorWeight : vec3(0.0);
// Inverse tonemap
ssilColor /= 1.0 - dot(ssilColor, vec3(0.299, 0.587, 0.114));
ssilColor *= pc.strength * fadeOut;
imageStore(output, coord, vec4(ssilColor, ao));
```

### 5. Update `gtao_spatial.comp` — blur vec4

**File**: `c-engine/data/pak_0_engine/shaders/pass/gtao/gtao_spatial.comp`

Change from scalar filtering to vec4:

- Input/output format: `RGBA16F` instead of `R16F`
- Filter all 4 channels with the same edge-aware bilateral weights
- AO and color bleed are blurred together (they share the same edge structure)

### 6. Update `gtao_temporal.comp` — accumulate vec4

**File**: `c-engine/data/pak_0_engine/shaders/pass/gtao/gtao_temporal.comp`

Change from scalar history to vec4 history:

- History images: `R16G16B16A16_SFLOAT` instead of `R16_SFLOAT`
- Variance clamp applies to all channels (or just RGB for color, A for AO)
- Neighbourhood clamp in 3x3: operate on `vec4`
- Same disocclusion/velocity logic applies to all channels

### 7. Update image formats in C code

**File**: `c-engine/renderer/vulkan/pass/gtao/VulkanGtaoPass.c`

- `gtaoRawImage`: `R16_SFLOAT` → `R16G16B16A16_SFLOAT`
- `gtaoFilteredImage`: same
- `gtaoHistoryImages[]`: same

### 8. Apply SSIL in the scene shader

**File**: `c-engine/data/pak_0_engine/shaders/pass/scene/scene.frag`

Current AO application (line ~237):

```glsl
float ao = materialAo * screenAo;
// ...
vec3 color = (ambientDiffuse + ambientSpecular) * ao * shadowDarkFactor + ...;
```

New:

```glsl
float ao = materialAo * screenAo.r;  // .r because now RGBA, alpha or .r = AO

// SSIL color bleed (rgb channels of the same texture)
vec3 ssilColor = vec3(0.0);
if (sceneBuffer.shadow.aoImageIndex != 0u) {
    vec2 screenUV = gl_FragCoord.xy / sceneBuffer.cameras[0].viewport;
    vec4 aoSample = texture(sampler2D(textures[nonuniformEXT(sceneBuffer.shadow.aoImageIndex)],
                                      samplers[SAMPLER_CLAMP_LINEAR]),
                            screenUV);
    ao = materialAo * aoSample.a;  // AO in alpha channel
    ssilColor = aoSample.rgb;
}

// Apply: darken by AO, add color bleed
vec3 color = (ambientDiffuse + ambientSpecular) * ao * shadowDarkFactor
           + ssilColor * diffuseAlbedo
           + ...;
```

The `ssilColor * diffuseAlbedo` term simulates one bounce of indirect light:
the SSIL gives us the bounced irradiance, multiplied by the surface's
diffuse albedo gives the final contribution.

---

## Execution Order (within a frame)

```
1. Scene pass               → writes sceneColor
2. [Barrier]
3. Blit sceneColor → prevSceneColor   (NEW)
4. Generate mips on prevSceneColor     (NEW)
5. GTAO/SSIL raw pass       → reads depth + normals + prevSceneColor
6. [Barrier]
7. Spatial blur
8. [Barrier]
9. Temporal accumulation
10. [Barrier]
11. Remaining passes (SSR, composite, etc.) read AO+SSIL result
```

Steps 3–4 are the only overhead when the camera isn't moving much (SSIL
itself costs ~1–2ms extra over plain GTAO due to the extra texture reads).

---

## Risks / Considerations

- **Fireflies**: high-energy specular highlights in the prev frame cause
  bright speckles. Mitigated by mip-5 sampling and per-sample tonemapping.
  If still visible, add a soft clamp (e.g. `maxComponent > threshold`).

- **Feedback loop**: SSIL reads from the previous frame which itself
  contained SSIL. This is fine — it converges. But if artifacts amplify,
  feed back the scene color _before_ SSIL is applied (i.e. copy from the
  composite output before the SSIL term is added). This breaks the feedback
  loop entirely.

- **Half-resolution**: our GTAO runs at half-res. SSIL color is naturally
  low-frequency so half-res is fine, but the bilinear upscale when sampling
  in the full-res scene shader handles this.

- **Performance**: extra cost is ~32 texture reads per pixel (one per GTAO
  tap, at mip 5 which is cache-friendly). Expected ~1–2ms at 1080p.

---

## Files Changed Summary

| File                                                  | Change                                                     |
| ----------------------------------------------------- | ---------------------------------------------------------- |
| `c-engine/data/.../gtao/gtao.comp`                    | Add SSIL color accumulation, RGBA16F output                |
| `c-engine/data/.../gtao/gtao_spatial.comp`            | Vec4 bilateral blur                                        |
| `c-engine/data/.../gtao/gtao_temporal.comp`           | Vec4 temporal accumulation                                 |
| `c-engine/data/.../scene/scene.frag`                  | Apply SSIL color bleed + AO from RGBA                      |
| `c-engine/renderer/vulkan/pass/gtao/VulkanGtaoPass.c` | prevSceneColor alloc/mip, RGBA formats, new push constants |
| `c-engine/ecs/system/camera/Camera.h`                 | Add reprojection matrix to CameraUbo                       |
| `c-engine/ecs/system/camera/CameraSystem.c`           | Compute reprojection matrix                                |

## Reference

- Godot SSIL shader: `/home/enes/temp/godot/servers/rendering/renderer_rd/shaders/effects/ssil.glsl`
- Godot SSIL compositing: `scene_forward_clustered.glsl` lines 2189–2197
- Intel ASSAO (shared foundation): same sample pattern, radius calc, edge detection
