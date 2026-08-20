#pragma once

// Caps for the FMG biome table's icon arrays (section 3).  FMG's authored icon
// lists are short (<= ~10 entries); 16 is plenty.  Names are short species
// keys ("conifer", "grass", ...); 16 chars covers every FMG icon name.
#define AZGAAR_BIOME_MAX_ICONS 16
#define AZGAAR_ICON_NAME_LEN   16

namespace game {
struct AzgaarCell {
    float x;
    float y;
    float height;
    u32   biome;
    // Climate / coast scalars (sections 8/9/10/11).  `coast` is the signed
    // distance to the coast in cell units (+1 land coast, -1..-10 water,
    // 0 interior/deep water).  `feature` is the waterbody/landmass id
    // (section 9).  Filled by the climate parse; 0 when absent.
    float temp;     // degrees C
    float prec;     // 0..100
    float coast;    // cell units, see above
    u32   feature;  // waterbody / landmass id
    std::vector<u32>  vertices;
    u32               vertexCount;
    std::vector<u32>  neighbors;
    u32               neighborCount;
};

struct AzgaarVertex {
    float x;
    float y;
    u32*  cells;
    u32   cellCount;
};

struct AzgaarCellGrid {
    u32   cols;
    u32   rows;
    float bucketSize;
    float invBucketSize;
    std::vector<u32>  bucketStart;
    std::vector<u32>  bucketCells;
};

// A rendered pack cell used for political-zone lookups. Positions are in
// Azgaar map pixels (same space as AzgaarCell.x/.y). State/province ids use
// Azgaar's 1-based indexing; 0 means neutral / unclaimed.
struct AzgaarPackCell {
    float x;
    float y;
    u32   state;
    u32   province;
};

struct AzgaarNamedRegion {
    char  name[32];
    float color[3]; // authored region colour ([0,1] RGB) from the section's "color" hex
};

// FMG biome definition (section 3 of the .map).  `color` is the authored
// biome tint in [0,1] RGB, converted from the FMG hex string.  Biome ids index
// this array; 0 is Marine (water).  Used by the Azgaar terrain pass to add
// per-biome colour variation over the default grass texture.
//
// `habitability` / `iconsDensity` / `icons` (workstream B, plans/azgaar-world-
// population.md) drive the props scatter: `icons` lists the species icon names
// to scatter in this biome, repeated by weight (FMG's icon picker weighs each
// entry by its repetition, e.g. ["acacia" x8, "palm"] → acacia 8×, palm 1×);
// `iconsDensity` is the biome's base icon density (0..~250, scaled by /120).
struct AzgaarBiome {
    u32   id;
    char  name[32];
    float color[3];
    u32   habitability;
    u32   iconsDensity;
    u32   iconCount;
    char  icons[AZGAAR_BIOME_MAX_ICONS][AZGAAR_ICON_NAME_LEN];
};

// Result of a zone query at a map position. Name pointers reference the
// retained AzgaarWorld and stay valid until azgaarWorldDestroy(). They are
// "" (empty) when the cell has no such region (e.g. neutral wildlands).
struct AzgaarZoneInfo {
    u32         provinceId;
    u32         stateId;
    const char* provinceName;
    const char* stateName;
};

enum AzgaarRouteGroup {
    AZGAAR_ROUTE_ROAD,
    AZGAAR_ROUTE_TRAIL,
    AZGAAR_ROUTE_SEAROUTE,
};

struct AzgaarRoutePoint {
    float x;
    float y;
    u32   cell;
};

struct AzgaarRoute {
    AzgaarRouteGroup group;
    char             name[64];
    std::vector<AzgaarRoutePoint> points;
    u32                           pointCount;
    float            length;
    u32              feature;
};

// Azgaar river (workstream C, plans/azgaar-world-population.md).  Metadata
// comes from section 32 JSON; the centerline polyline comes from the SVG
// paths in section 5, joined by `i == N`.  Widths (`width`, `sourceWidth`)
// are in map pixels; convert to metres with world->metersPerPixel.  `type`
// is "River" or "Fork".  When the section 32 entry for a path id is missing
// the width defaults to 4 m.
struct AzgaarRiver {
    u32   id;
    char  name[48];
    u32   source;
    u32   mouth;
    float discharge;
    float lengthKm;
    float widthPx;
    float sourceWidthPx;
    float widthFactor;
    u32   parent;
    u32   basin;
    // Centerline polyline in map px (x,y pairs) from the SVG `d` attribute.
    std::vector<float> pointsPx;
    u32     pointCount;
};

// Named settlement (section 15, workstream D of plans/azgaar-world-population.md).
// Positions are converted to world space at parse time (azgaarMapToWorld of x,y).
// `flatY` is the natural terrain height at the centre (sampled before any
// plateau is applied) and drives the D8 terrain flattening.
enum AzgaarBurgGroup {
    AZGAAR_BURG_CAPITAL,
    AZGAAR_BURG_CITY,
    AZGAAR_BURG_TOWN,
    AZGAAR_BURG_VILLAGE,
    AZGAAR_BURG_HAMLET,
    AZGAAR_BURG_FORT,
    AZGAAR_BURG_MONASTERY,
    AZGAAR_BURG_CARAVANSERAI,
    AZGAAR_BURG_TRADING_POST,
    AZGAAR_BURG_GROUP_COUNT,
};

#define AZGAAR_SETT_FLAG_WALLS    (1u << 0)
#define AZGAAR_SETT_FLAG_CITADEL  (1u << 1)
#define AZGAAR_SETT_FLAG_PORT     (1u << 2)
#define AZGAAR_SETT_FLAG_PLAZA    (1u << 3)
#define AZGAAR_SETT_FLAG_TEMPLE   (1u << 4)
#define AZGAAR_SETT_FLAG_SHANTY   (1u << 5)

struct AzgaarSettlement {
    u32   id;            // 1-based burg id
    char  name[48];
    float wx, wz;        // world space (azgaarMapToWorld of x,y)
    float flatY;         // natural height at the centre (for the D8 plateau)
    float radiusM;       // footprint radius in metres
    float populationK;   // population in thousands
    u32   group;        // AzgaarBurgGroup
    u32   flags;        // AZGAAR_SETT_FLAG_* union
    u32   stateId;
    u32   cultureId;
    float stateColor[3]; // from section 14 (state trim/band colour)
};

// FMG marker types we render 3D landmarks for (section 35, workstream E of
// plans/azgaar-world-population.md).  The .map stores plural `type` strings
// ("volcanoes", "ruins", ...); anything not modelled yet (encounters,
// dungeons, inns, ...) maps to OTHER and is skipped as Phase-5 gameplay data.
enum AzgaarMarkerKind {
    AZGAAR_MARKER_OTHER = 0,
    AZGAAR_MARKER_VOLCANO,
    AZGAAR_MARKER_LIGHTHOUSE,
    AZGAAR_MARKER_HOT_SPRING,
    AZGAAR_MARKER_WATER_SOURCE,
    AZGAAR_MARKER_RUINS,
    AZGAAR_MARKER_MINE,
    AZGAAR_MARKER_BRIDGE,
    AZGAAR_MARKER_SACRED_FOREST,
};

struct AzgaarMarker {
    AzgaarMarkerKind kind;
    u32   id;       // 1-based marker index in section 35
    float x, y;     // map px (FMG marker position)
    float wx, wz;   // world space (azgaarMapToWorld of x,y)
    u32   cell;     // pack cell (informational)
    float size;     // FMG size scalar ~0.2..1.0 (defaults to 1)
};

struct AzgaarWorld {
    utils::String jsonData;
    std::vector<AzgaarCell>   cells;
    std::vector<AzgaarVertex> vertices;
    AzgaarCellGrid            cellGrid;
    std::vector<float>        heightGrid;
    u32    heightGridWidth;
    u32    heightGridHeight;
    // Smoothed biome-colour grid: RGB u8 triplets at the same dimensions as
    // heightGrid.  Baked from each texel's nearest-cell biome colour, then
    // low-passed (masked land/water) so biome transitions blend over roughly
    // one FMG cell spacing instead of switching at the pixel-hard Voronoi
    // border.  Sampled bilinearly by azgaarWorldSampleBiomeColorSmooth.
    std::vector<u8> biomeColorGrid;
    u32 biomeColorGridWidth;
    u32 biomeColorGridHeight;
    // Climate grids (workstream A of plans/azgaar-world-population.md).
    // Rasterized from the per-cell temp/prec/coast scalars at the same
    // dimensions as heightGrid, then Gaussian-blurred like it (biomeGrid is
    // a nearest-cell id field and is NOT blurred).  Sampled by
    // azgaarWorldSampleClimate and baked into the terrain pass' `climate`
    // texture.  `climateGridWidth == heightGridWidth` when hasClimate.
    std::vector<float> tempGrid;    // degrees C
    std::vector<float> precGrid;    // 0..100
    std::vector<float> coastGrid;   // cell units, + land / - water (see AzgaarCell.coast)
    std::vector<u8>    biomeGrid;   // biome id per texel
    u32    climateGridWidth;
    u32    climateGridHeight;
    // Peak land elevation in metres (azgaarHeightToMeters of the tallest
    // texel); drives the terrain pass' altitude rock band and the terrain
    // bounds' maxY.
    float maxLandHeightM;
    // Wind directions (degrees) from the settings JSON `winds[6]`
    // (section 1, field 19).  0 = not authored.
    float winds[6];
    char   mapName[128];
    double widthPx;
    double heightPx;
    double distanceScale;
    char   distanceUnit[16];
    double metersPerPixel;
    char   heightUnit[16];
    double heightExponent;
    u32    cellCount;
    u32    gridVertexCount;
    u32    vertexCount;
    std::vector<AzgaarSettlement> settlements; // section 15 (workstream D)
    u32    settlementCount;
    u32    routeCount;
    std::vector<AzgaarRoute> routes;
    std::vector<AzgaarRiver> rivers;
    u32    riverCount;
    std::vector<AzgaarMarker> markers; // section 35 (workstream E)
    u32    markerCount;

    // Political zone data parsed from pack.cells / pack.states / pack.provinces.
    std::vector<AzgaarPackCell>    packCells;
    u32                packCellCount;
    std::vector<AzgaarNamedRegion> states;
    u32                stateCount;
    std::vector<AzgaarNamedRegion> provinces;
    u32                provinceCount;
    // FMG biome table (section 3) + per-grid-cell biome id.  The biome id is
    // recomputed from each grid cell's height/temperature/moisture using FMG's
    // biome matrix (biome is only stored per *pack* cell in the .map, which we
    // do not reconstruct, but the grid inputs yield the same classification).
    std::vector<AzgaarBiome> biomes;
    u32          biomeCount;
};

// Look up a biome's authored colour.  Returns a neutral grey for unknown ids
// (e.g. when the biome table failed to parse).  Safe to call before the world
// is fully loaded (returns grey).
void azgaarWorldBiomeColor(const AzgaarWorld* world, u32 biomeId, float outColor[3]);

void azgaarMapToWorld(const AzgaarWorld* world, float mapX, float mapY, float* outWx, float* outWz);

// FMG .map height convention: per-cell height is a Uint8 in [0,100].
// Heights 0..19 are water (rendered at sea level = 0 m); 20 is the coastline
// baseline.  These are properties of the FMG data format, not of any one map,
// so they live here as the single source of truth shared by the height->metres
// mapping and the debug colour ramp.
#define AZGAAR_SEA_LEVEL_HEIGHT   20.0f
#define AZGAAR_HEIGHT_OFFSET      18.0f   // FMG formula base: (h - 18)^exp
#define AZGAAR_MAX_HEIGHT        100.0f

// Maximum seabed depth in metres.  Water cells (h < SEA_LEVEL) map to a
// negative world-Y seabed that shelves from 0 m at the coast (h=20) down to
// -AZGAAR_OCEAN_DEPTH_METERS offshore (h=0).
#define AZGAAR_OCEAN_DEPTH_METERS 60.0f

// Land-side coastline blend: cells with h in [20, 25] ramp smoothly from
// sea level (0 m) to full land height, eliminating instant cliffs where
// land cells first emerge from the water.
#define AZGAAR_WATER_BLEND_HIGH 25.0f

// FMG .map coast-distance conventions (section 10, `grid.cells.t`):
// +1 = land coast cell, -1..-10 = water cells by distance from the shore,
// 0 = interior land OR deep water (disambiguated by the cell height).
// These fill values complete the rasterized coast field into a coarse
// signed distance: at least one cell inland, and beyond the shallow band
// offshore.  Properties of the FMG data format, like the constants above.
#define AZGAAR_COAST_INTERIOR_LAND 2.0f
#define AZGAAR_COAST_DEEP_WATER   -11.0f

// Biome id returned by azgaarWorldSampleClimate when nothing is known
// (no cells / no climate grids).  Distinct from every real FMG biome id
// (0..12).
#define AZGAAR_BIOME_NONE 13u

// World-space Y of the sea level.  With the default height offset/scale
// this is 0 m.
float azgaarSeaLevelMeters(const AzgaarWorld* world);

float azgaarHeightToMeters(const AzgaarWorld* world, float h);

float azgaarWorldSampleHeightNearest(const AzgaarWorld* world, float xPx, float yPx);
float azgaarWorldSampleHeightCell(const AzgaarWorld* world, float xPx, float yPx, u32* outCellIndex);
float azgaarWorldSampleHeightSmooth(const AzgaarWorld* world, float xPx, float yPx);

// Smoothed biome colour (RGB in [0,1]) at a map-space position: bilinear over
// the pre-blurred biomeColourGrid, so neighbouring biomes fade into each
// other instead of meeting at a hard Voronoi edge.  Falls back to the flat
// nearest-cell biome colour (azgaarWorldBiomeColor) when the grid is
// unavailable, and to neutral grey when there are no cells at all.
void azgaarWorldSampleBiomeColorSmooth(const AzgaarWorld* world,
                                       float xPx,
                                       float yPx,
                                       float outColor[3]);

// Climate sample at a map-space position (world-population plan, workstream
// A).  Fields come from the pre-blurred climate grids via bilinear
// interpolation (nearest for the biome id).  When the grids are unavailable
// the per-cell scalars are used as a flat fallback; with no cells at all the
// sample is neutral (biome 13 = none).
struct AzgaarClimateSample {
    float temperature;   // degrees C
    float precipitation; // 0..100
    float coastCells;    // + land side / - water side, cell units (0 = n/a)
    u32   biome;         // biome id (13 = none/water fallback)
};

void azgaarWorldSampleClimate(const AzgaarWorld* world,
                              float xPx,
                              float yPx,
                              AzgaarClimateSample* out);

// Pack the climate grids into an RGBA8 UNORM texture for the terrain pass:
//   R = temperature + 64 (deg C, filter-safe biased encode)
//   G = precipitation (0..255 clamped)
//   B = coast distance + 11 (cell units)
//   A = biome id
// The bias keeps every channel monotonic across its sign change so bilinear
// filtering cannot ring (a raw two's-complement Int8 byte would wrap 255<->1
// at 0).  Returns an empty vector when the climate grids are unavailable.
std::vector<u8> azgaarWorldPackClimateTexture(const AzgaarWorld* world, u32* outWidth, u32* outHeight);

// Pack biomeColorGrid (RGB u8) into an RGBA8 buffer (A = 255) for GPU upload.
// Empty vector when the grid is absent.
std::vector<u8> azgaarWorldPackBiomeColorTexture(const AzgaarWorld* world, u32* outWidth, u32* outHeight);

// Samples the political zone (province + state) at a map-space position by
// finding the nearest pack cell. out->provinceName / out->stateName point into
// the retained world and are valid until azgaarWorldDestroy().
// outPackCellIndex (may be nullptr) receives the index of the nearest pack cell.
void azgaarWorldSampleZone(const AzgaarWorld* world,
                           float xPx,
                           float yPx,
                           AzgaarZoneInfo* out,
                           u32* outPackCellIndex);

bool azgaarWorldLoad(AzgaarWorld* world, const char* path);
void azgaarWorldDestroy(AzgaarWorld* world);
}  // namespace game
