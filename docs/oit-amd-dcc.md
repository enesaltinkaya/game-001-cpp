# OIT AMD DCC Artifact Fix

## Symptom

Black square flickering on transparent objects, appearing every 2–3 seconds.
Only reproducible on AMD GPUs. Never seen on NVIDIA. Disappears when FSR is enabled.

## Root Cause

AMD Delta Color Compression (DCC) intermittently fails to correctly decompress tiles
for `VK_FORMAT_R8_UNORM` images used as color attachments with **multiplicative blending**
and **renderpass fast-clear** (loadOp = CLEAR).

The OIT reveal pass uses an unusual blend mode:

| Attachment       | Blend factors              | Operation            |
| ---------------- | -------------------------- | -------------------- |
| Accum (color 0)  | ONE / ONE                  | ADD (additive)       |
| Reveal (color 1) | ZERO / ONE_MINUS_SRC_COLOR | ADD (multiplicative) |

AMD's DCC applies aggressive compression to R8_UNORM tiles. The multiplicative
blend combined with `vkCmdBeginRendering` fast-clear can leave stale compressed
data in certain tiles. When the decompressor reads those tiles on a subsequent
frame, it produces zero (black) instead of the correct reveal value.

The ~2–3 second interval matches DCC cache eviction timing: tiles are fine until
they age out of the fast path and get re-read through a decompression path that
hits the corrupted metadata.

### Why FSR masked it

When FSR upscaling is active, the OIT images are allocated at the lower render
resolution. The changed tile dimensions shift the DCC tile alignment so the
problematic code path is not triggered.

## Fix

Changed the OIT reveal attachment format from `VK_FORMAT_R8_UNORM` to
`VK_FORMAT_R16_SFLOAT`.

AMD's DCC does not apply the same aggressive compression to float formats, so
the decompression bug is avoided entirely. The shader code (`out float
outReveal` in the fragment shader, `texelFetch(...).r` in the composite compute
shader) is format-agnostic and required no changes.

### Cost

1 extra byte per pixel for the reveal target (~2 MB at 1920×1080). Negligible
relative to the RGBA16F accum buffer already in use.

### Files changed

| File                                                          | Change                                                            |
| ------------------------------------------------------------- | ----------------------------------------------------------------- |
| `c-engine/renderer/vulkan/resources/VulkanFrameResources.c`   | `oitReveal` format: `VK_FORMAT_R8_UNORM` → `VK_FORMAT_R16_SFLOAT` |
| `c-engine/renderer/vulkan/pass/oit/VulkanOitAccumulatePass.c` | `colorFormat2`: `VK_FORMAT_R8_UNORM` → `VK_FORMAT_R16_SFLOAT`     |

## References

- McGuire & Bavoil, "Weighted Blended Order-Independent Transparency", 2013
- AMD DCC / fast-clear behavior with UNORM formats (vendor-specific)
