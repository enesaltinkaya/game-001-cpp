# GPU Optimization Plan

**Date:** 2026-05-23  
**Current GPU frame time:** ~12.2ms (83fps potential)  
**Target:** ~8-9ms (111-125fps potential)

## Current GPU Budget (from screenshot stats)

| Pass                                           | Time   | % of Total |
| ---------------------------------------------- | ------ | ---------- |
| **Terrain**                                    | 3.86ms | 31.6%      |
| **GTAO**                                       | 1.79ms | 14.7%      |
| **Shadow (CSM)**                               | 1.64ms | 13.4%      |
| Other (depth, culling, composite, bloom, etc.) | ~2.8ms | 23.2%      |

## 1. Terrain Pass (3.86ms → ~1.5-2.0ms) — Biggest Win

The terrain shader is extremely heavy. Per-fragment costs:

- **POM** with blurred height sampling: `POM_MAX_STEPS=8` × 5-tap blur = **40+ texture reads** just for the ray-march, plus 4 binary refinement steps × 5 taps = 20 more. ~60 texture fetches for POM alone.
- **Triplanar cliff sampling**: 3 planar projections × 2 textures (albedo+normal) = **6 extra texture reads**
- **Splatmap layers**: loops over active splat layers with texture lookups each
- **Grass FBM noise**: `grassFBM` with `grassGradientNoise` — expensive per-fragment noise computation with hash functions
- **Forward+ lights** evaluated in the terrain shader on top of the directional light

### Optimizations

- **Reduce POM step count**: `POM_MAX_STEPS=8` → `4`, `POM_MIN_STEPS=6` → `3`. The blurred height sampling already provides smooth results. Halving steps cuts ~30 texture fetches.
- **Reduce POM fade distance**: `POM_FADE_END=30m` → `20m` so POM kicks out sooner.
- **Skip grass noise for distant pixels**: The `grassFBM` runs at 0.08 and 0.35 frequency — add a distance fade similar to POM.
- **Skip triplanar when cliffBlend < 0.01**: Currently guarded but the `buildTerrainTBN` function runs unconditionally — move it inside the cliffBlend check.
- **Consider baking grass variation into a low-res world-space noise texture** instead of computing FBM per-fragment.

## 2. GTAO (1.79ms → ~1.0ms)

Already running at half resolution (good). 6 directions × 5 steps = 30 depth reads per pixel, plus thin-geometry detection.

### Optimizations

- **Reduce directions**: `NUM_DIRECTIONS=6` → `4` saves ~33% of the ray-march cost. AO is low-frequency so 4 directions with the temporal filter should be sufficient.
- **Reduce steps**: `NUM_STEPS=5` → `3` saves another 40%. Combined with the existing temporal filter, visual quality should hold.

## 3. Shadow CSM (1.64ms → ~1.0ms)

4 cascades × 3072×3072 shadow map, rendering all visible scene draws per cascade.

### Optimizations

- **Reduce shadow map size**: 3072 → 2048. For the current scene complexity this should be fine. Saves ~55% rasterization + bandwidth per cascade.
- **Reduce cascade count**: 4 → 3. The scene is outdoors with mostly distant terrain; cascade 3+ can often be merged.
- **Tighten `SHADOW_MAX_DISTANCE`**: Currently 100m. Reduce to 60-70m for less coverage on distant terrain.
- **Add per-cascade frustum culling** for scene draws (if not already done by the culling pass).

## 4. Quick Wins / Low-Hanging Fruit

- **`SHADOW_LAMBDA=0.95`** is very aggressive (nearly logarithmic), giving tiny near cascades and huge far ones. Setting to `0.85` gives more balanced split distribution and better texel utilization.
- **MSAA is OFF** (good) — keep it that way.
- **116 draw calls** is modest; the bottleneck is per-pixel shader cost, not draw overhead.

## Priority Order

1. **Terrain POM step reduction** (saves ~1.5ms, trivial change)
2. **Shadow map size reduction** (saves ~0.5ms, one constant)
3. **GTAO direction/step reduction** (saves ~0.5ms, two constants)
4. **Terrain grass noise distance fade** (saves ~0.3ms)

**Estimated total savings: ~3ms → GPU frame time drops from ~12ms to ~9ms (from 83fps potential to ~111fps).**
