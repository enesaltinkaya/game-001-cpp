# Vegetation System Plan

## The Three Layers

| Layer                   | Source                                 | What it places                | Density                      | Draw method                        |
| ----------------------- | -------------------------------------- | ----------------------------- | ---------------------------- | ---------------------------------- |
| **Default grass**       | No splatmap paint + not steep          | Short grass blades            | High (fills the world)       | GPU scatter from terrain triangles |
| **Splatmap vegetation** | `grass*` / future `bushes*` UDIM paint | Styled grass, bushes, flowers | Medium (painted areas)       | Baked or GPU scatter from splatmap |
| **Trees**               | `trees*` UDIM paint                    | 3D tree models                | Low (hundreds, not millions) | Instanced mesh draw                |

These are fundamentally different problems, and trying to force them all through one pipeline will make a mess.

---

## Layer 1 — Default Grass Blades (the huge area)

**This is the compute scatter path, already built.** It needs one change: **splatmap rejection**.

How it works today:

- `vegetationMapSubmit()` buckets all terrain triangles by tile
- Compute shader scatters instances on non-steep triangles
- Draws grass blades via indirect draw

What to add:

- Pass the splatmap weight textures (already uploaded to GPU as `TerrainData.splatGroups[].weightTextures[]`) into the scatter compute shader
- For each scattered instance, compute its UDIM tile + local UV from world position (same math the terrain frag already does), sample the weight texture, and **reject if total paint weight > ~0.3**
- This means: unpainted flat ground → grass blades. Painted road/rock/custom grass → no default blades

Budget: ~150m radius, 8-16 instances/m², ~500K-1M visible after culling. Very doable.

## Layer 2 — Splatmap Vegetation (painted grass/bush styles)

**This is the baked path, already built.** The vegetation-builder already reads `grass1` UDIM PNGs and bakes instances.

Current state: only uses green channel. The design doc (`docs/vegetation-map.md`) describes R/G/B/A encoding with 5 types × 4 channels = 20 categories per splat group.

Plan:

- Extend the vegetation-builder to decode all 4 channels per group using the `vegetation_decode()` / `vegetation_decode_alpha()` functions from the design doc
- Store `channel` + `type` in `BakedInstance` (the fields already exist!)
- At runtime, use `channel + type` to select different blade meshes or visual styles in the vertex/fragment shader
- For `grass*` groups: different grass styles (tall, short, dry, flowers)
- For future `bushes*` groups: low bush/fern meshes (still billboards, just different shapes)

The distinction from Layer 1: Layer 1 is the "everywhere filler" at uniform density. Layer 2 is the "artist-painted specific grass types" at variable density. They overlap and that's fine — more density where painted.

## Layer 3 — Trees (instanced 3D models)

Trees are a completely different beast from grass. They need:

- Actual 3D meshes (not billboard strips)
- LOD (at 150m a tree needs maybe 50 triangles, up close maybe 5000)
- Shadow casting
- Much lower count (thousands, not millions)

### A. Tree model library scene

- Create a new `.blend` file (e.g. `vegetation-models.blend`) with tree models in an `export` collection
- Each tree is a named object: `tree_oak_lod0`, `tree_oak_lod1`, `tree_pine_lod0`, etc.
- Export via the existing `1-blender-scene.sh` pipeline → `.dat` file
- Load at startup in `VulkanVegetationPass.added()` via `sceneLoadCb()`

### B. Tree placement from splatmaps

- Add `trees*` as a new vegetation group type in `isVegetationGroupName()` (currently only `grass*`)
- The UDIM paint maps define where trees go (A channel = tree density per the design doc)
- The vegetation-builder bakes tree instance positions (same as grass, but at much lower density)
- Store a `category` field: grass vs tree, so the runtime knows which draw path to use

### C. Tree rendering

- Separate from the grass blade draw — trees use actual meshlet/indexed mesh rendering
- In the vegetation pass update, after uploading baked instances, split them: grass instances → blade draw, tree instances → instanced mesh draw
- Each tree instance: position + rotation + scale + type index → selects which tree mesh + LOD to draw
- LOD selection based on distance to camera
- Draw with the same PBR material pipeline as regular scene meshes (they need shadows, proper lighting)

---

## Implementation Order

### Phase 1 — Get default grass working (Layer 1)

1. Remove the `return;` on line 483 of `VulkanVegetationPass.c`
2. Add splatmap weight textures to the scatter compute shader
3. Reject instances where splatmap weight > threshold
4. Tune density, blade mesh, fade distance
5. Visual verification

### Phase 2 — Multi-style painted grass (Layer 2)

1. Extend vegetation-builder to decode all 4 RGBA channels
2. Add 3-4 different blade mesh variants (stored in the same vertex buffer, different offsets)
3. Vertex shader selects mesh variant based on `type`
4. Fragment shader selects color palette based on `channel + type`
5. Paint `grass1` in Blender with different channel values, rebuild, verify

### Phase 3 — Trees (Layer 3)

1. Create `vegetation-models.blend` with tree meshes + LODs
2. Add `trees*` group recognition to the pipeline
3. Extend vegetation-builder to bake tree placements at low density
4. Load tree model scene in vegetation pass
5. Implement instanced tree draw with LOD selection
6. Hook into shadow pass so trees cast shadows
7. Paint `trees1` in Blender, rebuild, verify

Phase 1 is probably a day of work — the infrastructure is all there. Phase 2 is a few days. Phase 3 is the big one, maybe a week, because instanced mesh rendering with LOD and shadows is real work.

---

## Key Files

- **Blender export:** `scripts/0-blender-terrain.py`
- **Offline bake tool:** `tools/vegetation-builder/main.cpp`
- **Runtime loading:** `c-engine/renderer/vulkan2/pass/vegetation/VegetationMap.c`
- **GPU pass:** `c-engine/renderer/vulkan2/pass/vegetation/VulkanVegetationPass.c`
- **Scatter compute shader:** `c-engine/data/pak_0_engine/shaders/pass/vegetation/vegetation_scatter_cull.comp`
- **Vertex shader:** `c-engine/data/pak_0_engine/shaders/pass/vegetation/vegetation.vert`
- **Fragment shader:** `c-engine/data/pak_0_engine/shaders/pass/vegetation/vegetation.frag`
- **Terrain frag (splatmap reference):** `c-engine/data/pak_0_engine/shaders/pass/meshlet/meshlet_terrain.frag`
- **Design doc:** `docs/vegetation-map.md`
- **Scene parser integration:** `c-engine/ecs/scene/SceneParser.c` (lines ~225, ~937, ~1650)
