# Bloom Implementation Plan — Phase 8c

_Kawase dual-filter bloom with soft-knee threshold_

---

## Overview

Bloom runs **after TAA** and **before the Final pass**. It reads TAA's `ResolvedColor` (R16G16B16A16_SFLOAT, linear HDR), applies a luminance threshold with soft knee, then performs a Kawase downsample/upsample chain, and produces a bloom texture that the Final pass composites during tonemap.

### Pipeline position

```
... → Composite → TAA → [Bloom] → Final → RmlUI
```

---

## Frame Resources

### New images in `VulkanFrameResources`

| Resource | Format | Size | Usage | Purpose |
|---|---|---|---|---|
| `BloomMipChain` | `B10G11R11_UFLOAT_PACK32` | half-res, 6 mip levels | `SAMPLED \| STORAGE` | Single image with mips 0-5 for the entire down/up chain |

**Why B10G11R11?** It's 4 bytes/pixel (vs 8 for R16G16B16A16), sufficient precision for bloom (which is purely additive glow), and halves bandwidth on the mip chain. No alpha channel needed.

**Why a single image with mips?** Avoids creating 12 separate images. Each mip level gets per-mip `VkImageView`s registered in the bindless pool (same pattern as `VulkanHiZPass`). The downsampling chain writes mip N by reading mip N-1; the upsampling chain writes mip N-1 by reading mip N and the previous down result.

### Mip chain dimensions (example at 1920×1080)

| Mip | Width | Height | Pixels |
|---|---|---|---|
| 0 | 960 | 540 | 518K |
| 1 | 480 | 270 | 130K |
| 2 | 240 | 135 | 32K |
| 3 | 120 | 68 | 8K |
| 4 | 60 | 34 | 2K |
| 5 | 30 | 17 | 510 |

---

## New Files

### C files

| File | Purpose |
|---|---|
| `c-engine/renderer/vulkan2/pass/bloom/VulkanBloomPass.h` | Header — `extern System vulkanBloomPass;` |
| `c-engine/renderer/vulkan2/pass/bloom/VulkanBloomPass.c` | Pass implementation |

### Shader files

| File | Purpose |
|---|---|
| `c-engine/data/pak_0_engine/shaders/pass/bloom/bloom_downsample.comp` | Prefilter (mip 0) + Kawase downsample (mips 1-5) |
| `c-engine/data/pak_0_engine/shaders/pass/bloom/bloom_upsample.comp` | 9-tap tent filter upsample + additive blend |

---

## Shader Details

### `bloom_downsample.comp`

- **Workgroup size:** 8×8
- **Push constants:**
  ```glsl
  uint srcIndex;        // sampled pool index of source (ResolvedColor or prev mip)
  uint dstIndex;        // storage pool index of destination mip
  uint srcWidth;
  uint srcHeight;
  uint dstWidth;
  uint dstHeight;
  float threshold;      // luminance threshold (e.g. 1.0)
  float softKnee;       // soft-knee width (e.g. 0.5)
  uint isPrefilter;     // 1 for mip 0, 0 for mips 1-5
  ```

- **Mip 0 (prefilter):** Reads `ResolvedColor` at half-res with bilinear sampling. Applies soft-knee threshold:
  ```glsl
  float brightness = max(color.r, max(color.g, color.b));
  float knee = threshold * softKnee;
  float soft = brightness - threshold + knee;
  soft = clamp(soft, 0.0, 2.0 * knee);
  soft = soft * soft / (4.0 * knee + 1e-5);
  float contribution = max(soft, brightness - threshold) / max(brightness, 1e-5);
  color *= contribution;
  ```

- **Mips 1-5 (downsample):** 13-tap Kawase downsample pattern (standard dual-filter kernel from Marius Bjørge's "Bandwidth-efficient rendering" / Jimenez SIGGRAPH 2014):
  ```glsl
  // 4 corner samples (weight 0.125 each), 4 edge samples (weight 0.25 each), 1 center (weight 0.5)
  // Total effectively a 6×6 box with better weighting than bilinear
  vec3 a = textureLod(src, uv + vec2(-1, -1) * texelSize, 0).rgb;
  vec3 b = textureLod(src, uv + vec2( 1, -1) * texelSize, 0).rgb;
  vec3 c = textureLod(src, uv + vec2(-1,  1) * texelSize, 0).rgb;
  vec3 d = textureLod(src, uv + vec2( 1,  1) * texelSize, 0).rgb;
  vec3 e = textureLod(src, uv, 0).rgb;
  // ...full 13-tap pattern
  color = e * 0.125 + (a+b+c+d) * 0.03125 + ... ;
  ```

- Writes to `imageStore(storageImages[dstIndex], coord, vec4(color, 1.0));`

### `bloom_upsample.comp`

- **Workgroup size:** 8×8
- **Push constants:**
  ```glsl
  uint srcIndex;        // sampled pool index of the lower (smaller) mip
  uint dstIndex;        // storage pool index of the upper (larger) mip to blend into
  uint curIndex;        // sampled pool index of current mip's downsample result (for additive blend)
  uint dstWidth;
  uint dstHeight;
  float bloomRadius;    // filter radius multiplier (default 1.0)
  float bloomIntensity; // blend weight per upsample step (e.g. 0.5-1.0)
  ```

- **9-tap tent filter** (3×3 bilinear taps with tent weighting):
  ```glsl
  //  1  2  1
  //  2  4  2  × (1/16)
  //  1  2  1
  vec3 upsampled = /* weighted sum of 9 bilinear taps */;
  ```

- **Additive blend:** Read current mip's existing value (from the downsample pass) and add the upsampled contribution:
  ```glsl
  vec3 existing = textureLod(sampler2D(textures[curIndex], samplers[SAMPLER_LINEAR]), uv, 0).rgb;
  vec3 result = existing + upsampled * bloomIntensity;
  imageStore(storageImages[dstIndex], coord, vec4(result, 1.0));
  ```

---

## C Pass Implementation — `VulkanBloomPass.c`

### Structure (follows `VulkanHiZPass` pattern)

```
#define BLOOM_MIP_COUNT 6

Static state:
- VulkanPipe downsamplePipe, upsamplePipe
- VulkanImage bloomImage              // single image, 6 mips, half-res
- VkImageView mipViews[BLOOM_MIP_COUNT]
- int mipSampledPoolIndex[BLOOM_MIP_COUNT]
- int mipStoragePoolIndex[BLOOM_MIP_COUNT]
- u32 cachedWidth, cachedHeight
```

### Lifecycle

**`added()`**
- Create two compute pipelines: `bloom_downsample` and `bloom_upsample`

**`preUpdate()`**
- Get `ResolvedColor` dimensions
- If size changed or first frame: destroy old bloom image/views, create new `BloomMipChain` (B10G11R11, half-res, 6 mips), create per-mip image views and register in bindless pool
- Reset GPU profiles

**`update()`**
1. **Read ResolvedColor** — already in `SHADER_READ_ONLY_OPTIMAL` from TAA
2. **Prefilter → mip 0:**
   - Transition mip 0 to `GENERAL`
   - Dispatch `bloom_downsample` with `isPrefilter=1`, reading `ResolvedColor`, writing mip 0
   - Barrier: mip 0 write → read
3. **Downsample chain (mips 1-5):**
   - For each mip N (1→5):
     - Transition mip N-1 to `SHADER_READ_ONLY_OPTIMAL` (if not already)
     - Transition mip N to `GENERAL`
     - Dispatch `bloom_downsample` with `isPrefilter=0`, reading mip N-1, writing mip N
     - Barrier: mip N write → read
4. **Upsample chain (mips 4→0):**
   - For each mip N (4→0):
     - Transition mip N+1 to `SHADER_READ_ONLY_OPTIMAL` (lower/smaller mip, source)
     - Transition mip N to `GENERAL` (for read+write since we blend)
     - Need to read mip N's downsample result + the upsampled mip N+1 → write combined into mip N
     - Dispatch `bloom_upsample` reading mip N+1 (upsampled source) + mip N (existing downsample), writing mip N
     - Barrier: mip N write → read
5. **Final state:** mip 0 ends up in `SHADER_READ_ONLY_OPTIMAL`, containing the full bloom result at half-res

**`removed()`**
- Destroy per-mip views, remove from pool, destroy bloom image, destroy pipelines

### Important: upsample read+write on same mip

The upsample pass needs to **read** mip N's downsample result and **write** the blended result back. Two approaches:

**Option A (simpler):** Use `imageLoad`/`imageStore` on the storage view with the mip in `GENERAL` layout. The shader reads via `imageLoad` and writes via `imageStore` to the same image. Since each thread reads and writes the same pixel, there's no race condition.

**Option B:** Use a separate temp image. More memory but avoids any concerns about same-image read/write.

→ **Go with Option A.** `imageLoad`+`imageStore` on the same pixel in the same dispatch is well-defined in Vulkan when each invocation only touches its own pixel.

---

## Final Pass Modification

### `VulkanFinalPass.c` changes

Add `bloomMip0SampledIndex` to `FinalPushConstants`:
```c
typedef struct FinalPushConstants {
    u32 colorTextureIndex;
    u32 bloomTextureIndex;  // NEW: bloom mip 0 sampled pool index (0 = no bloom)
    float bloomStrength;    // NEW: overall bloom mix strength (e.g. 0.04)
} FinalPushConstants;
```

### `final.frag` changes

After sampling `ResolvedColor`, before tonemapping:
```glsl
vec3 hdr = texture(...colorTextureIndex..., uv).rgb;

if (bloomTextureIndex != 0u) {
    vec3 bloom = texture(sampler2D(textures[nonuniformEXT(bloomTextureIndex)],
                                   samplers[SAMPLER_LINEAR]), uv).rgb;
    hdr += bloom * bloomStrength;
}

float exposure = sceneBuffer.cameras[0].exposure;
vec3 ldr = aces(hdr * exposure);
```

Note: bloom texture is half-res; bilinear sampling handles the implicit 2× upsample to full-res. The SAMPLER_LINEAR gives free hardware upscale.

---

## Pass Registration

In `Vulkan.c`, insert bloom between TAA and Final:

```c
addPass(&vulkanTaaPass);
addPass(&vulkanBloomPass);   // NEW
addPass(&vulkanFinalPass);
```

---

## Barrier Summary

| Step | Source | Destination | Layout transition |
|---|---|---|---|
| Prefilter | ResolvedColor (READ_ONLY) | Bloom mip 0 (GENERAL) | mip 0: UNDEFINED → GENERAL |
| Down mip N | Bloom mip N-1 (READ_ONLY) | Bloom mip N (GENERAL) | mip N-1: GENERAL → READ_ONLY, mip N: UNDEFINED → GENERAL |
| Up mip N | Bloom mip N+1 (READ_ONLY) + mip N (GENERAL via imageLoad) | Bloom mip N (GENERAL via imageStore) | mip N+1: GENERAL → READ_ONLY, mip N: READ_ONLY → GENERAL |
| Final read | Bloom mip 0 (READ_ONLY) | — | mip 0: GENERAL → READ_ONLY |

---

## Tunable Parameters

| Parameter | Default | Range | Notes |
|---|---|---|---|
| `threshold` | 1.0 | 0.0 – 5.0 | Luminance cutoff for bloom contribution |
| `softKnee` | 0.5 | 0.0 – 1.0 | Smooth transition below threshold |
| `bloomIntensity` | 0.8 | 0.0 – 2.0 | Per-upsample-step blend weight |
| `bloomStrength` | 0.04 | 0.0 – 0.5 | Final mix strength in `final.frag` |
| `bloomRadius` | 1.0 | 0.5 – 2.0 | Upsample filter radius multiplier |

These can be hardcoded initially and later exposed to the UI or a settings struct.

---

## Implementation Order

1. **Create bloom image + per-mip views** in `VulkanBloomPass.c` (test: image creates without validation errors)
2. **Write `bloom_downsample.comp`** — prefilter path only (mip 0), verify with RenderDoc
3. **Wire downsample chain** (mips 1-5), verify mip contents in RenderDoc
4. **Write `bloom_upsample.comp`** — 9-tap tent + additive blend
5. **Wire upsample chain** (mips 4→0), verify mip 0 contains accumulated bloom
6. **Modify `final.frag`** to add bloom, adjust `bloomStrength` to taste
7. **Register pass** in `Vulkan.c`, build and test
8. **Tune parameters** — adjust threshold, softKnee, intensity, strength

---

## Estimated File Sizes

| File | ~Lines |
|---|---|
| `VulkanBloomPass.h` | ~15 |
| `VulkanBloomPass.c` | ~300 |
| `bloom_downsample.comp` | ~80 |
| `bloom_upsample.comp` | ~60 |
| `final.frag` changes | ~10 |
| `VulkanFinalPass.c` changes | ~15 |
| `Vulkan.c` changes | ~3 |
| `VulkanFrameResources` changes | 0 (bloom image is self-contained in the pass) |

Total: ~480 new/modified lines.
