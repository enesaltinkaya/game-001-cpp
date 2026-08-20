# Azgaar Deciduous Variants Plan

_Use all N objects in the authored `deciduous.dat` model as per-instance
variants, so deciduous trees stop looking uniform and boring. The count N is
deciduous-specific: other species (conifer, acacia, palm, cactus, ...) derive
their variant count from their own model files — the engine pass supports any
per-species count._

**Status:** proposed. Follow-on to `plans/azgaar-world-population.md`
(workstream B vegetation). The props system currently renders every
deciduous tree from a single procedural placeholder mesh; this plan swaps
in the N hand-authored objects from the Blender model and assigns one at
random per instance.

---

## Goal

The `deciduous.blend` model (packed to `models/props/deciduous.dat` by
`scripts/1-blender-scene.sh`) contains **N objects** (one per tree variant).
Today the azgaar
props system ignores the real model and draws every deciduous tree from the
procedural placeholder (`buildDeciduous` in `AzgaarProps.c`). This plan:

1. Extracts all N objects from the loaded scene.
2. Feeds them into the `azgaar_props` pass as **per-species mesh variants**.
3. Makes the scatter assign a random (deterministic) variant per instance,
   so neighbouring trees differ.

Everything stays **deterministic** (variant pick is a pure function of the
tile seed + texel, like the rest of the scatter) and **streaming-compatible**
(evicted tiles regenerate bit-identically).

---

## Current state (self-contained)

### Props architecture

- `c-game/game/azgaar/AzgaarProps.c` — CPU side: builds the merged
  species-mesh buffer (procedural placeholders via `buildAllMeshes`), the
  per-tile scatter (thread pool, D7), and the road-distance hash. Pushes
  results to the domain-agnostic engine pass.
- `c-engine/renderer/vulkan/pass/azgaar_props/VulkanAzgaarPropsPass.{h,c}` —
  the instanced renderer. One merged mesh buffer (`meshVbo`/`meshIbo`) with
  a per-species sub-range (`PropSpeciesMeta.indexOffset/indexCount`), plus
  per-tile instance buffers grouped by species. One instanced draw per
  `(tile, species)` pair.
- `PropInstance` (40 B) carries `pos`, `yaw`, `scale`, `color`, `phase`,
  `species`. `PropTileRange` carries `species`, `start`, `count`.
- The deciduous model is already loaded: `azgaarLoadDeciduousModel()` in
  `AzgaarProps.c` finds `deciduous.dat` via `dataManagerListFiles(".dat")`
  and loads it into `g_deciduousScene` (verified: 4,087,511 verts /
  14,622,099 indices / 24 draws).

### Scene → SceneVertex conversion reference

`c-engine/renderer/vulkan/scene/VulkanScene.c` shows how a scene's `Mesh`
components are packed into the interleaved `SceneVertex` layout
(position[3], normal[3], tangent[4], uv[2], joints, weights = 56 B):
positions from `prim->positions`, and normal/tangent/uv/joints/weights
unpacked from `prim->attributes[type]` gated by `prim->attributeMask`.
The extraction in this plan mirrors that packing.

---

## Design

### 1. Extract objects → merged mesh buffer (`AzgaarProps.c`)

- Iterate `g_deciduousScene`'s `Mesh` components (one per object, N total)
  via `getComponents(scene, Mesh)`.
- For each object, convert its primitives to `SceneVertex` + indices
  (mirror `VulkanScene.c` packing), and append to the merged props mesh
  buffer.
- Record each object's sub-range: `indexOffset`, `indexCount`, local AABB
  (`boundsMin`/`boundsMax`), `swayFactor`, `flags`.

### 2. Variant-aware data structures (`VulkanAzgaarPropsPass.h`)

- `PropInstance`: add `u32 variant` (struct grows 40 → 44 B; update the
  `_Static_assert(sizeof(PropInstance) == 44, ...)`).
- `PropTileRange`: add `u32 variant`.
- Replace the single-sub-range `PropSpeciesMeta` with a flat
  **`PropVariantRange`** table:
  ```c
  typedef struct PropVariantRange {
      u32 species;
      u32 variant;
      u32 indexOffset;
      u32 indexCount;
      float boundsMin[3];
      float boundsMax[3];
      float swayFactor;
      u32 flags;
  } PropVariantRange;
  ```
- New API: `vulkanAzgaarPropsSetVariants(const PropVariantRange*, u32 count)`
  (replaces `vulkanAzgaarPropsSetSpecies`).

### 3. Engine pass draw loop (`VulkanAzgaarPropsPass.c`)

- Store the variant table.
- For each `(tile, species, variant)` range, look up the matching
  `PropVariantRange`, bind the index buffer at that variant's `indexOffset`,
  push its bounds/sway, and draw.
- **No new GPU vertex attribute** is needed: the CPU binds the correct index
  offset per range, so `azgaar_props.vert`/`.frag` are unchanged. The
  `variant` field in `PropInstance` is a CPU-side grouping key only.

### 4. Scatter picks a variant (`AzgaarProps.c`)

- For each deciduous instance, deterministically pick a variant index
  `0..(count-1)` via `propsRand(tileSeed, tx, tz, salt)` (bit-identical on
  regeneration), where `count` is that species' variant count (N for
  deciduous; other species may have fewer or more).
- Group instances by `(species, variant)` into the tile's ranges (each range
  now carries a `variant`).

### 5. Wire into `azgaarPropsInit`

- Build the variant table from the extracted objects and push it via
  `vulkanAzgaarPropsSetVariants`.
- Fallback: if `deciduous.dat` is absent (no `g_deciduousScene`), keep the
  existing procedural placeholder mesh (one variant per species).

---

## Design choice: near vs far LOD

The N objects map to the **near** deciduous species
(`AZGAAR_PROP_DECIDUOUS`). For the **far** LOD
(`AZGAAR_PROP_DECIDUOUS_FAR`) we reuse the same N variants by default
(simplest); a lighter subset for distance is a possible future optimisation.

---

## Note: variant counts are per-species

The variant count is a per-species property, derived from the number of
objects in that species' model file. Deciduous has N; other species
(conifer, acacia, palm, cactus, ...) may have fewer or more. The
`PropVariantRange` table + `vulkanAzgaarPropsSetVariants()` support any
count, so nothing in the engine pass hardcodes a count.

---

## Files touched

| File                                                                 | Change                                                                                                                                 |
| -------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------- |
| `c-engine/renderer/vulkan/pass/azgaar_props/VulkanAzgaarPropsPass.h` | `PropInstance` +`variant` (44 B), `PropTileRange` +`variant`, new `PropVariantRange` table + `vulkanAzgaarPropsSetVariants()`          |
| `c-engine/renderer/vulkan/pass/azgaar_props/VulkanAzgaarPropsPass.c` | Store variant table; per `(species, variant)` range, bind index offset + push constants + draw                                         |
| `c-game/game/azgaar/AzgaarProps.c`                                   | Extract N objects → merged buffer + variant table; scatter picks a variant; group by `(species, variant)`; wire into `azgaarPropsInit` |
| `c-game/game/azgaar/AzgaarProps.h`                                   | Update API comments if the species-table signature changes                                                                             |

No shader changes (`azgaar_props.vert`/`.frag` unchanged).

---

## Verification

1. `./scripts/build.sh` — must compile with **no new warnings**.
2. `./scripts/run.sh play log 5000` — confirm the log shows the variant table
   built (N deciduous variants) and that deciduous trees render with visible
   variety.
3. Optional screenshot: `./scripts/run.sh play screenshot /tmp/deciduous.jpg`
   to visually confirm the N variants are distinguishable.
