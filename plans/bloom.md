# Bloom Implementation Plan

_Kawase dual-filter bloom with soft-knee threshold_

---

## Overview

Bloom runs **after Composite** and **before FSR** (or Final when FSR is off). It reads `CompositeColor` (R16G16B16A16_SFLOAT, linear HDR), applies a luminance threshold with soft knee, performs a Kawase downsample/upsample chain on a half-res mip chain, and produces a bloom texture that the Final pass adds before tonemapping.

### Pipeline position

```
... → Composite → [Bloom] → FSR → Final → RmlUI
```

Bloom inserts between `vulkanCompositePass` and `vulkanFsrPass` in `Vulkan.c`.

---

## Existing assets

- Compiled SPV debug files already exist (from a prior attempt):
  - `shaders/pass/bloom/spv/bloom_downsample.comp.spv.debug`
  - `shaders/pass/bloom/spv/bloom_upsample.comp.spv.debug`
- No GLSL source files exist yet — they must be written fresh.
- No C pass implementation exists yet.
- No bloom image resources exist in `VulkanFrameResources` — the bloom image will be self-contained in the pass (same pattern as HiZ).

---

## Frame Resources

### Bloom image (self-contained in `VulkanBloomPass`, NOT in `VulkanFrameResources`)

| Resource | Format | Size | Usage | Purpose |
|---|---|---|---|---|
| `BloomMipChain` | `B10G11R11_UFLOAT_PACK32` | half-res, 6 mip levels | `SAMPLED \| STORAGE` | Single image with mips 0–5 for the entire down/up chain |

**Why B10G11R11?** 4 bytes/pixel (vs 8 for R16G16B16A16), sufficient precision for additive glow, halves bandwidth on the mip chain. No alpha needed.

**Why a single image with mips?** Avoids 12 separate images. Per-mip `VkImageView`s registered in the bindless pool (same pattern as `VulkanHiZPass`).

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
| `c-engine/renderer/vulkan/pass/bloom/VulkanBloomPass.h` | Header — `extern System vulkanBloomPass;` + getter for bloom output |
| `c-engine/renderer/vulkan/pass/bloom/VulkanBloomPass.c` | Pass implementation |

CMake uses `GLOB_RECURSE` so no `CMakeLists.txt` changes are needed.

### Shader files (GLSL → compiled by `scripts/shaders.sh`)

| File | Purpose |
|---|---|
| `c-engine/data/pak_0_engine/shaders/pass/bloom/bloom_downsample.comp` | Prefilter (mip 0) + Kawase downsample (mips 1–5) |
| `c-engine/data/pak_0_engine/shaders/pass/bloom/bloom_upsample.comp` | 9-tap tent filter upsample + additive blend |

### Modified files

| File | Change |
|---|---|
| `c-engine/renderer/vulkan/Vulkan.c` | Add `#include "renderer/vulkan/pass/bloom/VulkanBloomPass.h"` and `addPass(&vulkanBloomPass)` between composite and FSR |
| `c-engine/renderer/vulkan/pass/final/VulkanFinalPass.c` | Add bloom texture index + strength to push constants |
| `c-engine/renderer/vulkan/pass/final/VulkanFinalPass.h` | (no change needed — the pass header is minimal) |
| `c-engine/data/pak_0_engine/shaders/pass/final/final.frag` | Sample bloom texture and add before tonemapping |

---

## Shader Details

### `bloom_downsample.comp`

- **Workgroup size:** 8×8
- **Push constants:**
  ```glsl
  layout(push_constant) uniform PushConstants {
      uint srcImageIndex;   // sampled pool index of source (CompositeColor or prev mip)
      uint dstImageIndex;   // storage pool index of destination mip
      uint srcWidth;
      uint srcHeight;
      uint dstWidth;
      uint dstHeight;
      float threshold;      // luminance threshold (e.g. 1.0)
      float softKnee;       // soft-knee width (e.g. 0.5)
      uint isPrefilter;     // 1 for mip 0, 0 for mips 1-5
  };
  ```

- **Mip 0 (prefilter):** Reads `CompositeColor` at half-res with bilinear sampling. Applies soft-knee threshold:
  ```glsl
  float brightness = max(color.r, max(color.g, color.b));
  float knee = threshold * softKnee;
  float soft = brightness - threshold + knee;
  soft = clamp(soft, 0.0, 2.0 * knee);
  soft = soft * soft / (4.0 * knee + 1e-5);
  float contribution = max(soft, brightness - threshold) / max(brightness, 1e-5);
  color *= contribution;
  ```

- **Mips 1–5 (downsample):** 13-tap Kawase downsample pattern (dual-filter kernel from Marius Bjørge / Jimenez SIGGRAPH 2014):
  ```glsl
  vec3 color = textureLod(src, uv, 0).rgb * 0.125;
  color += textureLod(src, uv + vec2(-1, -1) * texelSize, 0).rgb * 0.03125;
  color += textureLod(src, uv + vec2( 1, -1) * texelSize, 0).rgb * 0.03125;
  color += textureLod(src, uv + vec2(-1,  1) * texelSize, 0).rgb * 0.03125;
  color += textureLod(src, uv + vec2( 1,  1) * texelSize, 0).rgb * 0.03125;
  // ...remaining 8 taps of the 13-tap pattern
  ```

- Writes: `imageStore(storageImages[dstImageIndex], coord, vec4(color, 1.0));`

### `bloom_upsample.comp`

- **Workgroup size:** 8×8
- **Push constants:**
  ```glsl
  layout(push_constant) uniform PushConstants {
      uint lowerMipSampled;   // sampled pool index of the lower (smaller) mip
      uint currentMipSampled; // sampled pool index of current mip's downsample result
      uint currentMipStorage; // storage pool index of current mip (read+write)
      uint width;
      uint height;
      float bloomIntensity;   // blend weight per upsample step (e.g. 0.5–1.0)
      float bloomRadius;      // filter radius multiplier (default 1.0)
  };
  ```

- **9-tap tent filter** (3×3 bilinear taps with tent weighting):
  ```glsl
  //  1  2  1
  //  2  4  2  × (1/16)
  //  1  2  1
  vec3 upsampled = /* weighted sum of 9 bilinear taps */;
  ```

- **Additive blend using `imageLoad`/`imageStore`** (Option A — same-image read+write is safe since each invocation only touches its own pixel):
  ```glsl
  vec3 existing = imageLoad(storageImages[currentMipStorage], coord).rgb;
  vec3 result = existing + upsampled * bloomIntensity;
  imageStore(storageImages[currentMipStorage], coord, vec4(result, 1.0));
  ```

---

## C Pass Implementation — `VulkanBloomPass.c`

### Structure (follows `VulkanHiZPass` pattern closely)

```c
#define BLOOM_MIP_COUNT 6

static VulkanPipe downsamplePipe;
static VulkanPipe upsamplePipe;
static VulkanImage bloomImage;
static VkImageView mipViews[BLOOM_MIP_COUNT];
static int mipSampledPoolIndex[BLOOM_MIP_COUNT];
static int mipStoragePoolIndex[BLOOM_MIP_COUNT];
static u32 cachedWidth, cachedHeight;

// Public getter — returns mip 0 sampled pool index for the final pass
int vulkanBloomPassGetOutputIndex(void);
```

### Lifecycle

**`added()`**
1. Subscribe to `EVENT_SWAPCHAIN_CREATED` for cleanup on resize
2. Create two compute pipelines: `bloom_downsample` and `bloom_upsample`

**`preUpdate()`**
1. Skip if `vulkan.skipFrame`
2. Get `CompositeColor` from frame resources
3. If size changed or first frame: destroy old bloom image/views, create new `BloomMipChain` (B10G11R11, half-res, 6 mips), create per-mip image views, register in bindless pool
4. Reset GPU profiles

**`update()`**
1. **Read CompositeColor** — already in `SHADER_READ_ONLY_OPTIMAL` from the composite pass
2. **Prefilter → mip 0:**
   - Transition mip 0 to `GENERAL`
   - Dispatch `bloom_downsample` with `isPrefilter=1`, reading `CompositeColor`, writing mip 0
   - Barrier: mip 0 write → read
3. **Downsample chain (mips 1→5):**
   - For each mip N:
     - Barrier: mip N-1 GENERAL → `SHADER_READ_ONLY_OPTIMAL`
     - Barrier: mip N UNDEFINED → `GENERAL`
     - Dispatch `bloom_downsample` with `isPrefilter=0`, reading mip N-1, writing mip N
     - Barrier: mip N write → read
4. **Upsample chain (mips 4→0):**
   - For each mip N (from second-to-largest down to 0):
     - Barrier: mip N+1 GENERAL → `SHADER_READ_ONLY_OPTIMAL` (source)
     - Barrier: mip N `SHADER_READ_ONLY_OPTIMAL` → `GENERAL` (read+write via imageLoad/imageStore)
     - Dispatch `bloom_upsample` reading mip N+1 (upsampled source) + mip N (existing downsample), writing mip N
     - Barrier: mip N write → read
5. **Final barrier:** mip 0 GENERAL → `SHADER_READ_ONLY_OPTIMAL`

**`removed()`**
1. Destroy per-mip views, remove from bindless pool
2. Destroy bloom image
3. Destroy pipelines

### Key reference: HiZ pass pattern

The HiZ pass (`VulkanHiZPass.c`) is the primary reference for:
- Single image with multiple mip levels
- Per-mip `VkImageView` creation via `vkCreateImageView`
- Bindless pool registration via `vulkanAddImageViewToPool()` / `vulkanAddStorageImageViewToPool()`
- Per-mip barrier management using raw `VkImageMemoryBarrier`
- Swapchain resize cleanup via `EVENT_SWAPCHAIN_CREATED`

---

## Final Pass Modification

### `VulkanFinalPass.c` changes

Update `FinalPushConstants`:
```c
typedef struct FinalPushConstants {
    u32 colorTextureIndex;
    u32 bloomTextureIndex;  // NEW: bloom mip 0 sampled pool index (0 = no bloom)
    float bloomStrength;    // NEW: overall bloom mix strength (e.g. 0.04)
    float casStrength;
    float contrast;
    u32 pad[3];
} FinalPushConstants;
```

In `update()`, set:
```c
extern int vulkanBloomPassGetOutputIndex(void);  // 0 if bloom not active

FinalPushConstants pc = {
    .colorTextureIndex = (u32)colorImage->sampledPoolIndex,
    .bloomTextureIndex = (u32)vulkanBloomPassGetOutputIndex(),
    .bloomStrength     = 0.04f,
    .casStrength       = 0.0f,
    .contrast          = CONTRAST,
};
```

### `final.frag` changes

Update push constant block and add bloom sampling before tonemapping:
```glsl
layout(push_constant) uniform PushConstants {
    uint colorTextureIndex;
    uint bloomTextureIndex;  // NEW
    float bloomStrength;     // NEW
    float casStrength;
    float contrast;
    uint pad[3];
};

// In main(), before tonemapping:
vec3 hdr = sampleSceneHdr(uv);
if (bloomTextureIndex != 0u) {
    vec3 bloom = texture(sampler2D(textures[nonuniformEXT(bloomTextureIndex)],
                                   samplers[SAMPLER_LINEAR]), uv).rgb;
    hdr += bloom * bloomStrength;
}
// Then pass hdr to tonemapAndContrast()
```

Since `final.frag` currently calls `sampleSceneHdr(uv)` inside `sampleDisplayLdr()`, the bloom addition needs to happen at the HDR stage. The CAS path that calls `sampleDisplayLdr()` will need to account for bloom too. The cleanest approach:

1. Add a `sampleSceneHdrWithBloom(uv)` function that adds bloom to the HDR sample
2. Use it in both the CAS path and the non-CAS path
3. CAS continues to operate in LDR space after tonemapping (unchanged)

---

## Pass Registration

In `Vulkan.c`, insert bloom between Composite and FSR:

```c
#include "renderer/vulkan/pass/bloom/VulkanBloomPass.h"
// ...
addPass(&vulkanCompositePass);
addPass(&vulkanBloomPass);   // NEW
addPass(&vulkanFsrPass);
addPass(&vulkanFinalPass);
```

---

## Barrier Summary

| Step | Source | Destination | Layout transition |
|---|---|---|---|
| Prefilter | CompositeColor (READ_ONLY) | Bloom mip 0 (GENERAL) | mip 0: UNDEFINED → GENERAL |
| Down mip N | Bloom mip N-1 (READ_ONLY) | Bloom mip N (GENERAL) | mip N-1: GENERAL → READ_ONLY, mip N: UNDEFINED → GENERAL |
| Up mip N | Bloom mip N+1 (READ_ONLY) + mip N (GENERAL) | Bloom mip N (GENERAL) | mip N+1: GENERAL → READ_ONLY, mip N: READ_ONLY → GENERAL |
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

These will be hardcoded initially. Future work can expose them to the UI or a settings struct.

---

## Implementation Order

1. **Write `bloom_downsample.comp`** GLSL source → compile via `scripts/build.sh`
2. **Write `bloom_upsample.comp`** GLSL source → compile
3. **Create `VulkanBloomPass.h`** — minimal header
4. **Create `VulkanBloomPass.c`** — bloom image creation, mip views, bindless registration, dispatch loop, barriers
5. **Register pass** in `Vulkan.c` between composite and FSR
6. **Modify `VulkanFinalPass.c`** — add bloom texture index + strength to push constants
7. **Modify `final.frag`** — sample bloom and add before tonemapping
8. **Build and test** — `./scripts/build.sh && ./scripts/run.sh`
9. **Tune parameters** — adjust threshold, softKnee, intensity, strength via screenshot verification

### Verification

Use screenshot verification at each milestone:
```bash
./scripts/run.sh screenshot /tmp/bloom_test.png
```

Use RenderDoc for debugging shader output at each mip level.

---

## Estimated File Sizes

| File | ~Lines |
|---|---|
| `VulkanBloomPass.h` | ~15 |
| `VulkanBloomPass.c` | ~300 |
| `bloom_downsample.comp` | ~80 |
| `bloom_upsample.comp` | ~60 |
| `final.frag` changes | ~15 |
| `VulkanFinalPass.c` changes | ~10 |
| `Vulkan.c` changes | ~3 |

Total: ~480 new/modified lines.

---

## Differences from old plan (`plans.old/bloom.md`)

- **Pipeline position:** Old plan placed bloom after TAA. TAA has been removed; bloom now goes after Composite, before FSR.
- **Input image:** Reads `CompositeColor` instead of `ResolvedColor` (TAA output no longer exists).
- **File paths:** Use `c-engine/renderer/vulkan/pass/bloom/` (not `vulkan2`).
- **FSR interaction:** Bloom runs at render resolution before FSR upscales, so the bloom texture is at render-res (half of display-res when FSR is active). This is correct — bloom should be computed before upscaling.
- **Simplified final pass integration:** Bloom is added in HDR space before tonemapping in `final.frag`, rather than requiring a separate composite step.
