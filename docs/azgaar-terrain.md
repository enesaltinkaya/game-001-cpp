# Azgaar `.map` Terrain Representation

How terrain is stored inside an Azgaar Fantasy Map Generator `.map` file,
and what that implies for reconstructing a 3D surface in this project.

Source of truth: `save.ts` / `load.ts` in the generator
(`/home/enes/Apps/Fantasy-Map-Generator/src/services/io/`), plus
`graphUtils.ts` (grid + Voronoi) and `resample.ts` (`reGraph`, height
smoothing). All numbers below are read from
`c-game/data/pak_1/azgaar/oghuz/Thoucy 2026-08-09-23-02.map`.

## TL;DR

Terrain lives in section **7** (`grid.cells.h`): a CSV of `Uint8` heights in
range **0–100**, one value per **jittered Voronoi cell** (NOT per pixel).
`0–19` = water, `20–100` = land. Real-world meters:

```
h >= 20 :  meters = (h - 18) ^ heightExponent
0 < h < 20 : depth  = ((h - 20) / h) * 50          // negative
```

The heightmap is a **piecewise-constant label field**, not a continuous
elevation field. Any smooth 3D mesh must be reconstructed by interpolation.

## 1. File-level format: CRLF-delimited sections

A `.map` is plain UTF-8 text, lines separated by `\r\n`. Each line is a
"section" indexed by position. Terrain is in sections **6** (grid geometry)
and **7** (heights). Full layout (from `save.ts`):

| #     | Field                                                                         | Type        |
| ----- | ----------------------------------------------------------------------------- | ----------- |
| 0     | params: `version\|license\|date\|seed\|graphWidth\|graphHeight\|mapId`        | `\|`-split  |
| 1     | settings (distanceUnit, distanceScale, **heightUnit**, **heightExponent**, …) | `\|`-split  |
| 2     | map coordinates (lat/lon)                                                     | JSON        |
| 3     | biomes                                                                        | JSON        |
| 5     | serialized SVG (the 2D map image)                                             | huge string |
| 6     | **grid general** (spacing, cellsX, cellsY, boundary, points, …)               | JSON        |
| **7** | **`grid.cells.h` — the heightmap**                                            | CSV `Uint8` |
| 8     | `grid.cells.prec` (precipitation)                                             | CSV         |
| 9     | `grid.cells.f` (feature id)                                                   | CSV         |
| 10    | `grid.cells.t` (terrain type)                                                 | CSV         |
| 11    | `grid.cells.temp` (temperature)                                               | CSV         |
| 12+   | pack features, cultures, states, burgs, per-pack-cell data, rivers, routes, … | JSON/CSV    |

> Heights are stored only on the **grid mesh** (section 7). The lower-res
> `pack` mesh does **not** store heights in the file — FMG recomputes
> `pack.cells.h` at load via `reGraph()`.

## 1b. Full section layout (verified against `save.ts`, 47 sections)

`mapData` in `services/io/save.ts` is 47 entries joined by CRLF. The SVG
(section 5) spans many *lines* in the file because its internal newlines
are bare `\n` (only section boundaries are `\r\n`), so section indexing
must split on CRLF, never on line number.

| # | Field | # | Field |
|---|-------|---|-------|
| 0 | params | 24 | `pack.cells.s` |
| 1 | settings (incl. JSON: `winds[6]`, …) | 25 | `pack.cells.state` |
| 2 | coordinates | 26 | `pack.cells.religion` |
| 3 | biomes (name, color, **habitability, iconsDensity, icons[], cost**) | 27 | `pack.cells.province` |
| 4 | notes/regiments | 28 | (deprecated crossroad) |
| 5 | serialized SVG | 29 | religions |
| 6 | grid general (spacing, points, **features** = ocean/island/lake ids) | 30 | provinces |
| 7 | `grid.cells.h` (heights) | 31 | name bases |
| 8 | `grid.cells.prec` (precipitation) | 32 | **rivers** (source/mouth, discharge, width, `cells[]` = pack ids) |
| 9 | `grid.cells.f` (waterbody id per cell) | 33 | (empty) |
| 10 | `grid.cells.t` (**signed distance to coast**, cell units: +1 land coast, −1…−10 water, 0 interior/deep — see `features.ts`) | 34 | fonts |
| 11 | `grid.cells.temp` (temperature) | 35 | **markers** (x, y, cell, size, type: volcanoes, ruins, lighthouses, …) |
| 12 | packFeatures (landmass objects) | 36 | cellRoutes |
| 13 | cultures | 37 | routes (roads/trails/searoutes) |
| 14 | states | 38 | zones |
| 15 | **burgs** (x, y, name, population, group, walls/citadel/port/…, state, culture) | 39 | **ice** (iceberg polygons + cellId) |
| 16 | `pack.cells.biome` | 40 | `pack.cells.good` |
| 17 | `pack.cells.burg` | 41 | goods (e.g. `distribution:"biome(5, 6, 7, 8, 9)"`) |
| 18 | `pack.cells.conf` | 42 | markets |
| 19 | `pack.cells.culture` | 43 | deals |
| 20 | `pack.cells.fl` | 44 | `pack.cells.market` |
| 21 | population per pack cell | 45 | custom good icons |
| 22 | `pack.cells.r` | 46 | measurers |
| 23 | (deprecated road) | | |

Example header from `Thoucy 2026-08-09-23-02.map`:

```
version=1.139.11   seed=7660245   graphWidth=1920  graphHeight=993
distanceUnit=km   distanceScale=0.1   heightUnit=m   heightExponent=1.5
```

## 2. The geometry: a _jittered square grid_ of Voronoi cells

From section 6 of the example map:

```
spacing: 5.22    cellsX: 368    cellsY: 190    →  69,920 grid points
```

The heightmap is **not a pixel grid**. It is:

- Points placed on a regular lattice with `spacing` px between them.
- Then **jittered** by up to ±0.45·spacing (see `getJitteredGrid` in
  `graphUtils.ts`).
- Each point owns a **Voronoi cell** (the polygon of all pixels closer to
  it than to any other point).
- **One height value is stored per point/cell.**

So terrain is a **piecewise-constant field**: constant height inside each
Voronoi polygon, with a step at every cell boundary. FMG only ever renders
this as 2D SVG — **there is no canonical continuous 3D surface stored**.
Any 3D mesh must be reconstructed by interpolation.

Two meshes exist in FMG:

- **grid** — high-resolution heightmap (section 6 + 7). Use this for terrain.
- **pack** — low-resolution mesh derived via `reGraph()`; used for gameplay
  data (biomes, states, burgs, rivers, routes).

## 3. The height values (section 7)

CSV of `Uint8` values, one per grid point (69,920 for the example map),
range **0–100**.

- **`h = 0..19` → water** (below sea level)
- **`h = 20..100` → land** (20 = coastline / sea level)

Example distribution from `Thoucy 2026-08-09-23-02.map`:

```
water (h < 20):  44,382 cells  (63.5 %)
land  (h >= 20): 25,538 cells  (36.5 %)
peak value:      h = 71
```

The histogram is a smooth decay from h=0 out to h=71 with **no natural
plateau band** — neighboring cells very commonly differ by 1–3 units. This
matters for the blockiness problem (§5).

## 4. Height → real-world meters

From `unitUtils.ts → getHeight()`:

```js
if (h >= 20)
  height = (h - 18) ** heightExponent; // meters
else if (0 < h < 20) height = ((h - 20) / h) * 50; // negative, underwater
```

`heightUnit` multiplies the result: `m` → ×1, `ft` → ×3.281, `f` → ×0.5468.

With `heightExponent = 1.5` (the example map), land heights map to:

| h          | (h−18)^1.5 → meters |
| ---------- | ------------------- |
| 20 (coast) | 2^1.5 = **2.8 m**   |
| 25         | 7^1.5 = **18.5 m**  |
| 30         | 12^1.5 = **41.6 m** |
| 40         | 22^1.5 = **103 m**  |
| 50         | 32^1.5 = **181 m**  |
| 71 (peak)  | 53^1.5 = **386 m**  |

## 5. Key source files

In the generator (`/home/enes/Apps/Fantasy-Map-Generator/src/`):

- `services/io/save.ts`, `services/io/load.ts` — `.map` section layout
- `utils/graphUtils.ts` — jittered grid, `calculateVoronoi`, height helpers
- `generators/resample.ts` — `reGraph`, `smoothHeightmap`
- `utils/unitUtils.ts` — `getHeight()` (h → meters formula)
- `generators/voronoi.ts` — `Cells`, `Vertices` types

## 6. Climate fields (terrain material blending)

`AzgaarWorld.c` keeps the per-cell climate scalars (sections 8/9/10/11:
precipitation, waterbody id, signed coast distance, temperature) on each
`AzgaarCell`, plus the `winds[6]` array from the settings JSON (section 1,
field 19).  At load they are rasterized alongside the height grid into
`heightGrid`-sized fields (`tempGrid`/`precGrid`/`coastGrid`, Gaussian-blurred
like the height grid; `biomeGrid` nearest) and uploaded once per world as two
static RGBA8 textures sampled by `heightmap_terrain.frag`:

- **biomeColor** — the blurred biome-colour grid (authored FMG hex colours,
  sRGB view); soft-multiplied over the grass base for the biome tint.
- **climate** — UNORM, filter-safe biased byte encodings so bilinear cannot
  ring across a sign change:
  `R = temp °C + 64`, `G = precipitation`, `B = coast cells + 11`,
  `A = biome id`.

The shader blends, in order: grass base → biome tint → beach/sand band (+
darker wet-sand strip at the waterline, driven by the fragment's own height)
→ rock (slope cliff blend + highland band at 55–85% of max land height) →
snow (isotherm band from the temperature field, Glacier biome forced snow,
value-noise line breakup).  CPU sampling is available via
`azgaarWorldSampleClimate` (bilinear over the same grids).

Thresholds are env-overridable at load (`LoadingAzgaar.c`):
`ENGINE_AZGAAR_SNOW_LO` / `ENGINE_AZGAAR_SNOW_HI` (°C, defaults −1/+3),
`ENGINE_AZGAAR_BEACH_H` (m, default 2.5),
`ENGINE_AZGAAR_CLIMATE_DISABLED=1` (kill switch),
`ENGINE_AZGAAR_CLIMATE_SIGMA` (climate grid blur σ override, texels).
