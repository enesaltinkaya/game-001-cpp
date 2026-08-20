# Vegetation / Object Placement Map

## Overview

Object placement across the 8192×8192m terrain is controlled by painted
UDIM tile textures created in Blender. Each pixel's RGB channels encode
both the **type** and **density** of objects to spawn at that location.

## UDIM Tile Layout

- **Grid:** 10×10 UDIM tiles
- **Coverage:** each tile covers ~819×819 meters
- **Recommended resolution:** 1024×1024 or 2048×2048 per tile
  - 1024 → ~0.8m per pixel
  - 2048 → ~0.4m per pixel
- **Image format:** must be **Non-Color / Linear** (not sRGB) to avoid
  color space conversion artifacts when painting exact values
- Empty / fully black tiles are skipped during export

## Channel Encoding

Each channel (R, G, B) independently encodes object types using value
ranges. A value of `0` means nothing. Values `1–255` are divided into
ranges where each range represents a different type, and the position
within the range controls density.

### RGB Channels (R, G, B)

| Range   | Type Index | Density Range |
| ------- | ---------- | ------------- |
| 0       | —          | Nothing       |
| 1–50    | 0          | 2%–100%       |
| 51–100  | 1          | 2%–100%       |
| 101–150 | 2          | 2%–100%       |
| 151–200 | 3          | 2%–100%       |
| 201–255 | 4          | 0%–100%       |

### Alpha Channel (inverted — trees)

Alpha is **inverted**: the image starts fully opaque (`A=255` = no trees)
so that painted RGB colors remain visible in Blender. Erase alpha to
place trees.

| Range   | Type Index | Density Range |
| ------- | ---------- | ------------- |
| 255     | —          | Nothing       |
| 205–254 | 0          | 2%–100%       |
| 155–204 | 1          | 2%–100%       |
| 105–154 | 2          | 2%–100%       |
| 55–104  | 3          | 2%–100%       |
| 0–54    | 4          | 0%–100%       |

### Channel Assignment

| Channel | Category         | Example types                            |
| ------- | ---------------- | ---------------------------------------- |
| R       | Rocks / pebbles  | coastal pebbles, boulders, mossy rocks … |
| G       | Grass            | short grass, tall grass, dry grass …     |
| B       | Bushes / flowers | wildflowers, shrubs, ferns …             |
| A       | Trees            | oak, pine, birch, dead tree …            |

This gives **5 types × 4 channels = 20 object categories**, each with
continuous density control, using a single RGBA texture.

## Decoding (C)

```c
// Decode an RGB channel value into type index and density.
// Returns false when val == 0 (nothing to spawn).
static inline bool vegetation_decode(uint8_t val, int *type, float *density) {
    if (val == 0) return false;
    *type    = (val - 1) / 50;             // 0–4
    *density = ((val - 1) % 50) / 49.0f;  // 0.0–1.0
    return true;
}

// Decode the alpha channel (inverted) into tree type and density.
// Returns false when val == 255 (fully opaque = no trees).
static inline bool vegetation_decode_alpha(uint8_t val, int *type, float *density) {
    if (val == 255) return false;
    uint8_t inv = 254 - val;               // flip: 0→254, 254→0
    *type    = inv / 50;                    // 0–4
    *density = (inv % 50) / 49.0f;         // 0.0–1.0
    return true;
}
```

Usage per pixel:

```c
uint8_t r = pixel.r, g = pixel.g, b = pixel.b, a = pixel.a;
int type; float density;

if (vegetation_decode(g, &type, &density))
    spawn_grass(type, world_pos, density);

if (vegetation_decode(r, &type, &density))
    spawn_rocks(type, world_pos, density);

if (vegetation_decode(b, &type, &density))
    spawn_bushes(type, world_pos, density);

if (vegetation_decode_alpha(a, &type, &density))
    spawn_trees(type, world_pos, density);
```

## Blender Workflow

1. Create a UDIM tile image (10×10) with **Non-Color** color space and
   **Alpha = 1.0** (fully opaque).
2. Enter **Texture Paint** mode.
3. When painting RGB channels (rocks, grass, bushes), uncheck
   **Affect Alpha** on the brush so the image stays opaque and visible.
4. When painting trees, use the **Erase Alpha** brush to lower alpha.
   Lower alpha values = trees present (the viewport will show erased
   areas as transparent, giving visual feedback on tree placement).
5. Set the brush color to the desired channel/value:
   - e.g. `G = 25/255 ≈ 0.098` for grass type 0 at 50% density
   - e.g. `R = 51/255 ≈ 0.200` for rock type 1 at minimum density
6. Paint the desired areas.
7. Save the UDIM tiles; the exporter skips fully black ones.

## Runtime Tile Streaming

- Only tiles near the camera are loaded (typically a 3×3 neighbourhood).
- A tile cache prevents reloading on chunk boundary crossings.
- Tile index maps directly to world position:
  `tile(x,y)` covers `[x*819.2 .. (x+1)*819.2] × [y*819.2 .. (y+1)*819.2]` meters.
