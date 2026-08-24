# SSPR (Screen Space Planar Reflection) — Summary

## What was done

Converted reflections from SSR to SSPR. The implementation uses a 3-pass compute pipeline:

1. **Project** (`sspr_project.comp`) — For each screen pixel above the reflection plane (`y = 0`), reconstruct world position, mirror it through the plane, project back to screen, and write depth via `atomicMax` into a buffer. Closest-to-camera wins.

2. **Resolve** (`sspr_resolve.comp`) — Re-run the same projection. If this pixel's depth matches the atomicMax winner at the target location, scatter the source pixel's scene color into the temp image (`ssrTexture`). Output is premultiplied RGBA (rgb × confidence, a = confidence).

3. **Hole-fill** (`sspr_holefill.comp`) — Reads the resolve output (sampled), fills empty pixels by averaging nearby non-empty neighbors (±4 vertical, ±2 horizontal, inverse-distance weighted), writes to the final output (`ssrTextureUpsampled`). This fixes horizontal band gaps caused by vertical stretching in the scatter.

### Files modified/created
- `c-engine/renderer/vulkan2/pass/sspr/VulkanSSPRPass.c` — Added `holefillPipeline`, `SsprHolefillPushConstants`, 3rd dispatch pass, uses `ssrTexture` as intermediate ping-pong target
- `c-engine/data/pak_0_engine/shaders/pass/sspr/sspr_holefill.comp` — New hole-fill shader

### Key details
- Reflection plane is hardcoded: `y = 0`, normal `(0, 1, 0)`, `d = 0`
- Uses `viewProjectionNoJitter` for stable projection
- Atomic buffer stores `floatBitsToUint(depth)` — works with reverse-Z (`atomicMax` = closest wins)
- `ssrTexture` (unused since old SSR is gone) serves as the temp resolve target
- Toggle with `Ctrl+R`

## Known limitation — occluded geometry

SSPR can only reflect what's visible on screen. If geometry is occluded from the camera's perspective (e.g., a character's neck hidden behind a large head), it won't appear in the reflection. The hole-fill pass can interpolate over small scatter gaps, but cannot invent data that was never rendered.

This was observed with the tree creature character (left) — neck area missing in reflection because the head occludes it from the camera angle.

## Future improvement — mirrored camera fallback

To fix the occlusion limitation, render the scene a second time from a camera mirrored through the reflection plane:

### How it works
- Mirror the camera position and orientation through the reflection plane (`y = 0`)
- Reflected view matrix: negate the camera's Y position and flip the up vector
- Render the scene from this mirrored viewpoint into a separate color texture
- Use an **oblique near-clip plane** at the reflection surface to avoid rendering below-plane geometry
- Use the mirrored render as a **fallback** where SSPR has no data (alpha ≈ 0)

### Cost reduction strategies
- Render at **half or quarter resolution**
- Use **lower LODs** and simplified shaders
- Skip particles, transparencies, small objects
- Only needed for the **primary reflection plane** — secondary surfaces keep SSPR + IBL fallback

### Architecture notes
- Requires a second scene submission (draw calls, vertex transforms, rasterization, shading)
- One mirrored render per unique reflection plane — if all reflective surfaces share `y = 0`, only one extra pass needed
- If reflective surfaces are on different planes (floor, wall, puddle at different heights), each unique plane needs its own mirrored render — gets expensive, so prioritize the most prominent one
- Composite logic: blend SSPR result where available, fill remaining gaps from mirrored camera texture, final fallback to IBL

### Implementation checklist (when ready)
- [ ] Create mirrored camera (reflect position + view matrix through plane)
- [ ] Set up oblique near-clip plane
- [ ] Render scene into a lower-res reflection texture
- [ ] Pass the mirrored texture to the SSPR resolve or composite
- [ ] Blend: SSPR (primary) → mirrored camera (gap fill) → IBL (final fallback)
