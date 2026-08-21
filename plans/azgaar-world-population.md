# Azgaar World Population Plan

_Procedural vegetation, rivers, settlements and ambience from the `.map`
file's own data — so the world doesn't read as empty grass + ocean._

**Status:** proposed. Single live plan for all world-population work.
The former grass plan (`plans.old/azgaar-grass.md`, archived) is
superseded by it: its scatter architecture — clumped fBm gating,
per-tile budgets, wind sway — is folded into workstream B with the
parameters inlined below. Note that plan targeted the old
`AzgaarTerrain` batch-build architecture, which no longer exists —
terrain is now the engine's streaming `HeightmapTerrain`.

---

## Goal

Today the rendered world is: smooth heightmap terrain with a single default
grass texture (+ slope-based cliff triplanar), an animated ocean, and road
decals. It looks empty. This plan populates it using data that is already in
the `.map` file but not yet parsed:

1. **Terrain material blending** — snow line, beaches, rock/scree, biome
   tint (shader-only, no new geometry)
2. **Vegetation** — biome-specific trees + grass, instanced, clumped,
   wind-animated (species and density come from the biome table's `icons`
   / `iconsDensity` fields)
3. **Rivers** — the 187 real river polylines as animated water ribbons
4. **Settlements** — the 822 named burgs as procedural building clusters
   (walls, ports, temples, plazas per their feature flags)
5. **Props & landmarks** — rocks, shrubs, flowers, volcanoes, lighthouses,
   ruins, icebergs
6. **Ambience** — wind from the file's `winds[]`, weather particles
   (snow/dust/rain) chosen by the local climate

Everything is **deterministic** (pure function of world position + a seed
derived from the map name, like the existing height fBm detail) and
**streaming-compatible** (works with the engine's 5×5 `HeightmapTerrain`
window; evicted content regenerates bit-identically; nothing is persisted).

---

## Current state (self-contained)

### Rendering pipeline

- `vulkanHeightmapTerrainPass`
  (`c-engine/renderer/vulkan/pass/heightmap_terrain/`) renders a 5×5 window
  of 2048 m tiles (`HEIGHTMAP_TILE_SIZE_M`, `HEIGHTMAP_WINDOW_SIZE`).
  Per tile: 512² CPU height grid (4 m texels) built by a background
  builder thread from `HeightmapSource.heightAt`, R32F GPU height texture,
  Jolt heightfield. Fragment shader `heightmap_terrain.frag`: default grass
  texture (world-space tiling), slope-based triplanar cliff blend,
  micro-band normal perturbation, full PBR (sun GGX, IBL, contact shadows,
  forward+).
- `vulkanAzgaarWaterPass` (`.../pass/azgaar_water/`): camera-following
  8 km grid, depth-buffer-sampled Beer–Lambert absorption (shallow→deep
  tint), shore foam band, analytic sky reflection. `WaterData` uniform
  already carries `windAngle`.
- Decal pass: roads/trails from section 37 routes
  (`AzgaarRoadDecals.c`, 34 m / 12 m wide, `DECAL_FLAG_GROUND_ONLY` +
  `DECAL_FLAG_ROAD` union layer).
- Pass order in `c-engine/renderer/vulkan/Vulkan.c`:
  culling → depth → occlusion → hiz → shadow → contact_shadow →
  light_culling → **heightmap_terrain** → debug_navmesh → scene → skybox →
  **azgaar_water** → oit → ssr → volumetric → decal → composite → …
- Terrain textures are registered once via
  `getTextureByName("images/terrain/grass_default/albedo.ktx2")` +
  `vulkanResourceSetTerrainDefaults()` → `TerrainData` in the
  `SceneBuffer` (`shaders/includes/globalset.shader`, mirrored on the CPU
  in `VulkanResourceManager.h`). Candidate albedo assets already sit in
  `c-game/data/pak_1/images/terrain/` (wet-sand, Icelandic rock, dried
  grass, mossy forest floor, …).

### Parsed Azgaar world (`c-game/game/azgaar/AzgaarWorld.c`)

- params (size/seed), settings (unit/scale/height exponent), biome table
  (**name + color only**), grid points (100×100, 800 m spacing in the
  Chilerel map), heights, precipitation, temperature (precip/temp are used
  **only** to classify each cell's biome via FMG's `biomesMatrix`, then
  freed), per-cell biome, Voronoi cell polygons, `heightGrid` (800² float,
  Gaussian-blurred), `biomeColorGrid` (800² RGB, blurred, **computed but
  not consumed by any render pass today**), state/province names, routes.
- World scale: 800×800 px × 0.1 km/px = **80×80 km**, 100 m/px.
  `heightGrid` texel = 100 m.

### Load sequence (`c-game/game/loadingAzgaar/LoadingAzgaar.c`)

`azgaarWorldLoad` → `azgaarHeightmapSourceInit` → `heightmapTerrainInit`
→ `azgaarRoadCorridorBuild` → `azgaarRoadDecalsBuild` → background tile
generation → `azgaarWaterInit` → gameplay. Teardown is mirrored in
`loadingAzgaarReleaseWorld`.

---

## `.map` data inventory (47 CRLF sections)

Verified against `save.ts` (FMG source) and the Chilerel file. "Status"
refers to `AzgaarWorld.c` today.

| #   | Section                                                                                                                                                                                                  | Status                 | Used by                      |
| --- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------- | ---------------------------- |
| 0   | params (version, seed, width, height)                                                                                                                                                                    | parsed                 | —                            |
| 1   | settings (unit/scale/exponent + JSON: **`winds[6]`**, temperatureEquator, …)                                                                                                                             | partial                | F (wind)                     |
| 2   | map coordinates                                                                                                                                                                                          | —                      | —                            |
| 3   | biome table: name, color, **habitability, iconsDensity, `icons[]`, cost**                                                                                                                                | partial (name, color)  | **B** (species + density)    |
| 4   | regiments/notes                                                                                                                                                                                          | —                      | —                            |
| 5   | serialized SVG (single section, internal `\n`)                                                                                                                                                           | not parsed             | **C** (`<path id="riverN">`) |
| 6   | grid general (spacing, points, **features**: ocean/island/lake ids)                                                                                                                                      | partial (points)       | G (lakes)                    |
| 7   | `grid.cells.h` heights (0–100, 20 = sea level)                                                                                                                                                           | parsed                 | —                            |
| 8   | `grid.cells.prec` precipitation                                                                                                                                                                          | parsed (classify only) | **A**, F                     |
| 9   | `grid.cells.f` waterbody id per cell                                                                                                                                                                     | —                      | G (lakes)                    |
| 10  | `grid.cells.t` **signed distance to coast** in cell units (+1 land coast, −1…−10 water, 0 interior/deep)                                                                                                 | —                      | A (beach ring), G            |
| 11  | `grid.cells.temp` temperature °C (Int8)                                                                                                                                                                  | parsed (classify only) | **A** (snow line), F         |
| 12  | packFeatures (landmass objects: type, firstCell, shoreline, area)                                                                                                                                        | —                      | G (optional)                 |
| 13  | cultures (name, color, type)                                                                                                                                                                             | —                      | D (building style)           |
| 14  | states (name, **color**, coa, …)                                                                                                                                                                         | partial (names)        | D (trim colour)              |
| 15  | **burgs**: `x`, `y`, name, population (thousands), group (capital/city/town/village/fort/monastery/caravanserai/trading_post/hamlet), `walls`/`citadel`/`port`/`plaza`/`temple`/`shanty`, state, culture | —                      | **D**                        |
| 16  | `pack.cells.biome` (authoritative; FMG moisture incl. river flux)                                                                                                                                        | —                      | P5 (refine biome field)      |
| 17  | `pack.cells.burg`                                                                                                                                                                                        | —                      | —                            |
| 18  | `pack.cells.conf`                                                                                                                                                                                        | —                      | —                            |
| 19  | `pack.cells.culture`                                                                                                                                                                                     | —                      | D (optional)                 |
| 20  | `pack.cells.fl`                                                                                                                                                                                          | —                      | —                            |
| 21  | population per pack cell                                                                                                                                                                                 | —                      | P5 (gameplay)                |
| 22  | `pack.cells.r`                                                                                                                                                                                           | —                      | —                            |
| 23  | (deprecated road)                                                                                                                                                                                        | —                      | —                            |
| 24  | `pack.cells.s`                                                                                                                                                                                           | —                      | —                            |
| 25  | `pack.cells.state`                                                                                                                                                                                       | —                      | —                            |
| 26  | `pack.cells.religion`                                                                                                                                                                                    | —                      | —                            |
| 27  | `pack.cells.province`                                                                                                                                                                                    | —                      | —                            |
| 28  | (deprecated crossroad)                                                                                                                                                                                   | —                      | —                            |
| 29  | religions                                                                                                                                                                                                | —                      | —                            |
| 30  | provinces                                                                                                                                                                                                | parsed (names)         | —                            |
| 31  | name bases                                                                                                                                                                                               | —                      | —                            |
| 32  | **rivers**: i, source, mouth, **discharge, length, width, widthFactor, sourceWidth**, parent, basin, name, `cells[]` (pack ids)                                                                          | —                      | **C**                        |
| 33  | (empty)                                                                                                                                                                                                  | —                      | —                            |
| 34  | fonts                                                                                                                                                                                                    | —                      | —                            |
| 35  | **markers**: icon, type (volcanoes, hot-springs, water-sources, ruins, mines, lighthouses, bridges, sacred-forests, …), x, y, cell, size — 1036 in Chilerel                                              | —                      | **E**                        |
| 36  | cellRoutes                                                                                                                                                                                               | —                      | —                            |
| 37  | routes (roads/trails/searoutes)                                                                                                                                                                          | parsed                 | B (clearing), D              |
| 38  | zones                                                                                                                                                                                                    | —                      | —                            |
| 39  | **ice**: 308 iceberg polygons + cellId + size                                                                                                                                                            | —                      | **G**                        |
| 40  | `pack.cells.good`                                                                                                                                                                                        | —                      | —                            |
| 41  | goods (e.g. Wood `distribution:"biome(5,6,7,8,9)"`)                                                                                                                                                      | —                      | P5 (gameplay)                |
| 42  | markets                                                                                                                                                                                                  | —                      | —                            |
| 43  | deals                                                                                                                                                                                                    | —                      | —                            |
| 44  | `pack.cells.market`                                                                                                                                                                                      | —                      | —                            |
| 45  | custom good icons                                                                                                                                                                                        | —                      | —                            |
| 46  | measurers                                                                                                                                                                                                | —                      | —                            |

**Headline:** the biome table literally lists which species to scatter in
each biome and at what relative density
(`icons:["conifer"]`, `iconsDensity:100` for Taiga;
`icons:[acacia×8, palm]`, 120 for Tropical seasonal forest;
`icons:[cactus×6, deadTree]`, 3 for Hot desert), and the file contains
exact positions for every settlement, river and landmark. Almost nothing
needs to be invented — it needs to be parsed and rendered.

---

## Design decisions

### D1 — Climate fields become static per-world textures, not per-tile data

Temperature, precipitation and the signed coast-distance field are
rasterized into grids at **`heightGrid` dimensions** at load time (same
nearest-cell rasterization + Gaussian low-pass the height grid uses —
refactor `azgaarSmoothHeightGrid` into a generic separable-Gaussian
helper over a float array and reuse it). They are uploaded **once per
world load** as two small static textures sampled by the terrain
fragment shader:

- `biomeColor` — R8G8B8, the existing `biomeColorGrid` (already computed
  and blurred; currently orphaned).
- `climate` — R8G8B8A8: R = temperature (Int8 in a byte), G =
  precipitation (u8), B = signed coast distance in **cell units**
  (Int8 in a byte, ±127 clamp), A = biome id (u8).

Rationale: the fields are static per map; a single 800×800 texture
(≈2.5 MB) + bilinear sampling is far cheaper than per-tile uniforms, and
world→texture UV is trivial from the already-stored
`TerrainData.worldMin/worldMax`. Memory for the CPU grids:
3×f32 + 1×u8 at 800² ≈ 9 MB — fine.

### D2 — Terrain blending happens inside the existing

`heightmap_terrain.frag` (no new pass)

Snow/beach/rock are per-fragment material blends, exactly like the
existing grass↔cliff slope blend. New texture indices join `TerrainData`
(`biomeColorIndex`, `climateIndex`, `snowAlbedoIndex`, `sandAlbedoIndex`).
Snow reuses the cliff-side rock texture is **not** wanted — rock uses the
existing `cliff_side_default` (already registered); snow and sand get new
assets (see D6).

### D3 — One unified instanced "props" system (grass, trees, rocks,

buildings, landmarks)

A single engine pass (`azgaar_props`) with one instance layout, a
per-species mesh table, and per-tile instance buffers. The biome table's
`icons`/`iconsDensity` drive species weights/density; buildings and
landmarks are just more species. Rationale: one scatter budget, one
culling path, one upload lifecycle, and buildings can share the same
distance-falloff + per-tile frustum culling as trees. (The old grass plan
is the architectural ancestor; its per-tile budget/falloff scheme is
kept.)

### D4 — River geometry comes from the SVG, not pack-cell reconstruction

Rivers' `cells[]` are **pack** cell ids, and pack cell positions are not
stored in the `.map` (FMG regenerates them via `reGraph()` — a
deterministic decimation of the grid that we have not ported). The SVG
section (5) however always contains the exact 2D river geometry as
`<path id="riverN" d="M…C…">` (188 paths in Chilerel; join to the 187
river JSON entries by `i`, default width for strays). Parsing the path
strings is ~100 lines (M/L/C tokenizer + cubic flattening) and is robust
across maps. **Decision:** SVG for geometry, JSON section 32 for metadata
(width/discharge/name). Porting FMG's `reGraph` (needed later for
`pack.cells.biome`, zone lookups, pack-level gameplay data) is deferred
to P5.

### D5 — Rivers are a thin ribbon mesh in their own pass, not decals

Decals project a texture onto terrain; they can't displace, animate flow,
or reflect the sky like the sea. A ribbon (2 verts per sample, y =
terrain height + 3–5 cm) with a shader derived from `azgaar_water.frag`
(same ripple/fresnel/sun-spec, UV scrolling along the tangent for flow)
reads as a real stream and reuses the proven water look. Rendered
between `heightmap_terrain` and `azgaar_water` (transparent, no depth
write). A `DECAL_FLAG_GROUND_ONLY` dark "wet earth" strip under the
ribbon (same pattern as road decals) ties it into the ground.

### D6 — Textures: 2 new engine assets, 1 reuse

- New `c-engine/data/pak_0_engine/images/terrain/snow_default/{albedo,normal}.ktx2`
- New `c-engine/data/pak_0_engine/images/terrain/sand_default/{albedo,normal}.ktx2`
  (convert from the `wet-sand_Albedo.png` already in
  `c-game/data/pak_1/images/terrain/`)
- Rock/scree: reuse `cliff_side_default` (already registered + triplanar
  sampling exists in the shader).
  Generated/converted in a one-off script; committed as ktx2 like the other
  terrain assets.

### D7 — Scatter runs on a small thread pool, triggered per tile

The old grass plan assumed a 5×5 batch rebuild hook that no longer
exists (the engine's `HeightmapTerrain` streams tiles individually on its
own builder thread). Instead: `AzgaarProps` polls the active
`HeightmapTerrain` each frame; any in-window tile that is
`HEIGHTMAP_TILE_READY` and not yet props-built gets a job on a dedicated
`threadPool` (c-utils `thread/Thread.h`; 2–4 workers, mirroring
`TransformSystem`'s pool). Scatter samples the tile's 512² CPU height
grid (same surface the mesh was built from → flush placement) plus the
world's climate/biome/coast grids. Instances are written into a
per-tile buffer; a short main-thread finalize uploads it (same
lifecycle as `vulkanHeightmapTerrainSetMesh`). Deterministic RNG seeded
by `(mapSeed, tileX, tileZ)` so eviction + regeneration is
bit-identical.

### D8 — Settlements flatten terrain through the height _source_

Burg clusters get a gentle plateau by extending
`AzgaarHeightmapSource.heightAt`: for each settlement within the tile's
extent, `y' = mix(y, flatY, 1 − smoothstep(0.55r, r, d))` where
`flatY` is the natural height at the settlement centre. This is a pure
function of world coordinates, so tiles regenerate identically, seam
checks stay valid, and physics/collision (Jolt heightfield) get the
plateau for free. Order vs roads: plateau first, then the road corridor
wins where they intersect (corridor sampling already runs after the
source on the decal path; for tile meshing the corridor hook is applied
after `heightAt` in the same way).

### D9 — All load-time parsing is additive and failure-tolerant

Every new section parse (burgs, rivers, markers, winds, ice) is
independent: a missing/malformed section logs a warning and leaves that
feature empty, exactly like the existing routes/biome parses. Nothing in
Phase 0–3 depends on the others — each workstream ships visually
verifiable on its own.

### D10 — No collision / navmesh changes in this plan

Trees and buildings are render-only in v1 (player walks through them).
Jolt convex bodies for near-field props + recast navmesh obstacle
regeneration are follow-ups (P5) — doing them here would block the
visual payoff for engine-side work.

### D11 — Placeholder-first species meshes with drop-in Blender

replacement

**Every species starts as a simple procedural placeholder mesh generated
in C at init** (the species table in B). The placeholders exist so the
scatter / budgeting / culling / wind systems can be built and tuned
before any art exists. They are _explicitly temporary_:

- `AzgaarProps` keeps a **per-species mesh registry**. At init, for each
  species key it tries to load `models/props/<key>.glb` (pak-relative
  path, e.g. `models/props/conifer.glb`) with **cgltf — already linked
  into c-game** (`CMakeLists.txt`, thirdparty). File present + parses →
  use it; missing/failed → generate the procedural placeholder and log
  which species are still placeholders.
- **Replacing a model later is a file drop, zero code changes:** draw it
  in Blender, export GLB (the same `export_yup=True` convention as
  `scripts/1-blender-scene.py`), save as `models/props/<key>.glb`,
  rebuild the pak (`./scripts/build.sh` packs assets), done. Mixing is
  per-species: 17 hand-drawn + 3 placeholders in the same world is a
  normal state during iteration.
- Loader convention (documented for the Blender work): first mesh only
  (join its primitives), positions + normals + UVs copied into the prop
  pass' `SceneVertex`-layout buffer (same 56 B format the water pass
  uses for its grid mesh), **real-world metre scale (1 Blender unit =
  1 m)**, **origin at the model base** (y = 0 at the ground contact
  point), +Y up (glTF Y-up is the engine's Y-up — no axis flip, same as
  the scene exporter), target tri counts: grass/reed/flower < 20,
  trees < 300, rocks < 60, buildings < 200. Materials are ignored in v1
  (the prop shader tints per instance); UVs are kept in the buffer so
  species albedo textures are a shader-only follow-up (P5), no buffer
  rework.
- Sway: the vertex shader computes the sway weight as
  `smoothstep(0,1,(y − minY)/(maxY − minY))²` (trunk ≈ 0, canopy = 1)
  from the mesh's own bounds — works identically for placeholders and
  hand-drawn models, no custom vertex attribute needed.
- Per-instance `scale` stays uniform: the registry stores
  `unitHeight` (1.0 m for placeholders, built unit-height; real height
  for hand-drawn meshes), and scatter computes
  `scale = targetMeters / unitHeight` — scatter code is identical either
  way.
- `_far` LOD variants (`conifer_far`, `deciduous_far`): optional file;
  missing → the near mesh is reused at far range.
- `ENGINE_AZGAAR_PROPS_DEBUG=1` logs per species: source
  (`glb`/`placeholder`), tri count, unitHeight — so a dropped file that
  fails to parse is visible immediately (fallback: placeholder + warn).

---

## Workstreams

### A — Climate fields + terrain material blending (Phase 0)

**Parse** (in `azgaarWorldLoadMap`, next to the existing height/prec/temp
code):

- section 10 (`grid.cells.t`) → per-cell signed coast distance
- section 9 (`grid.cells.f`) → per-cell waterbody id
- section 1 JSON → `winds[6]` (stored, used in F)
- keep `prec[]`/`temp[]` after classification (currently freed)

**Rasterize** into `heightGrid`-sized grids (nearest cell, then the
shared Gaussian blur): `tempGrid` (f32 °C), `precGrid` (f32),
`coastGrid` (f32, cell units; unmarked 0 resolved with the sign of
height vs sea level), `biomeGrid` (u8). CPU sample API:

```c
typedef struct AzgaarClimateSample {
    float temperature;   // °C
    float precipitation; // 0..100
    float coastCells;    // + land side / - water side, cell units (0 = n/a)
    u32   biome;         // biome id (13 = none/water fallback)
} AzgaarClimateSample;
void azgaarWorldSampleClimate(const AzgaarWorld* world,
                              float xPx, float yPx, AzgaarClimateSample* out);
```

**Upload** (new `VulkanResourceManager` setters, called from
`VulkanHeightmapTerrainPass.c`'s `setTerrainDefaults`-style path or
`LoadingAzgaar`): `biomeColor` (R8G8B8) + `climate` (R8G8B8A8) textures;
`TerrainData` gains `biomeColorIndex`, `climateIndex`,
`snowAlbedoIndex`, `sandAlbedoIndex` (+ CPU mirror in
`VulkanResourceManager.h`).

**Shader** (`heightmap_terrain.frag`, after the grass+cliff block):

```glsl
vec4  climate = texture(climateTex, mapUV);           // R=temp G=prec B=coast A=biome
vec3  biomeT  = texture(biomeColorTex, mapUV).rgb;
// 1) biome tint over the grass base (soft multiply, keeps texture detail)
baseColor = mix(baseColor, baseColor * (biomeT * 2.0), 0.55);
// 2) snow: FMG temperature already falls with altitude → altitude snow line for free
float t = (char)climate.r;                            // or stored 0..255 → °C
float snowT = 1.0 - smoothstep(azgaarSnowLo, azgaarSnowHi, t);   // defaults -1..+3
snowT = max(snowT, step(10.5, climate.a) * step(20.0, biomeHeight)); // glacier biome
snowT *= landMask;                                    // heightGrid >= 20 (via climate? or worldY > 0.2)
snowT *= 0.75 + 0.25 * noise(inWorldPos.xz * 0.02);   // edge breakup
baseColor = mix(baseColor, snowAlbedo.rgb, snowT);
// 3) beach: low land (worldY above sea level is inWorldPos.y here)
float beachT = smoothstep(2.5, 0.6, inWorldPos.y) * landMask;
baseColor = mix(baseColor, sandAlbedo.rgb, beachT);
// wet-sand band right at the waterline
baseColor = mix(baseColor, sandAlbedo.rgb * 0.55, smoothstep(1.2, 0.1, inWorldPos.y) * landMask);
// 4) rock/highlands: existing cliffBlend (slope) + altitude band from TerrainData.worldMax.y
float rockT = max(cliffBlend, smoothstep(0.55, 0.85, inWorldPos.y / worldMaxLandY));
// (rock albedo = the already-loaded cliff triplanar sample where available)
roughness = mix(roughness, 0.6, snowT);
roughness = mix(roughness, 0.85, beachT);
```

Snow normal: flat (skip snow normal map in v1; the micro-band noise still
applies, which reads well on snow). Env overrides:
`ENGINE_AZGAAR_SNOW_LO` / `ENGINE_AZGAAR_SNOW_HI` /
`ENGINE_AZGAAR_BEACH_H` (same pattern as `ENGINE_AZGAAR_HM_SIGMA`).

Also: **docs task** — update `docs/azgaar-terrain.md`'s section table
with the verified 47-section layout above.

**Done when:** screenshot shows biome-tinted terrain, white peaks with a
broken (non-straight) snow line, sandy beaches with a darker wet band,
rocky high slopes; zero new geometry; no new warnings; terrain pass
profile unchanged within ~0.5 ms.

### B — Vegetation + props scatter (Phase 1, the big one)

**Species set** (**placeholder-first** — simple procedural meshes
generated in C at init; each key has a drop-in
`models/props/<key>.glb` replacement path, see D11):

_Species keys are the asset filenames used for the later Blender
replacements (`models/props/<key>.glb`):_
`grass_tuft`, `conifer`, `conifer_far`, `deciduous`, `deciduous_far`,
`acacia`, `palm`, `cactus`, `dead_tree`, `reed`, `shrub`, `rock`,
`flower`, `hut`, `house`, `tower`, `wall`, `temple`, `dock`, `gate`,
`volcano`, `lighthouse`, `ruin_column`, `ruin_arch`, `mine_frame`,
`bridge`, `iceberg`.

| id  | species           | geometry                                    | base size (m) |
| --- | ----------------- | ------------------------------------------- | ------------- |
| 0   | grass tuft        | 3–5 crossed blades (5 v / 3 tris per blade) | 0.3–1.0       |
| 1   | conifer           | trunk + 2–3 stacked cones                   | 4–9           |
| 2   | conifer_far       | trunk + 1 cone (distance LOD)               | same          |
| 3   | deciduous         | trunk + 1–2 displaced icosahedron blobs     | 5–12          |
| 4   | deciduous_far     | trunk + 1 blob                              | same          |
| 5   | acacia            | trunk + flat wide disc canopy               | 6–10          |
| 6   | palm              | bent trunk + 6–8 flat fronds                | 5–8           |
| 7   | cactus            | cylinder + 1–2 arms                         | 1–2.5         |
| 8   | dead_tree         | 2–3 branch cylinders, no canopy             | 3–7           |
| 9   | reed              | 3 thin tall blades, sways more              | 0.8–1.5       |
| 10  | shrub             | small flattened blob                        | 0.4–1         |
| 11  | rock              | displaced icosahedron (2 sizes)             | 0.5–3         |
| 12  | flower            | single alpha-test quad, 4-colour table      | 0.2–0.5       |
| 13+ | buildings (see D) | hut/house/tower/wall/temple/dock/gate       | 4–15          |

(Buildings are placeholder-first too — same D11 registry, same
`models/props/*.glb` keys.)

All opaque except flowers (alpha-test, same pipeline flag as the planned
grass). Canopies are solid low-poly (no alpha) → no sorting, cheap.

**Instance layout (40 B, CPU/GPU identical):**

```c
typedef struct PropInstance {
    float pos[3];   // world position on the tile's CPU height grid
    float yaw;      // 0..2π
    float scale;    // base × jitter
    float color[3]; // biome tint × jitter (buildings: state/culture colour)
    float phase;    // wind phase 0..2π
    u32   species;  // id above
} PropInstance;
```

**Scatter** (per tile, on the props thread pool — D7):

1. Jittered ~8 m candidate lattice over the tile (seeded RNG
   `(mapSeed, tileX, tileZ)`).
2. For each candidate: sample the tile height grid (height, slope via
   finite differences) + `azgaarWorldSampleClimate` + biome colour.
3. Species weights from the biome table's `icons[]` (weighted by icon
   repetition, exactly like FMG's icon pick); base density from
   `iconsDensity` (table below). Roll per species slot.
4. Rejection gates: water (`y < 0.5 m`); slope > species limit
   (conifer 0.55, palm 0.2, cactus 0.35, grass 0.4, …); inside a
   settlement footprint (`d < r + 30 m`); within 20 m of a road
   centerline (routes polyline hash); inside a river ribbon
   (`d < width/2 + 4 m`) — river/road distances from load-time spatial
   hashes of the polylines (20 m buckets).
5. Boosts: riparian band (`d < 60 m` from a river in a forest biome:
   ×1.5 tree weight); clumping so vegetation grows in visible patches
   with bare ground between: two-octave fBm value noise (amplitudes
   0.6/0.3, base frequency 1/10 m, fixed seed `0x9E3779B9`) sampled in
   world space — an anchor candidate is kept when noise > 0.55 (~40% of
   positions), and a kept anchor spawns a tuft: 8–32 grass blades (or
   1–4 trees) packed in a ~1 m disc around it. Near-player guarantee:
   within 250 m of the build-time camera every anchor is kept (full
   meadow), fading to 0 by 600 m via a probability roll, so the spawn
   area is never bare.
6. Color: `biomeColorGrid` sample × per-instance jitter (hue ±0.06,
   scale ×0.7–1.3). Buildings: state colour for trim, culture index →
   palette variant.
7. Distance LOD: tiles whose centre is > 800 m from the build-time camera
   use the `_far` tree variants and 0.5× density.

**Density table** (instances/m² before gating; `× iconsDensity/120`):

| biome               | icons (Chilerel)            | base density                  |
| ------------------- | --------------------------- | ----------------------------- |
| 6/7/8 forests       | deciduous/acacia/palm/swamp | 0.025                         |
| 5 tropical seasonal | acacia, palm                | 0.020                         |
| 9 taiga             | conifer                     | 0.020                         |
| 4 grassland         | grass                       | 0.060 (tuft anchors)          |
| 3 savanna           | acacia                      | 0.003                         |
| 10 tundra           | grass                       | 0.002                         |
| 1/2 deserts         | cactus, dead_tree, dune     | 0.001 (cactus), 0.0002 (dead) |
| 12 wetland          | swamp→reed                  | 0.015                         |
| 0/11                | —                           | 0                             |

Plus non-biome: rocks 0.0005–0.002 where slope > 0.3 or `y > 0.6×maxLand`;
coastal pebbles 0.002 in the beach ring; flowers 0.004 in grassland.

**Budget:** per-tile dynamic instance buffer, cap **150k instances/tile**
(150k × 40 B = 6 MB; 25 tiles worst-case 150 MB, in practice far less —
ocean/ice tiles build nothing). Allocation order within a tile:
distance-weighted budget from the build-time camera
(`1/(1+(d/1200 m)²)`); warn-log when a tile hits
its cap.

**Engine pass** `azgaar_props` (new, registered right after
`vulkanHeightmapTerrainPass`): opaque, depth-write on, MSAA like terrain;
one draw per resident tile (25 draws), per-tile CPU frustum test
(v1), per-species mesh bind within the draw (instanced). Vertex shader:
`rotY(yaw) × (meshVert × scale)` + wind sway
(`sin(time×speed + phase) × vertexY × strength`, reed/grass sway most);
`AzgaarPropsData` uniform: wind dir/speed/strength, enabled. Fragment:
analytic Lambert + hemispheric (mirror of the old azgaar pass), species-12
uses alpha-test discard.

**Done when:** screenshot shows biome-distinct vegetation (dense conifer
forests in taiga, acacia savanna, cactus desert, reed wetland, grass
meadows with clumps), everything flush with the ground, visible sway,
deserts/glacier/ocean clean of vegetation, no trees inside settlements or
on roads; instance counts logged per tile; ≤ 6 MB per tile buffer.

### C — Rivers (Phase 2)

**Parse:**

1. Section 32 JSON → `AzgaarRiver` metadata (id, name, discharge, length,
   `width`, `widthFactor`, `sourceWidth`).
2. Section 5 SVG → for each `<path id="riverN">`, tokenize `d`
   (supports `M`, `L`, `C` + relative variants; Chilerel uses `M`+`C`),
   flatten each cubic at 16 samples → polyline in map px. Join by
   `i == N`; missing metadata → default width 4 m.

**Build (load time, whole map, ~40k verts total — trivial):**

- Map px → world (`azgaarMapToWorld`), resample to ~10 m spacing, lift
  with `azgaarWorldSampleHeightSmooth` (+3 cm).
- Width profile: `w(s) = lerp(sourceWidth, width, s/length)` × 100 m/px
  (SVG/JSON widths are in px) → clamp 2–40 m; main rivers read 10–30 m.
- Ribbon mesh: 2 verts/sample (±normal × w/2), per-vertex attributes:
  world pos, tangent (flow dir), `arcLen` (uv for scrolling), `widthF`.
- Spatial hash of river points (10 m buckets) → shared with B (riparian /
  clearing queries) and with E (bridge placement).

**Render:** new `azgaar_river` pass between `heightmap_terrain` and
`azgaar_water` (transparent, depth-write off, MSAA). Shader derived from
`azgaar_water.frag`: same ripple noise + fresnel + sun spec, but ripple
UVs advected along the per-vertex tangent at a speed ∝ discharge (visible
flow), plus a subtle dark "bed" tint from the depth-buffer height diff.
Z-fight guard at mouths: ribbon sits +3–5 cm above sea level and the sea
pass' shore foam covers the junction.

**Ground strip:** road-decal-style `DECAL_FLAG_GROUND_ONLY` + union-layer
decals along the centerline, dark wet earth colour
(`vec4(0.20, 0.16, 0.10, 0.35)`), width = ribbon width × 1.6. Reuses the
proven road decal pattern (`AzgaarRoadDecals.addSegment`).

**Done when:** 187 rivers visible as flowing ribbons with source-tapered
widths, no Z-fighting at sea mouths, wet strip reads in satellite view,
riparian tree band present in forests, river polyline hash query < 5 µs
typical.

### D — Settlements (Phase 3)

**Parse** section 15 → `AzgaarSettlement[]` (822 in Chilerel):

```c
typedef enum AzgaarBurgGroup {
    AZGAAR_BURG_CAPITAL, AZGAAR_BURG_CITY, AZGAAR_BURG_TOWN,
    AZGAAR_BURG_VILLAGE, AZGAAR_BURG_HAMLET, AZGAAR_BURG_FORT,
    AZGAAR_BURG_MONASTERY, AZGAAR_BURG_CARAVANSERAI,
    AZGAAR_BURG_TRADING_POST,
} AzgaarBurgGroup;

typedef struct AzgaarSettlement {
    u32   id;            // 1-based burg id
    char  name[48];
    float wx, wz;        // world (azgaarMapToWorld of x,y)
    float flatY;         // natural height at centre (for D8 plateau)
    float radiusM;       // footprint
    float populationK;   // thousands
    u32   group;         // AzgaarBurgGroup
    u32   flags;         // WALLS | CITADEL | PORT | PLAZA | TEMPLE | SHANTY
    u32   stateId, cultureId;
    float stateColor[3]; // from section 14
} AzgaarSettlement;
```

**Footprint & counts** (population in thousands):

- `radiusM = clamp(14 + 26·√popK, 12, 160)` (hamlet ~15 m, town ~50 m,
  capital ~120 m)
- `buildings = clamp(round(2 + 9·√popK), 3, 220)`
- Map-wide total lands at roughly 30–60k instances (one-time, cached).

**Cluster generation** (deterministic, load time):

- Layout rings: plaza (if flag) at centre; houses on jittered rings
  (poisson-ish: random angle + radius with min-distance retry); street
  yaw aligned to the nearest road direction when a road passes within
  `2r` (routes hash).
- Walls (if flag): ring of wall segments at `radiusM` with one 2-gate
  opening facing the nearest road/centre; corner towers every ~40 m.
- Citadel (if flag): 2–3 towers + keep at the back-centre.
- Temple (if flag): one temple building offset from centre.
- Port (if flag): search the height grid for the nearest water
  (`h < 20`) within `1.5r`; if found, 2–3 piers (dock instances) toward
  it, otherwise drop the port (log once).
- Variant/micro-variation: culture index → roof pitch + palette shift;
  state colour → band/trim colour; per-instance scale ×0.85–1.15.

**Building meshes** (procedural, species 13+): hut (box + gable roof),
house (box + gable, bigger), tower (cylinder + cone cap), wall (box,
optional merlons), temple (box + 4 columns + pediment), dock (posts +
deck), gate (2 towers + lintel). 40–120 tris each.

**Render:** same `azgaar_props` pass/species path as vegetation
(buildings are instances; per-tile frustum culling applies — a tile
without settlements builds none).

**Terrain plateau:** `AzgaarHeightmapSource.heightAt` sums the plateau
mix from D8 over settlements within the tile's extent (precomputed
settlement AABB list; ≤ a few per tile). `flatY` sampled at load time
before any plateau is applied.

**Names for the GUI (optional, cheap):** extend `zoneGui` to show
`"Nipona — Tauseia"` when the player is inside `radiusM + 30 m` of a
settlement (nearest-settlement query against 822 entries is trivial).

**Done when:** screenshot near a capital shows a walled town with citadel
and piers at the coast; village = 5–8 huts; plateau has no visible lip
(seam check still passes); no buildings intersect roads; streaming across
a tile border with a settlement stays crack-free.

### E — Props & landmarks (Phase 4)

From markers (section 35), the 3D-worthy subset:

| type           | count | representation                                                                                            |
| -------------- | ----- | --------------------------------------------------------------------------------------------------------- |
| volcanoes      | 1     | cone + crater prop species, grey rock, optional 2–3 animated smoke quads (billboard, alpha)               |
| lighthouses    | 1     | tower prop + emissive top (use `DECAL_FLAG_EMISSIVE`-style emissive on the instance? v1: plain white top) |
| hot-springs    | 1     | small pool decal + steam quad                                                                             |
| water-sources  | 1     | small pool decal                                                                                          |
| ruins          | 5     | 3–5 broken column/arch props, stone grey, half-buried                                                     |
| mines          | 4     | timber frame prop + rock pile                                                                             |
| bridges        | 1     | plank bridge prop across the river at that cell (river hash gives the span)                               |
| sacred-forests | 3     | no mesh — force a 300 m vegetation density ×3 disc (B hook)                                               |

Everything else (607 encounters, 110 dungeons, inns, brigands, …) is
gameplay content (P5), not world-filling visuals.

**Done when:** the volcano reads as a landmark from 5 km; ruins/mines
discoverable up close; bridge spans its river.

### F — Ambience & weather (Phase 4)

- **Wind:** store `winds[6]` from the settings JSON; v1 uses
  `winds[0]` (P5: seasons). Feeds: `AzgaarPropsData.wind` (sway
  direction/speed), `WaterData.windAngle` (set in `AzgaarWaterUpdate`
  instead of the current constant).
- **Weather state** (per frame, from the climate sample at the camera):
  | condition | rule |
  |-----------|------|
  | snowfall | temp < −1 °C |
  | drizzle | temp < 3 °C && prec > 60 |
  | dust storm | biome 1 && temp > 25 °C |
  | leaves | biome 6 (P5: autumn season) |
  | none | otherwise |
- **Implementation:** real GPU particles — full plan in
  `plans/azgaar-weather-gpu-particles.md` (replaces the originally
  sketched fullscreen-quad v1). One `azgaar_weather` engine pass
  (registered after `azgaar_water`, before `oit`): a compute shader
  integrates a persistent 65 536-particle buffer in a camera-following
  wrap volume (world-space positions, analytic fall + wind + turbulence,
  ground collision against the scene depth buffer, respawn with
  climate-rolled type + density roulette), then one instanced billboard
  draw renders snow discs / rain velocity streaks / dust blobs / spinning
  leaves with soft-particle depth fade. `WeatherData` in the
  `SceneBuffer` carries wind/type weights/density; the game-side
  `AzgaarWeather` module cross-fades it (~4 s) from the climate sample
  at the camera and feeds one shared gust value into
  `AzgaarPropsData.wind` + `WaterData.windAngle`.
- **Mist:** skipped until the fog infrastructure supports local height
  fog (existing `FogData` is global) — noted, not scheduled.

**Done when:** standing in taiga shows falling snow drifting with the
file's wind and occluded by tree canopies (flakes vanish under a
conifer, ground contact is a soft fade, not a clip); desert shows
ground-hugging drifting dust; no weather elsewhere; sway and
water-ripple direction match the flakes' drift (one gust source);
teleporting between climates cross-fades over ~4 s.

### G — Water & ice details (Phase 4, low priority)

- **Icebergs** (section 39, 308 polygons): centroid + size → flattened
  scaled icosahedron, instanced prop species, albedo `vec3(0.85, 0.90,
0.95)`, roughness 0.4, floating at sea level; CPU-filtered at load
  (only where nearest land biome is glacier/tundra or the water temp < 2
  °C) so warm seas get none.
- **Lake tint:** rasterize the waterbody id grid (section 9) into the
  climate texture's spare channel is impossible (channels are used) →
  separate 16×16 R8 texture if ever needed; **defer** — the existing
  depth-based shallow tint already differentiates lakes visually.
- **Foam:** existing shore foam is adequate; the `coastGrid` is available
  if a wider/wetter shoreline band is wanted later.

---

## New & modified files

### New files

| File                                                                                                       | Purpose                                                                                                                                                   |
| ---------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `c-game/game/azgaar/AzgaarRivers.{h,c}`                                                                    | Section 32 + SVG path parse, ribbon mesh build, river-point spatial hash                                                                                  |
| `c-game/game/azgaar/AzgaarSettlements.{h,c}`                                                               | Section 15/14 parse, cluster generation, settlement AABB list                                                                                             |
| `c-game/game/azgaar/AzgaarProps.{h,c}`                                                                     | Species meshes, scatter (D7), per-tile instance buffers, `azgaarPropsBuildStart/Finalize`, `AzgaarPropsData` push                                         |
| `c-engine/renderer/vulkan/pass/azgaar_props/VulkanAzgaarPropsPass.{h,c}`                                   | Engine pass: per-tile instance buffers + meshes, frustum culling, pipeline                                                                                |
| `c-engine/renderer/vulkan/pass/azgaar_props/azgaar_props.vert` / `.frag`                                   | Instance transform + wind sway; Lambert + hemispheric (+ alpha-test for flowers)                                                                          |
| `c-engine/renderer/vulkan/pass/azgaar_river/VulkanAzgaarRiverPass.{h,c}`                                   | Ribbon upload + draw                                                                                                                                      |
| `c-engine/renderer/vulkan/pass/azgaar_river/azgaar_river.vert` / `.frag`                                   | Flow-advected water shader (derived from azgaar_water)                                                                                                    |
| `c-engine/renderer/vulkan/pass/azgaar_weather/VulkanAzgaarWeatherPass.{h,c}`                               | GPU particle weather: compute update + instanced billboard draw (Phase 4, see `plans/azgaar-weather-gpu-particles.md`)                                    |
| `c-engine/data/pak_0_engine/shaders/pass/azgaar_weather/weather_update.comp` + `azgaar_weather.vert/.frag` | Particle simulation (wrap / fall / depth kill / respawn) + billboard & rain-streak rendering                                                              |
| `c-game/game/azgaar/AzgaarWeather.{h,c}`                                                                   | Climate→condition state machine, `WeatherData` cross-fade, gust-coherent wind wiring                                                                      |
| `c-engine/data/pak_0_engine/images/terrain/snow_default/{albedo,normal}.ktx2`                              | Snow asset                                                                                                                                                |
| `c-engine/data/pak_0_engine/images/terrain/sand_default/{albedo,normal}.ktx2`                              | Sand asset (from `wet-sand_Albedo.png`)                                                                                                                   |
| `c-game/data/pak_1/models/props/<key>.glb`                                                                 | **Initially absent** — placeholders render. Each hand-drawn Blender model lands as one file per species key (D11); no code change needed when they arrive |

(Both modules use `file(GLOB_RECURSE …)`, so new `.c` files are picked up
on reconfigure — `./scripts/build.sh` handles it.)

### Modified files

| File                                                                               | Change                                                                                                                                                                                                                                                                                                                               |
| ---------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `c-game/game/azgaar/AzgaarWorld.h/.c`                                              | New fields (climate grids, biome/feature/coast per cell, settlements, rivers, markers, winds, ice, biome table `icons`/`iconsDensity`/`habitability`/`cost`); new section parses (9, 10, 15, 32, 35, 39, winds in 1); generic separable-Gaussian refactor of `azgaarSmoothHeightGrid`; `azgaarWorldSampleClimate`; destroy() updates |
| `c-game/game/azgaar/AzgaarHeightmapSource.c`                                       | Settlement plateau (D8)                                                                                                                                                                                                                                                                                                              |
| `c-game/game/loadingAzgaar/LoadingAzgaar.c`                                        | Load-time wiring: rivers → hash, settlements → clusters, props init, climate texture upload, teardown mirrors                                                                                                                                                                                                                        |
| `c-engine/renderer/vulkan/Vulkan.c`                                                | Register `azgaar_props` (after heightmap_terrain) and `azgaar_river` (before azgaar_water); `azgaar_weather` later                                                                                                                                                                                                                   |
| `c-engine/data/pak_0_engine/shaders/includes/globalset.shader`                     | `TerrainData` += biomeColorIndex, climateIndex, snowAlbedoIndex, sandAlbedoIndex; new `AzgaarPropsData` block in SceneBuffer                                                                                                                                                                                                         |
| `c-engine/renderer/vulkan/resources/VulkanResourceManager.{h,c}`                   | CPU `VulkanSceneBuffer` mirror + setters for the above                                                                                                                                                                                                                                                                               |
| `c-engine/renderer/vulkan/pass/heightmap_terrain/VulkanHeightmapTerrainPass.c`     | Load snow/sand textures; descriptor layout gains the 2 new samplers                                                                                                                                                                                                                                                                  |
| `c-engine/data/pak_0_engine/shaders/pass/heightmap_terrain/heightmap_terrain.frag` | Biome tint + snow/beach/rock blends                                                                                                                                                                                                                                                                                                  |
| `c-game/game/azgaar/AzgaarWater.c`                                                 | `windAngle` from `winds[0]` (F)                                                                                                                                                                                                                                                                                                      |
| `c-game/game/zoneGui/ZoneGui.c`                                                    | (optional) settlement name in the zone readout (D)                                                                                                                                                                                                                                                                                   |
| `docs/azgaar-terrain.md`                                                           | Verified 47-section table (A)                                                                                                                                                                                                                                                                                                        |

---

## Data layouts (CPU/GPU must match)

### `TerrainData` additions (globalset.shader + VulkanSceneBuffer)

```glsl
struct TerrainData {
    uint grassAlbedoIndex;
    uint grassNormalIndex;
    uint cliffAlbedoIndex;
    uint cliffNormalIndex;
    vec4 worldMin;
    vec4 worldMax;
    uint splatGroupCount;
    uint pomEnabled;
    uint biomeColorIndex;   // NEW: R8G8B8
    uint climateIndex;      // NEW: R8G8B8A8 (temp/prec/coast/biome)
    uint snowAlbedoIndex;   // NEW
    uint sandAlbedoIndex;   // NEW
    SplatGroup splatGroups[MAX_SPLAT_GROUPS];
};
```

### `AzgaarPropsData` (new SceneBuffer block + CPU mirror)

```glsl
struct AzgaarPropsData {
    vec4 wind;        // xy = dir (unit), z = speed, w = strength
    vec4 density;     // xyz = global density multipliers (grass/tree/all), w = enabled
};
```

### `PropSpeciesMesh` (species registry, D11)

```c
typedef struct PropSpeciesMesh {
    char     key[24];      // "conifer", "hut", … — also the .glb filename
    bool     fromGlb;      // false = procedural placeholder
    u32      vertexOffset; // into the prop pass' merged SceneVertex buffer
    u32      vertexCount;
    u32      indexOffset;  // into the merged index buffer
    u32      indexCount;
    float    unitHeight;   // 1.0 m for placeholders, real height for .glb
    float    boundsMin[3]; // local (for sway-weight normalisation)
    float    boundsMax[3];
    u32      textureIndex; // 0 = tint-only (v1); P5: species albedo
} PropSpeciesMesh;
```

One merged vertex/index buffer per prop pass (all species concatenated —
the water pass' own-mesh pattern), one draw per (tile, species) pair.

### `AzgaarRiver`

```c
typedef struct AzgaarRiver {
    u32   id;         // SVG/JSON id
    char  name[48];
    float* points;    // world xyz, lifted, ~10 m spacing
    u32   pointCount;
    float* widths;    // per-point width in metres (sourceWidth→width profile)
    float  discharge; // drives flow speed
} AzgaarRiver;
```

---

## Shader sketches

### `heightmap_terrain.frag` (new block, see workstream A)

Blending order matters: grass base → biome tint → beach/sand → rock →
snow (snow last so peaks stay white over rock). Roughness follows the
same mix chain. All blend weights are `smoothstep`s with env-overridable
thresholds.

### `azgaar_props.vert`

```glsl
// in: mesh vertex (pos xy-plane local, y = height 0..1 for sway weight),
//     instance (PropInstance)
vec3 local = vec3(vPos.x, 0.0, vPos.z) * instance.scale;
local      = rotY(instance.yaw) * local + vec3(0.0, vPos.y * instance.scale, 0.0);
float sway = sin(sceneBuffer.time * props.wind.z + instance.phase)
           * props.wind.w * vPos.y * speciesSway(instance.species);
local.xz  += vec2(cos(props.wind.x), sin(props.wind.x)) * sway;
vec3 worldPos = instance.pos + local;
```

### `azgaar_props.frag`

Per-instance colour × (analytic Lambert + hemispheric), mirroring the old
`azgaar_terrain.frag`; `discard` only for species 12 (flowers). No IBL /
forward+ (grass-scale detail doesn't pay for it — same call as the old
azgaar pass).

### `azgaar_river.frag`

Copy of `azgaar_water.frag` with: ripple UVs = `worldPos.xz * k +
tangent * arcLen * flowSpeed`, no shore foam (handled by the sea pass at
mouths), bed tint from `(waterY − terrainY)` depth like the sea.

---

## Phased roadmap

| Phase | Scope                                                                                                                                                                                                                                                                              | Est. effort | Shippable on its own               |
| ----- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------- | ---------------------------------- |
| **0** | A: climate grids + textures + terrain blends + snow/sand assets + docs table                                                                                                                                                                                                       | 2–4 days    | ✅ (biggest visual delta per hour) |
| **1** | B: props system (grass + trees + rocks) + pass + scatter                                                                                                                                                                                                                           | 1–2 weeks   | ✅                                 |
| **2** | C: rivers (parse, ribbon, pass, wet strip, riparian)                                                                                                                                                                                                                               | 3–5 days    | ✅                                 |
| **3** | D: settlements (parse, clusters, plateau, GUI name)                                                                                                                                                                                                                                | ~1 week     | ✅                                 |
| **4** | E + F + G: landmarks, GPU particle weather (`plans/azgaar-weather-gpu-particles.md`), wind wiring, icebergs                                                                                                                                                                        | ongoing     | per item                           |
| **5** | (out of scope, listed for continuity): seasons, prop collision + navmesh obstacles, FMG `reGraph` port (authoritative pack biomes / zone lookups / pack gameplay data), marker gameplay (dungeons/inns/encounters), GPU culling of props (extend `scene_culling.comp`), local mist | —           | —                                  |

### Phase 0 definition of done

- [x] `tempGrid`/`precGrid`/`coastGrid`/`biomeGrid` built + logged
      (dims, blur σ, ms — same log style as the height grid)
- [x] `biomeColor` + `climate` textures uploaded; `TerrainData` indices set
- [x] Snow line, beach band, wet-sand band, highland rock, biome tint all
      visible in `./scripts/run.sh play screenshot`
- [x] Env overrides work (`ENGINE_AZGAAR_SNOW_LO/HI`, `ENGINE_AZGAAR_BEACH_H`)
- [x] Zero new warnings; terrain pass time within 0.5 ms of baseline
      (measured: +0.13 ms vs climate-disabled baseline, 300-frame rolling
      mean of the pass GPU profile)
- [x] `docs/azgaar-terrain.md` section table updated (§1b 47-section table + new §6 documenting the climate grids/textures/env vars)

> Implementation notes (deviations from the sketch): the climate texture's
> temperature/coast bytes use **biased unsigned encodes** (`temp+64`,
> `coast+11`) instead of raw Int8-in-a-byte — a raw two's-complement byte
> wraps 255↔1 across 0 and bilinear filtering rings into garbage; the
> biased encodes are monotonic and filter-safe. `biomeColor` uploads as
> RGBA8 (alpha=255) rather than R8G8B8 (R8G8B8_UNORM is not in Vulkan's
> mandatory-filterable set; RGBA8 is, and sRGB decoding of the authored
> biome hex colours comes free). Blend thresholds reach the shader via a
> `climateParams` vec4 in `TerrainData` (env vars are CPU-side; also
> `ENGINE_AZGAAR_CLIMATE_DISABLED=1` kill switch). Snow/sand albedo assets
> live in `pak_0_engine/images/terrain/{snow,sand}_default/` (snow
> generated procedurally — 1024² tileable FFT noise; sand copied from the
> pre-converted `wet-sand_Albedo.png` ktx2). `ENGINE_CAM_TELEPORT` now
> teleports the player (not just the camera) so the heightmap streaming
> window follows targeted screenshots.

### Phase 1 definition of done

- [ ] All 13 species meshes generated; prop pass registered + draws
- [ ] Biome-distinct scatter (screenshot comparison across ≥ 4 biomes)
- [ ] Placement gates verified: no vegetation in water/on roads/in
      settlements/on steep slopes; riparian band present
- [ ] Wind sway visible, per-instance phase de-synced
- [ ] Per-tile instance counts + buffer sizes logged; ≤ 150k/tile
- [ ] Streaming: fly across 3+ tile borders, no pops of _old_ props, no
      seam cracks
- [ ] `ENGINE_AZGAAR_PROPS_DEBUG=1` logs per-tile per-species counts +
      a 100 m ring histogram of instance density around the player
- [ ] **Swap path verified (D11):** drop a test `models/props/conifer.glb`
      (a Blender primitive export is fine) → renders in place of the
      placeholder with correct scale/origin/sway; delete it → placeholder
      returns; debug log shows `glb` vs `placeholder` source per species

### Phase 2 definition of done

- [ ] 187 rivers (Chilerel) as animated ribbons; width taper at sources
- [ ] No Z-fighting at mouths; wet strip under every river
- [ ] River hash built; riparian boost measurable in props debug output
- [ ] Load-time parse + build logged (< 50 ms)

### Phase 3 definition of done

- [ ] 822 settlements parsed; clusters generated; counts logged per group
- [ ] Capital screenshot: walls + citadel + port piers; village: huts
- [ ] Plateau: seam check passes, no visible lip at the falloff edge
- [ ] Zone GUI shows the settlement name inside the footprint
- [ ] Streaming across a settlement tile stays crack-free

---

## Validation

Per `AGENTS.md`:

```bash
./scripts/build.sh                                    # compiles C + shaders
./scripts/run.sh play screenshot /tmp/azgaar_<phase>.png
./scripts/run.sh play log 5000 && cat build/c-game/data/game.log
```

Useful existing levers: `ENGINE_CAM_TELEPORT=x,y,z,afterMs` (fly to a
biome/settlement/river for targeted screenshots), Ctrl+W wireframe,
Ctrl+H height ramp. Crashes: `ENGINE_DEBUG=1` + `gdb`.

Debug env vars to add (existing naming pattern):

| var                               | effect                                                                                                                 |
| --------------------------------- | ---------------------------------------------------------------------------------------------------------------------- |
| `ENGINE_AZGAAR_PROPS_DEBUG=1`     | per-tile per-species scatter counts + ring histogram; per-species mesh source (glb/placeholder), tri count, unitHeight |
| `ENGINE_AZGAAR_PROPS_WIREFRAME=1` | render prop meshes wireframe (origin/scale check for dropped .glb models)                                              |
| `ENGINE_AZGAAR_PROPS_DISABLED=1`  | kill switch (draws nothing)                                                                                            |
| `ENGINE_AZGAAR_SNOW_LO` / `_HI`   | snow line thresholds (°C)                                                                                              |
| `ENGINE_AZGAAR_BEACH_H`           | beach band height (m)                                                                                                  |
| `ENGINE_AZGAAR_RIVERS_DISABLED=1` | kill switch                                                                                                            |
| `ENGINE_AZGAAR_SETTLE_DISABLED=1` | kill switch                                                                                                            |
| `ENGINE_AZGAAR_WEATHER=0..4`      | force none/snow/rain/dust/leaves (see `plans/azgaar-weather-gpu-particles.md` for `..._COUNT` / `..._DEBUG`)           |

---

## Risks / open questions

1. **Scatter cost on the props pool.** 25 tiles × (~8 m lattice) =
   ~26M candidates per window if done naively. Mitigations: per-biome
   early-out (skip candidates in the wrong biome before any RNG work —
   the biome grid lookup is a 4-byte read), lattice at ~12 m with the
   fBm gate doing the fine clumping, and a per-tile wall-clock budget
   (e.g. 120 ms; overflow → sparser lattice next build, logged).
   Measure in Phase 1 with `genMs`-style logging before optimising.
2. **Low-poly trees at 80 km scale.** Forests viewed from a hill 2 km
   away may read as sparse dots. Mitigations in plan (density falloff +
   `_far` variants); open: whether a coarse "forest impostor" layer
   (one billboard per ~64 m cell in dense forest) is wanted in P5.
3. **Coarse climate grid (100 m texel).** The snow line is a km-scale
   feature — fine. But the beach band is driven by `inWorldPos.y`
   (4 m grid), not the climate grid — correct by construction.
4. **River mouths vs sea surface.** +3–5 cm ribbon offset could show a
   hairline at extreme grazing angles; the sea's shore foam band (0.3–1 m,
   noise-widened) should cover it — verify in the mouth screenshot.
5. **Settlement plateau vs road corridor.** If a road crosses a town,
   the corridor's grade-limited surface must be blended _after_ the
   plateau (corridor wins). Both are pure functions of world position, so
   order is the only invariant — assert it in code with a comment.
6. **SVG parse robustness.** Current FMG emits `M`+`C` chains; the
   tokenizer still implements `L`/relative forms, and a missing
   `riverN` path falls back to the JSON `cells[]` snapped through the
   grid (pack-cell id ≠ grid-cell id — a _last-resort_ approximation,
   logged loudly).
7. **Instance memory.** 150k × 40 B = 6 MB per tile buffer, 25 tiles
   worst case 150 MB GPU. Typical (ocean/ice tiles build nothing) ≪ 50 MB.
   Acceptable; revisit if mobile-class GPUs are ever a target.
8. **No prop collision (D10).** Player walking through forests is
   visually fine at blade/tree scale but noticeable inside towns.
   Accept for Phases 0–3; schedule convex-hull pass in P5.
9. **`pack.cells.biome` (section 16) vs recomputed biome.** Our grid-cell
   biome (recomputed) is what scatter uses; FMG's authoritative biome
   includes river flux in moisture. Divergence is visible only in narrow
   riparian strips — the riparian boost in B compensates. Revisit after
   the P5 `reGraph` port.
10. **Biome table is per-map authored** (users can edit icons/density in
    FMG). The density table in B must therefore be a _mapping_ of
    `iconsDensity` + icon names, not hardcoded per-biome species —
    implemented as such; the table above is the Chilerel instance.
11. **Hand-drawn model scale/origin drift (D11).** The #1 visual bug once
    real models land: a mesh whose base isn't at y=0 or whose unit scale
    differs from metre reads as floating/sunken/huge. Mitigations: the
    D11 loader normalises (unitHeight from bounds), the debug env var logs
    per-species bounds, and a `ENGINE_AZGAAR_PROPS_WIREFRAME=1` override
    renders prop meshes wireframe so origin/scale errors are obvious in
    one screenshot.
12. **cgltf loader edge cases.** Multi-primitive meshes, non-indexed
    geometry, embedded vs external images, DRACO/KTX2 compression from
    casual exports. Mitigation: the loader is permissive (join
    primitives, accept indexed or de-indexed, ignore anything but
    POSITION/NORMAL/TEXCOORD*0, never touch textures in v1) and \_any*
    failure falls back to the placeholder with a warn — a bad .glb can
    never take the world down.
