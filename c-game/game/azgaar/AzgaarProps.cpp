#include "azgaar/AzgaarProps.h"
#include "azgaar/AzgaarWorld.h"
#include "azgaar/AzgaarLandmarks.h"
#include "azgaar/AzgaarWeather.h"
#include "ecs/system/heightmap/HeightmapTerrain.h"
#include "ecs/system/heightmap/HeightmapSource.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/transform/TransformComponent.h"
#include "ecs/system/scene/SceneParser.h"
#include "ecs/system/scene/SceneSystem.h"
#include "ecs/system/scene/Scene.h"
#include "ecs/system/mesh/MeshComponent.h"
#include "ecs/Ecs.h"
#include "renderer/vulkan/pass/azgaar_props/VulkanAzgaarPropsPass.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/scene/VulkanScene.h"
#include "renderer/material/MaterialManager.h"
#include "renderer/material/Material.h"
#include "thread/Thread.h"
#include "logger/Logger.h"
#include "timer/Timer.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// ── Recursive .dat model search (across all paks) ─────────────────────────
// The deciduous model is authored in Blender, packed by 2-blender-props.sh
// into a .dat (gltfpack + zstd) and a simplified _far.dat, and dropped into a
// configurable directory (models/ or models/props/).  Because the target dir
// is now a parameter, the azgaar system recursively searches every pak for
// the files by basename so they are found regardless of which subdirectory
// they live in.

static char* azgaarFindDatPath(const char* baseName) {
    Array(String) dats = dataManagerListFiles(".dat");
    char* found        = NULL;
    for (u32 i = 0; i < arraySize(dats); i++) {
        String* p         = &dats[i];
        const char* slash = strrchr(p->data, '/');
        const char* base  = slash ? slash + 1 : p->data;
        if (strcmp(base, baseName) == 0) {
            found  = static_cast<char*>(memoryAlloc(p->size + 1));
            strcpy(found, p->data);
            break;
        }
    }
    for (u32 i = 0; i < arraySize(dats); i++) {
        stringDestroy(&dats[i]);
    }
    arrayFree(dats);
    return found;
}

static Scene* g_deciduousScene    = NULL;
static char* g_deciduousPath      = NULL;  // kept alive for the async scene load
static Scene* g_deciduousFarScene = NULL;
static char* g_deciduousFarPath   = NULL;
static u32 g_variantCount[AZGAAR_PROP_COUNT];  // per-species variant count

// Per-(species, variant) cull spheres in unit-height space, parallel to the
// variant table rows (same order: row = base[species] + variant).  Built in
// buildAllMeshes from each variant's local AABB (circumsphere); the cull
// stage scales them by the instance's `scale`.
static float* g_variantSphereC = NULL;  // [3] per row
static float* g_variantSphereR = NULL;  // [1] per row
static u32 g_variantSphereRows = 0;

// Forward declaration (defined further down, in the mesh-builder section).
static void buildAllMeshes(PropVariantRange** outVariants,
                           u32* outVariantCount,
                           u32* outVertCount,
                           u32* outIdxCount,
                           void** outVerts,
                           void** outIdx);

// Bumped every time the variant table is rebuilt; each tile remembers the
// version it was scattered with, so a rebuild forces a re-scatter with the
// updated per-species variant counts (fixes tiles scattered before the
// deciduous scene was ready staying on variant 0).
static u32 g_propsVersion = 0;

// Forward declaration (defined further down, in the tile-state section).
static void propsInvalidateTiles(void);

// Rebuilds the merged mesh + variant table from the current scene state and
// re-pushes it to the engine pass.  Runs on the main thread: at init (with
// whatever scenes are ready) and once per authored-scene ready callback.
static void propsRebuildAndPushMeshes(void) {
    u32 vCount = 0, iCount = 0, variantCount = 0;
    void* verts                = NULL;
    void* idx                  = NULL;
    PropVariantRange* variants = NULL;
    buildAllMeshes(&variants, &variantCount, &vCount, &iCount, &verts, &idx);
    vulkanAzgaarPropsSetMeshes(verts, vCount, idx, iCount);
    vulkanAzgaarPropsSetVariants(variants, variantCount);
    memoryFree(verts);
    memoryFree(idx);
    memoryFree(variants);

    // The variant table changed: drop all scattered tiles so the update loop
    // re-scatters them with the updated counts.
    propsInvalidateTiles();
    g_propsVersion++;
}

// Runs on the main thread once the near deciduous scene is fully parsed.
static void onDeciduousSceneReady(Scene* scene, void* userData) {
    (void)userData;
    g_deciduousScene = scene;  // keep the global in sync with the ready scene
    propsRebuildAndPushMeshes();
    info("azgaarProps: deciduous scene ready — variant table rebuilt: %u near variants",
         g_variantCount[AZGAAR_PROP_DECIDUOUS]);
}

// Runs on the main thread once the far-LOD scene is fully parsed.
static void onDeciduousFarSceneReady(Scene* scene, void* userData) {
    (void)userData;
    g_deciduousFarScene = scene;
    propsRebuildAndPushMeshes();
    info("azgaarProps: deciduous_far scene ready — variant table rebuilt: %u far variants",
         g_variantCount[AZGAAR_PROP_DECIDUOUS_FAR]);
}

static void azgaarLoadDeciduousModel(void) {
    char* path = azgaarFindDatPath("deciduous.dat");
    if (!path) {
        info(
            "azgaarProps: deciduous.dat not found in any pak; deciduous species falls back to the "
            "procedural mesh");
    } else {
        info("azgaarProps: found deciduous model at %s", path);
        // sceneLoad is async: the worker thread reads req->path, so the buffer
        // must outlive the load. Keep it in g_deciduousPath and free on destroy.
        g_deciduousPath  = path;
        g_deciduousScene = sceneLoadCb(path, onDeciduousSceneReady, NULL);
    }
    char* farPath = azgaarFindDatPath("deciduous_far.dat");
    if (!farPath) {
        info("azgaarProps: deciduous_far.dat not found; the far LOD reuses the near geometry");
    } else {
        info("azgaarProps: found deciduous far LOD at %s", farPath);
        g_deciduousFarPath  = farPath;
        g_deciduousFarScene = sceneLoadCb(farPath, onDeciduousFarSceneReady, NULL);
    }
}

// ── Species metadata (const table; Phase 1 = vegetation 0..12) ───────────
// Placeholders are authored at UNIT height (base y=0, top y=1) with
// proportional widths, so the scatter's uniform `scale` == target height in
// metres (unitHeight = 1.0).  `slopeMax` rejects a species on steeper ground;
// `swayFactor` drives the wind animation (0 = rock / static).
typedef struct PropSpeciesDef {
    const char* key;
    float baseMin;   // target height metres (min)
    float baseMax;   // target height metres (max)
    float slopeMax;  // reject above this slope (dy/dx)
    float sway;      // 0..1 wind sway
    u32 farVariant;  // species id used at far range (self if none)
    u32 flags;       // bit 0 = alpha test
} PropSpeciesDef;

static const PropSpeciesDef kSpecies[AZGAAR_PROP_COUNT] = {
    [AZGAAR_PROP_GRASS_TUFT] = {"grass_tuft", 0.3f, 1.0f, 0.40f, 1.0f, AZGAAR_PROP_GRASS_TUFT, 0},
    [AZGAAR_PROP_CONIFER]    = {"conifer", 4.0f, 9.0f, 0.55f, 0.5f, AZGAAR_PROP_CONIFER_FAR, 0},
    [AZGAAR_PROP_CONIFER_FAR] =
        {"conifer_far", 4.0f, 9.0f, 0.55f, 0.5f, AZGAAR_PROP_CONIFER_FAR, 0},
    [AZGAAR_PROP_DECIDUOUS] = {"deciduous", 5.0f, 12.0f, 0.45f, 0.4f, AZGAAR_PROP_DECIDUOUS_FAR, 0},
    [AZGAAR_PROP_DECIDUOUS_FAR] =
        {"deciduous_far", 5.0f, 12.0f, 0.45f, 0.4f, AZGAAR_PROP_DECIDUOUS_FAR, 0},
    [AZGAAR_PROP_ACACIA]    = {"acacia", 6.0f, 10.0f, 0.35f, 0.35f, AZGAAR_PROP_ACACIA, 0},
    [AZGAAR_PROP_PALM]      = {"palm", 5.0f, 8.0f, 0.20f, 0.4f, AZGAAR_PROP_PALM, 0},
    [AZGAAR_PROP_CACTUS]    = {"cactus", 1.0f, 2.5f, 0.35f, 0.15f, AZGAAR_PROP_CACTUS, 0},
    [AZGAAR_PROP_DEAD_TREE] = {"dead_tree", 3.0f, 7.0f, 0.5f, 0.5f, AZGAAR_PROP_DEAD_TREE, 0},
    [AZGAAR_PROP_REED]      = {"reed", 0.8f, 1.5f, 0.15f, 1.0f, AZGAAR_PROP_REED, 0},
    [AZGAAR_PROP_SHRUB]     = {"shrub", 0.4f, 1.0f, 0.6f, 0.2f, AZGAAR_PROP_SHRUB, 0},
    [AZGAAR_PROP_ROCK]      = {"rock", 0.5f, 3.0f, 1.0f, 0.0f, AZGAAR_PROP_ROCK, 0},
    [AZGAAR_PROP_FLOWER]    = {"flower", 0.2f, 0.5f, 0.4f, 0.8f, AZGAAR_PROP_FLOWER, 1},
    // Buildings (Phase 3, workstream D): placed by AzgaarSettlements on the
    // D8 plateau, never by the biome scatter.  High slopeMax (they sit on the
    // flattened centre), zero sway (static structures).
    [AZGAAR_PROP_HUT]    = {"hut", 4.0f, 6.0f, 1.0f, 0.0f, AZGAAR_PROP_HUT, 0},
    [AZGAAR_PROP_HOUSE]  = {"house", 5.0f, 8.0f, 1.0f, 0.0f, AZGAAR_PROP_HOUSE, 0},
    [AZGAAR_PROP_TOWER]  = {"tower", 8.0f, 15.0f, 1.0f, 0.0f, AZGAAR_PROP_TOWER, 0},
    [AZGAAR_PROP_WALL]   = {"wall", 4.0f, 6.0f, 1.0f, 0.0f, AZGAAR_PROP_WALL, 0},
    [AZGAAR_PROP_TEMPLE] = {"temple", 6.0f, 10.0f, 1.0f, 0.0f, AZGAAR_PROP_TEMPLE, 0},
    [AZGAAR_PROP_DOCK]   = {"dock", 2.0f, 4.0f, 1.0f, 0.0f, AZGAAR_PROP_DOCK, 0},
    [AZGAAR_PROP_GATE]   = {"gate", 8.0f, 12.0f, 1.0f, 0.0f, AZGAAR_PROP_GATE, 0},
    // Landmarks (Phase 4, workstream E): placed by AzgaarLandmarks from
    // section-35 markers, never by the biome scatter.  Static (zero sway);
    // slopeMax 1.0 since they sit on authored marker positions.  The bridge's
    // "height" scale is its SPAN in metres (deck authored unit-length along Z).
    [AZGAAR_PROP_VOLCANO]    = {"volcano", 420.0f, 850.0f, 1.0f, 0.0f, AZGAAR_PROP_VOLCANO, 0},
    [AZGAAR_PROP_LIGHTHOUSE] = {"lighthouse", 22.0f, 30.0f, 1.0f, 0.0f, AZGAAR_PROP_LIGHTHOUSE, 0},
    [AZGAAR_PROP_LIGHTHOUSE_CAP] =
        {"lighthouse_cap", 5.0f, 7.0f, 1.0f, 0.0f, AZGAAR_PROP_LIGHTHOUSE_CAP, 0},
    [AZGAAR_PROP_RUIN_COLUMN] = {"ruin_column", 2.5f, 4.5f, 1.0f, 0.0f, AZGAAR_PROP_RUIN_COLUMN, 0},
    [AZGAAR_PROP_RUIN_ARCH]   = {"ruin_arch", 3.0f, 5.0f, 1.0f, 0.0f, AZGAAR_PROP_RUIN_ARCH, 0},
    [AZGAAR_PROP_MINE_FRAME]  = {"mine_frame", 4.0f, 6.0f, 1.0f, 0.0f, AZGAAR_PROP_MINE_FRAME, 0},
    [AZGAAR_PROP_BRIDGE]      = {"bridge", 8.0f, 40.0f, 1.0f, 0.0f, AZGAAR_PROP_BRIDGE, 0},
};

// GPU LOD cross-fade role for a species: 0 = near, 1 = far, 2 = no LOD.  Only
// the conifer / deciduous pairs cross-fade; everything else is always visible.
static u32 propsLodRole(u32 species) {
    if (species == AZGAAR_PROP_CONIFER || species == AZGAAR_PROP_DECIDUOUS) return 0;
    if (species == AZGAAR_PROP_CONIFER_FAR || species == AZGAAR_PROP_DECIDUOUS_FAR) return 1;
    return 2;
}

// ── Mesh builder (procedural placeholders, all in PropsVertex layout) ──────

typedef struct MeshBuilder {
    PropsVertex* verts;
    u32 vertCap, vertCount;
    u32* idx;
    u32 idxCap, idxCount;
} MeshBuilder;

static void mbInit(MeshBuilder* mb, u32 vertCap, u32 idxCap) {
    mb->verts      = static_cast<PropsVertex*>(memoryAlloc(sizeof(PropsVertex) * vertCap));
    mb->vertCap   = vertCap;
    mb->vertCount = 0;
    mb->idx        = static_cast<u32*>(memoryAlloc(sizeof(u32) * idxCap));
    mb->idxCap    = idxCap;
    mb->idxCount  = 0;
}

static void mbFree(MeshBuilder* mb) {
    memoryFree(mb->verts);
    memoryFree(mb->idx);
    mb->verts = NULL;
    mb->idx   = NULL;
}

static u32 mbAddVert(MeshBuilder* mb,
                     float x,
                     float y,
                     float z,
                     float nx,
                     float ny,
                     float nz,
                     float u,
                     float v) {
    u32 i = mb->vertCount;
    if (i >= mb->vertCap) return (u32)-1;
    PropsVertex* p = &mb->verts[i];
    p->position[0] = x;
    p->position[1] = y;
    p->position[2] = z;
    p->normal[0]   = nx;
    p->normal[1]   = ny;
    p->normal[2]   = nz;
    p->tangent[0]  = 0;
    p->tangent[1]  = 0;
    p->tangent[2]  = 0;
    p->tangent[3]  = 0;
    p->uv[0]       = u;
    p->uv[1]       = v;
    p->joints      = 0;
    p->weights     = 0;
    p->texId       = NO_PROPS_TEX;  // procedural species: no texture
    p->color[0]    = 1.0f;          // white = tintable (receives the per-instance tint)
    p->color[1]    = 1.0f;
    p->color[2]    = 1.0f;
    mb->vertCount++;
    return i;
}

// Override a vertex' part colour (e.g. mark trunk verts brown so they are
// NOT tinted by the per-instance biome colour).  `color` is a 3-float array.
static void mbVertColor(MeshBuilder* mb, u32 idx, const float color[3]) {
    if (idx == (u32)-1 || idx >= mb->vertCount) return;
    PropsVertex* p = &mb->verts[idx];
    p->color[0]    = color[0];
    p->color[1]    = color[1];
    p->color[2]    = color[2];
}

static void mbTri(MeshBuilder* mb, u32 a, u32 b, u32 c) {
    if (a == (u32)-1 || b == (u32)-1 || c == (u32)-1) return;
    if (mb->idxCount + 3 > mb->idxCap) return;
    mb->idx[mb->idxCount++] = a;
    mb->idx[mb->idxCount++] = b;
    mb->idx[mb->idxCount++] = c;
}

static void mbQuad(MeshBuilder* mb, u32 a, u32 b, u32 c, u32 d) {
    mbTri(mb, a, b, c);
    mbTri(mb, a, c, d);
}

// An N-sided frustum/cone ring from baseY to topY (open, side quads only).
// Returns the first base-vertex index.
static u32 mbCone(MeshBuilder* mb,
                  float cx,
                  float cz,
                  float baseY,
                  float topY,
                  float baseR,
                  float topR,
                  u32 sides,
                  float nx,
                  float nz) {
    if (sides < 3) sides = 3;
    float span = topY - baseY;
    if (span <= 0.0f) span = 0.001f;
    float slopeN = 1.0f / sqrtf((topR - baseR) * (topR - baseR) + span * span);
    float ny     = (topR - baseR) * slopeN;  // outward-ish; good enough for lighting
    ny           = -ny;                      // point outward/up
    u32 ring0    = mb->vertCount;
    for (u32 s = 0; s < sides; s++) {
        float a0 = (float)s / (float)sides * 2.0f * M_PI;
        float a1 = (float)(s + 1) / (float)sides * 2.0f * M_PI;
        u32 b0 =
            mbAddVert(mb, cx + cosf(a0) * baseR, baseY, cz + sinf(a0) * baseR, nx, ny, nz, 0, 0);
        u32 b1 =
            mbAddVert(mb, cx + cosf(a1) * baseR, baseY, cz + sinf(a1) * baseR, nx, ny, nz, 0, 0);
        u32 t0 = mbAddVert(mb, cx + cosf(a0) * topR, topY, cz + sinf(a0) * topR, nx, ny, nz, 0, 1);
        u32 t1 = mbAddVert(mb, cx + cosf(a1) * topR, topY, cz + sinf(a1) * topR, nx, ny, nz, 0, 1);
        mbQuad(mb, b0, b1, t1, t0);
    }
    // Flat top cap (so cones read solid from below).
    if (topR > 0.001f) {
        u32 c = mbAddVert(mb, cx, topY, cz, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f);
        for (u32 s = 0; s < sides; s++) {
            u32 t0 = ring0 + s;
            u32 t1 = ring0 + ((s + 1) % sides);
            mbTri(mb, c, t1, t0);
        }
    }
    return ring0;
}

// A solid cylinder (side + base + top).  Convenience wrapper over mbCone.
static void
mbCylinder(MeshBuilder* mb, float cx, float cz, float baseY, float topY, float r, u32 sides) {
    mbCone(mb, cx, cz, baseY, topY, r, r, sides, 0.0f, 0.0f);
    u32 c0 = mbAddVert(mb, cx, baseY, cz, 0.0f, -1.0f, 0.0f, 0.5f, 0.0f);
    for (u32 s = 0; s < sides; s++) {
        float a0 = (float)s / (float)sides * 2.0f * M_PI;
        float a1 = (float)(s + 1) / (float)sides * 2.0f * M_PI;
        u32 b0   = mbAddVert(mb, cx + cosf(a0) * r, baseY, cz + sinf(a0) * r, 0, -1, 0, 0, 0);
        u32 b1   = mbAddVert(mb, cx + cosf(a1) * r, baseY, cz + sinf(a1) * r, 0, -1, 0, 0, 0);
        mbTri(mb, c0, b1, b0);
    }
}

// A small displaced octahedron blob (6 verts, 8 tris) — rocks / shrubs.
// `flat` squashes it in Y. `jit` displaces each vertex radially (deterministic
// via a salt hash) so rocks read as boulders, not crystals.
static void mbBlob(MeshBuilder* mb, float cx, float cy, float cz, float r, float flat, u32 salt) {
    float ry = r * flat;
    u32 v[6];
    v[0] = mbAddVert(mb, cx, cy + ry, cz, 0, 1, 0, 0.5f, 1.0f);
    v[1] = mbAddVert(mb, cx, cy - ry, cz, 0, -1, 0, 0.5f, 0.0f);
    for (u32 k = 0; k < 4; k++) {
        float a  = (float)k / 4.0f * 2.0f * M_PI;
        float j  = 1.0f + 0.25f * ((float)((salt + k * 7) & 3) - 1.5f);  // deterministic 0.75..1.25
        v[2 + k] = mbAddVert(mb,
                             cx + cosf(a) * r * j,
                             cy,
                             cz + sinf(a) * r * j,
                             cosf(a),
                             0.0f,
                             sinf(a),
                             (float)k,
                             0.5f);
    }
    mbTri(mb, v[0], v[2], v[3]);
    mbTri(mb, v[0], v[3], v[4]);
    mbTri(mb, v[0], v[4], v[5]);
    mbTri(mb, v[0], v[5], v[2]);
    mbTri(mb, v[1], v[3], v[2]);
    mbTri(mb, v[1], v[4], v[3]);
    mbTri(mb, v[1], v[5], v[4]);
    mbTri(mb, v[1], v[2], v[5]);
}

// A UV sphere (seg x ring) centred at (cx, cy, cz), radius r, squashed by
// `flat` in Y.  Used for the deciduous canopy so it reads as a rounded crown
// rather than a flat diamond.  Verts = (seg+1)*(ring+1); tris = seg*ring*2.
static void
mbSphere(MeshBuilder* mb, float cx, float cy, float cz, float r, float flat, u32 seg, u32 ring) {
    if (seg < 3) seg = 3;
    if (ring < 2) ring = 2;
    float ry  = r * flat;
    u32 first = mb->vertCount;
    for (u32 j = 0; j <= ring; j++) {
        float v   = (float)j / (float)ring;
        float phi = v * M_PI;
        float y   = cy + cosf(phi) * ry;
        float rad = r * sinf(phi);
        for (u32 i = 0; i <= seg; i++) {
            float u     = (float)i / (float)seg;
            float theta = u * 2.0f * M_PI;
            float x     = cx + cosf(theta) * rad;
            float z     = cz + sinf(theta) * rad;
            float nx    = cosf(theta) * sinf(phi);
            float nz    = sinf(theta) * sinf(phi);
            float ny    = cosf(phi) * flat;
            mbAddVert(mb, x, y, z, nx, ny, nz, u, v);
        }
    }
    for (u32 j = 0; j < ring; j++) {
        for (u32 i = 0; i < seg; i++) {
            u32 a = first + j * (seg + 1) + i;
            u32 b = a + 1;
            u32 c = a + (seg + 1);
            u32 d = c + 1;
            mbQuad(mb, a, b, d, c);
        }
    }
}

// `count` crossed blades from the base (grass / reed).  Each blade is a short
// strip of quads that curves outward as it rises and tapers to a point, so a
// tuft reads as grass instead of flat cards.  Per-blade height and curl vary
// deterministically (golden-ratio hash on the blade index, stable across
// tiles / rebuilds) so the silhouette is uneven like real turf.
// Verts per blade = 2*SEGS + 1 (pointed tip); tris = 2*SEGS - 1.
static void mbBlades(MeshBuilder* mb, u32 count, float height, float halfW, float spread) {
    const u32 SEGS = 3;
    for (u32 b = 0; b < count; b++) {
        float f    = (float)b;
        float a    = f / (float)count * 2.0f * M_PI + 0.3f;
        float dirX = cosf(a), dirZ = sinf(a);
        float h1 = f * 0.6180339887f + 0.13f;
        h1 -= floorf(h1);
        float h2 = f * 0.379f + 0.71f;
        h2 -= floorf(h2);
        float H    = height * (0.80f + 0.35f * h1);  // uneven blade heights
        float bend = 0.30f + 0.40f * h2;             // outward curl (rad)
        float R    = H / bend;                       // arc radius
        float bx = dirX * spread, bz = dirZ * spread;
        u32 prev0 = (u32)-1, prev1 = (u32)-1;
        for (u32 i = 0; i <= SEGS; i++) {
            float t  = (float)i / (float)SEGS;
            float th = bend * t;
            float ct = cosf(th), st = sinf(th);
            float r  = R * (1.0f - ct);
            float y  = R * st;
            float cx = bx + dirX * r, cz = bz + dirZ * r;
            // Top-face normal: convex side of the outward curl (straight up at
            // the base, tilted up-inward at the tip).
            float nx = -dirX * ct, ny = st, nz = -dirZ * ct;
            if (i == SEGS) {
                u32 tip = mbAddVert(mb, cx, y, cz, nx, ny, nz, 0.5f, 1.0f);
                mbTri(mb, prev0, prev1, tip);
                break;
            }
            float w = halfW * powf(1.0f - t, 1.5f);  // taper to a point
            // Pinch the very base (where the blade meets the ground) so the
            // tuft skirt reads thin; full width is reached by t = 0.25.
            w *= 0.3f + 0.7f * fminf(t / 0.25f, 1.0f);
            float px = -dirZ * w, pz = dirX * w;  // width axis
            u32 v0 = mbAddVert(mb, cx - px, y, cz - pz, nx, ny, nz, 0.0f, t);
            u32 v1 = mbAddVert(mb, cx + px, y, cz + pz, nx, ny, nz, 1.0f, t);
            if (prev0 != (u32)-1) mbQuad(mb, prev0, prev1, v1, v0);
            prev0 = v0;
            prev1 = v1;
        }
    }
}

// A single small quad (flowers): unit UV so the fragment shader's radial alpha
// test reads it as a flower dot.  A thin stem rises to the quad.
static void mbFlower(MeshBuilder* mb) {
    // stem
    mbCylinder(mb, 0.0f, 0.0f, 0.0f, 0.9f, 0.02f, 4);
    // flower head: a diamond quad at y=1.0 with unit UV.
    float h = 0.9f;
    float s = 0.28f;
    u32 n   = mbAddVert(mb, 0.0f, h + s, 0.0f, 0, 1, 0, 0.5f, 1.0f);
    u32 e   = mbAddVert(mb, s, h, 0.0f, 0, 1, 0, 1.0f, 0.5f);
    u32 w   = mbAddVert(mb, -s, h, 0.0f, 0, 1, 0, 0.0f, 0.5f);
    u32 f   = mbAddVert(mb, 0.0f, h - s * 0.5f, 0.0f, 0, 1, 0, 0.5f, 0.0f);
    mbQuad(mb, n, e, f, w);
}

// A box from baseY..topY centred at (cx, cz) with half-widths hx/hz:
// 8 verts, 12 tris, outward normals per face.
static void
mbBox(MeshBuilder* mb, float cx, float cz, float baseY, float topY, float hx, float hz) {
    float x0 = cx - hx, x1 = cx + hx;
    float z0 = cz - hz, z1 = cz + hz;
    u32 v[8];
    v[0] = mbAddVert(mb, x0, baseY, z1, 0.0f, -1.0f, 0.0f, 0, 0);
    v[1] = mbAddVert(mb, x1, baseY, z1, 0.0f, -1.0f, 0.0f, 1, 0);
    v[2] = mbAddVert(mb, x1, baseY, z0, 0.0f, -1.0f, 0.0f, 1, 1);
    v[3] = mbAddVert(mb, x0, baseY, z0, 0.0f, -1.0f, 0.0f, 0, 1);
    v[4] = mbAddVert(mb, x0, topY, z1, 0.0f, 1.0f, 0.0f, 0, 0);
    v[5] = mbAddVert(mb, x1, topY, z1, 0.0f, 1.0f, 0.0f, 1, 0);
    v[6] = mbAddVert(mb, x1, topY, z0, 0.0f, 1.0f, 0.0f, 1, 1);
    v[7] = mbAddVert(mb, x0, topY, z0, 0.0f, 1.0f, 0.0f, 0, 1);
    mbTri(mb, v[0], v[1], v[2]);
    mbTri(mb, v[0], v[2], v[3]);  // bottom
    mbTri(mb, v[4], v[7], v[6]);
    mbTri(mb, v[4], v[6], v[5]);  // top
    mbTri(mb, v[0], v[3], v[7]);
    mbTri(mb, v[0], v[7], v[4]);  // left (-x)
    mbTri(mb, v[1], v[5], v[6]);
    mbTri(mb, v[1], v[6], v[2]);  // right (+x)
    mbTri(mb, v[3], v[2], v[6]);
    mbTri(mb, v[3], v[6], v[7]);  // front (z0)
    mbTri(mb, v[0], v[4], v[5]);
    mbTri(mb, v[0], v[5], v[1]);  // back (z1)
}

// Gable roof from a base ring (eaves at baseY) to a single ridge point at
// (cx, topY, cz): 2 slope tris + 2 gable-end tris (4 tris total).
static void
mbGableRoof(MeshBuilder* mb, float cx, float cz, float baseY, float topY, float hx, float hz) {
    u32 rl = mbAddVert(mb, cx - hx, baseY, cz + hz, 0.0f, 0.6f, 0.4f, 0, 0);
    u32 rr = mbAddVert(mb, cx + hx, baseY, cz + hz, 0.0f, 0.6f, 0.4f, 1, 0);
    u32 fl = mbAddVert(mb, cx - hx, baseY, cz - hz, 0.0f, 0.6f, -0.4f, 0, 1);
    u32 fr = mbAddVert(mb, cx + hx, baseY, cz - hz, 0.0f, 0.6f, -0.4f, 1, 1);
    u32 rg = mbAddVert(mb, cx, topY, cz, 0.0f, 1.0f, 0.0f, 0.5, 1);
    mbTri(mb, rl, rr, rg);  // back slope
    mbTri(mb, fl, fr, rg);  // front slope
    mbTri(mb, fl, rl, rg);  // left gable
    mbTri(mb, fr, rr, rg);  // right gable
}

// Per-species geometry builders (unit height, base at y=0).
// Baked brown trunk colour: NOT tinted by the per-instance biome tint, so
// trunks stay brown like in Blender instead of going green in-game.
static const float kTrunkColor[3] = {0.36f, 0.25f, 0.16f};

// Colour the contiguous vertex block added by the most recent geometry call
// (a single mbCylinder / mbCone), starting at `start`.
static void mbColorSince(MeshBuilder* mb, u32 start, const float color[3]) {
    for (u32 i = start; i < mb->vertCount; i++) mbVertColor(mb, i, color);
}

static void buildGrassTuft(MeshBuilder* mb) {
    mbBlades(mb, 10, 1.0f, 0.10f, 0.35f);
}

static void buildConifer(MeshBuilder* mb) {
    u32 trunkStart = mb->vertCount;
    mbCylinder(mb, 0, 0, 0.0f, 0.25f, 0.06f, 6);  // trunk
    mbColorSince(mb, trunkStart, kTrunkColor);
    mbCone(mb, 0, 0, 0.15f, 0.55f, 0.38f, 0.0f, 7, 0, 0);  // lower cone
    mbCone(mb, 0, 0, 0.45f, 0.82f, 0.27f, 0.0f, 7, 0, 0);
    mbCone(mb, 0, 0, 0.72f, 1.0f, 0.16f, 0.0f, 7, 0, 0);
}

static void buildConiferFar(MeshBuilder* mb) {
    u32 trunkStart = mb->vertCount;
    mbCylinder(mb, 0, 0, 0.0f, 0.3f, 0.06f, 5);
    mbColorSince(mb, trunkStart, kTrunkColor);
    mbCone(mb, 0, 0, 0.2f, 1.0f, 0.34f, 0.0f, 6, 0, 0);
}

static void buildDeciduous(MeshBuilder* mb) {
    u32 trunkStart = mb->vertCount;
    mbCylinder(mb, 0, 0, 0.0f, 0.5f, 0.05f, 6);
    mbColorSince(mb, trunkStart, kTrunkColor);
    // Rounded crown (UV sphere) instead of the flat diamond blob.
    mbSphere(mb, 0.0f, 0.78f, 0.0f, 0.45f, 0.9f, 8, 6);
}

static void buildDeciduousFar(MeshBuilder* mb) {
    u32 trunkStart = mb->vertCount;
    mbCylinder(mb, 0, 0, 0.0f, 0.45f, 0.05f, 5);
    mbColorSince(mb, trunkStart, kTrunkColor);
    mbSphere(mb, 0.0f, 0.72f, 0.0f, 0.45f, 0.85f, 6, 4);
}

static void buildAcacia(MeshBuilder* mb) {
    // slightly bent trunk + a flat, wide canopy disc near the top.
    u32 trunkStart = mb->vertCount;
    mbCone(mb, 0.05f, 0.0f, 0.0f, 0.7f, 0.07f, 0.05f, 6, 0, 0);
    mbColorSince(mb, trunkStart, kTrunkColor);
    mbCone(mb, 0.1f, 0.0f, 0.68f, 0.82f, 0.55f, 0.5f, 10, 0, 0);  // disc canopy
}

static void buildPalm(MeshBuilder* mb) {
    u32 trunkStart = mb->vertCount;
    mbCone(mb, 0.0f, 0.0f, 0.0f, 0.85f, 0.08f, 0.05f, 6, 0, 0);  // trunk
    mbColorSince(mb, trunkStart, kTrunkColor);
    // fronds: flat quads radiating from the crown.
    u32 crown = 8;
    for (u32 k = 0; k < crown; k++) {
        float a  = (float)k / (float)crown * 2.0f * M_PI;
        float fx = cosf(a) * 0.4f;
        float fz = sinf(a) * 0.4f;
        u32 c0   = mbAddVert(mb, 0, 0.85f, 0, 0, 1, 0, 0.5f, 0.5f);
        u32 c1   = mbAddVert(mb, fx * 0.5f, 0.82f, fz * 0.5f, 0, 0.5f, 0, 0.2f, 0.5f);
        u32 tip  = mbAddVert(mb, fx, 0.95f + 0.05f * sinf(a), fz, 0, 0.3f, 0, 1.0f, 0.5f);
        u32 c2   = mbAddVert(mb, fx * 0.5f, 0.78f, fz * 0.5f, 0, 0.5f, 0, 0.8f, 0.5f);
        mbQuad(mb, c0, c1, tip, c2);
    }
}

static void buildCactus(MeshBuilder* mb) {
    mbCylinder(mb, 0, 0, 0.0f, 1.0f, 0.14f, 6);
    mbCone(mb, 0.14f, 0.0f, 0.35f, 0.6f, 0.09f, 0.06f, 5, 0, 0);   // arm 1
    mbCone(mb, -0.14f, 0.0f, 0.5f, 0.72f, 0.09f, 0.06f, 5, 0, 0);  // arm 2
}

static void buildDeadTree(MeshBuilder* mb) {
    u32 start = mb->vertCount;
    mbCone(mb, 0, 0, 0.0f, 0.9f, 0.09f, 0.04f, 5, 0, 0);             // trunk
    mbCone(mb, 0.12f, 0.0f, 0.5f, 0.85f, 0.05f, 0.01f, 4, 0.3f, 0);  // branch
    mbCone(mb, -0.1f, 0.1f, 0.6f, 0.95f, 0.04f, 0.01f, 4, -0.3f, 0.1f);
    mbColorSince(mb, start, kTrunkColor);  // whole dead tree is woody brown
}

static void buildReed(MeshBuilder* mb) {
    mbBlades(mb, 3, 1.0f, 0.06f, 0.2f);
}

static void buildShrub(MeshBuilder* mb) {
    mbBlob(mb, 0.0f, 0.45f, 0.0f, 0.6f, 0.6f, 51);
}

static void buildRock(MeshBuilder* mb) {
    mbBlob(mb, 0.0f, 0.4f, 0.0f, 0.9f, 0.5f, 97);
    mbBlob(mb, 0.5f, 0.25f, 0.3f, 0.5f, 0.5f, 131);
}

static void buildFlower(MeshBuilder* mb) {
    mbFlower(mb);
}

// ── Building placeholders (Phase 3, workstream D) ─────────────────────────
// All at unit height (base y=0, top y=1) so the per-instance `scale` is the
// building's real height in metres.  ~40-120 tris each (plan D11 placeholders).

static void buildHut(MeshBuilder* mb) {
    mbBox(mb, 0, 0, 0.0f, 0.55f, 0.34f, 0.34f);        // walls
    mbGableRoof(mb, 0, 0, 0.55f, 1.0f, 0.42f, 0.42f);  // eaves overhang
}

static void buildHouse(MeshBuilder* mb) {
    mbBox(mb, 0, 0, 0.0f, 0.65f, 0.45f, 0.38f);
    mbGableRoof(mb, 0, 0, 0.65f, 1.0f, 0.55f, 0.48f);
}

static void buildTower(MeshBuilder* mb) {
    mbCylinder(mb, 0, 0, 0.0f, 0.8f, 0.2f, 8);
    mbCone(mb, 0, 0, 0.8f, 1.0f, 0.26f, 0.02f, 8, 0.0f, 0.0f);
}

static void buildWall(MeshBuilder* mb) {
    mbBox(mb, 0, 0, 0.0f, 0.85f, 0.5f, 0.12f);
    // Crenellations (merlon nubs on the top edge).
    mbBox(mb, -0.35f, 0, 0.85f, 1.0f, 0.07f, 0.12f);
    mbBox(mb, -0.12f, 0, 0.85f, 1.0f, 0.07f, 0.12f);
    mbBox(mb, 0.12f, 0, 0.85f, 1.0f, 0.07f, 0.12f);
    mbBox(mb, 0.35f, 0, 0.85f, 1.0f, 0.07f, 0.12f);
}

static void buildTemple(MeshBuilder* mb) {
    mbBox(mb, 0, 0, 0.0f, 0.7f, 0.30f, 0.42f);  // nave
    for (u32 c = 0; c < 4; c++) {
        float cx = (c & 1 ? 1.0f : -1.0f) * 0.20f;
        float cz = (c < 2 ? -1.0f : 1.0f) * 0.35f;
        mbCylinder(mb, cx, cz, 0.0f, 0.7f, 0.06f, 4);
    }
    mbGableRoof(mb, 0, 0, 0.7f, 1.0f, 0.38f, 0.50f);
}

static void buildDock(MeshBuilder* mb) {
    mbCylinder(mb, -0.45f, 0, 0.0f, 0.8f, 0.07f, 4);
    mbCylinder(mb, 0.45f, 0, 0.0f, 0.8f, 0.07f, 4);
    mbBox(mb, 0, 0, 0.8f, 1.0f, 0.55f, 0.2f);  // deck
}

static void buildGate(MeshBuilder* mb) {
    mbCylinder(mb, -0.45f, 0, 0.0f, 0.85f, 0.14f, 6);
    mbCylinder(mb, 0.45f, 0, 0.0f, 0.85f, 0.14f, 6);
    mbCone(mb, -0.45f, 0, 0.85f, 1.0f, 0.18f, 0.02f, 6, 0, 0);
    mbCone(mb, 0.45f, 0, 0.85f, 1.0f, 0.18f, 0.02f, 6, 0, 0);
    mbBox(mb, 0, 0, 0.85f, 1.0f, 0.55f, 0.12f);  // lintel
}

// ── Landmark geometry (Phase 4, workstream E) ──────────────────────────────
// Unit-height placeholders like the buildings, EXCEPT the bridge whose unit
// extent is length along local +Z (instance scale == span in metres; the
// landmark module yaws +Z perpendicular to the river flow so it spans it).

// A horizontal vertex ring at height y, radius r; normals tilt by (nr, ny)
// rotated around Y.  Returns the first vertex index of the ring.
static u32 lmRing(MeshBuilder* mb, float y, float r, float nr, float ny, u32 sides) {
    u32 first = mb->vertCount;
    for (u32 s = 0; s < sides; s++) {
        float a  = (float)s / (float)sides * 2.0f * M_PI;
        float nx = cosf(a) * nr;
        float nz = sinf(a) * nr;
        mbAddVert(mb, cosf(a) * r, y, sinf(a) * r, nx, ny, nz, 0, y);
    }
    return first;
}

// Quad band between two rings (equal side counts).
static void lmBand(MeshBuilder* mb, u32 a, u32 b, u32 sides) {
    for (u32 s = 0; s < sides; s++) {
        u32 s1 = (s + 1u) % sides;
        mbQuad(mb, a + s, a + s1, b + s1, b + s);
    }
}

// Volcano: manual rings (mbCone's auto top cap would seal the crater).
// Outer slope ~45 deg, rim, inner crater wall, central vent cone.
static void buildVolcano(MeshBuilder* mb) {
    const u32 SIDES = 14;
    u32 base        = lmRing(mb, 0.0f, 1.0f, 0.76f, 0.65f, SIDES);
    u32 rim         = lmRing(mb, 1.0f, 0.24f, 0.76f, 0.65f, SIDES);
    u32 floorRing   = lmRing(mb, 0.93f, 0.17f, -0.7f, 0.7f, SIDES);    // crater wall
    u32 vent        = lmRing(mb, 0.975f, 0.05f, 0.35f, 0.94f, SIDES);  // vent cone
    u32 apex        = mbAddVert(mb, 0.0f, 0.995f, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f);
    lmBand(mb, base, rim, SIDES);
    lmBand(mb, rim, floorRing, SIDES);
    lmBand(mb, floorRing, vent, SIDES);
    for (u32 s = 0; s < SIDES; s++) {
        mbTri(mb, vent + s, vent + (s + 1u) % SIDES, apex);
    }
}

// Lighthouse tower: tapered shaft + plinth only.  The gallery / lantern /
// dome live in the separate cap species (azgaarLandmarks places a near-white
// cap instance on top — per-instance tint cannot two-tone one mesh).
static void buildLighthouse(MeshBuilder* mb) {
    mbBox(mb, 0, 0, 0.0f, 0.03f, 0.19f, 0.19f);  // base plinth
    mbCone(mb, 0, 0, 0.0f, 0.80f, 0.15f, 0.10f, 10, 0.0f, 0.0f);
}

// The lighthouse cap stands on the tower top: its unit height maps to the
// top 22% of the full lighthouse (gallery deck + lantern room + dome).
static void buildLighthouseCap(MeshBuilder* mb) {
    mbCylinder(mb, 0, 0, 0.0f, 0.364f, 0.59f, 10);                  // gallery deck
    mbCylinder(mb, 0, 0, 0.364f, 0.727f, 0.386f, 8);                // lantern room
    mbCone(mb, 0, 0, 0.727f, 1.0f, 0.409f, 0.055f, 8, 0.0f, 0.0f);  // dome
}

// Broken column: plinth + fluted-less shaft + crumbled capital.
static void buildRuinColumn(MeshBuilder* mb) {
    mbBox(mb, 0, 0, 0.0f, 0.06f, 0.16f, 0.16f);     // plinth
    mbCylinder(mb, 0, 0, 0.05f, 0.82f, 0.085f, 8);  // shaft
    mbBlob(mb, 0.0f, 0.84f, 0.0f, 0.10f, 0.7f, 7);  // crumbled capital
}

// Ruin arch: two uprights + lintel, plus a fallen drum and rubble at the
// base so it reads as collapsed up close.
static void buildRuinArch(MeshBuilder* mb) {
    mbCylinder(mb, -0.26f, 0.0f, 0.0f, 0.74f, 0.062f, 7);  // left upright
    mbCylinder(mb, 0.26f, 0.0f, 0.0f, 0.74f, 0.062f, 7);   // right upright
    mbBox(mb, 0, 0, 0.74f, 0.84f, 0.36f, 0.10f);           // lintel
    mbBox(mb, 0.05f, 0.30f, 0.0f, 0.13f, 0.26f, 0.055f);   // fallen drum
    mbBlob(mb, 0.42f, 0.10f, 0.05f, 0.09f, 0.7f, 31);      // rubble
}

// Mine adit headframe: two timber posts, crossbeam, low brace, and the rock
// mass around the entrance.  Rock piles around it are ROCK species instances.
static void buildMineFrame(MeshBuilder* mb) {
    mbBox(mb, -0.20f, 0.0f, 0.0f, 0.74f, 0.05f, 0.05f);  // left post
    mbBox(mb, 0.20f, 0.0f, 0.0f, 0.74f, 0.05f, 0.05f);   // right post
    mbBox(mb, 0, 0, 0.72f, 0.80f, 0.26f, 0.05f);         // crossbeam
    mbBox(mb, 0, 0, 0.30f, 0.38f, 0.24f, 0.04f);         // low brace
    mbBlob(mb, 0.0f, 0.45f, 0.10f, 0.48f, 1.0f, 17);     // adit rock mass
}

// Plank bridge: arched deck along local +Z, unit length (scale == span m).
// Deck strip + underside + side skirts + rope railings + support posts.
#define BRIDGE_SEG 9

static void buildBridge(MeshBuilder* mb) {
    const float HW = 0.07f;   // deck half-width (fraction of span)
    const float TH = 0.012f;  // deck thickness
    const float RL = 0.045f;  // railing height
    float yv[BRIDGE_SEG + 1];
    float zv[BRIDGE_SEG + 1];
    float tv[BRIDGE_SEG + 1];
    for (u32 k = 0; k <= BRIDGE_SEG; k++) {
        tv[k]      = (float)k / (float)BRIDGE_SEG;
        zv[k]      = tv[k] - 0.5f;
        float arch = 1.0f - (2.0f * zv[k]) * (2.0f * zv[k]);
        yv[k]      = 0.03f + 0.06f * arch;
    }
    u32 top[BRIDGE_SEG + 1][2], bot[BRIDGE_SEG + 1][2];
    for (u32 k = 0; k <= BRIDGE_SEG; k++) {
        top[k][0] = mbAddVert(mb, -HW, yv[k], zv[k], 0, 1, 0, 0, tv[k]);
        top[k][1] = mbAddVert(mb, HW, yv[k], zv[k], 0, 1, 0, 1, tv[k]);
        bot[k][0] = mbAddVert(mb, -HW, yv[k] - TH, zv[k], 0, -1, 0, 0, tv[k]);
        bot[k][1] = mbAddVert(mb, HW, yv[k] - TH, zv[k], 0, -1, 0, 1, tv[k]);
    }
    // Deck top + underside.
    for (u32 k = 0; k < BRIDGE_SEG; k++) {
        mbQuad(mb, top[k][0], top[k][1], top[k + 1][1], top[k + 1][0]);
        mbQuad(mb, bot[k][0], bot[k + 1][0], bot[k + 1][1], bot[k][1]);
    }
    // Side skirts (dedicated side-normal verts).
    for (u32 side = 0; side < 2; side++) {
        float x  = side ? HW : -HW;
        float nx = side ? 1.0f : -1.0f;
        for (u32 k = 0; k <= BRIDGE_SEG; k++) {
            u32 st = mbAddVert(mb, x, yv[k], zv[k], nx, 0, 0, tv[k], 1);
            u32 sb = mbAddVert(mb, x, yv[k] - TH, zv[k], nx, 0, 0, tv[k], 0);
            if (k > 0) {
                mbQuad(mb, st - 2, st, sb, sb - 2);  // prev top, cur top, cur bot, prev bot
            }
        }
    }
    // Railings: vertical quads from the deck edge up to a rail top vertex.
    for (u32 side = 0; side < 2; side++) {
        float x  = (side ? HW : -HW) + (side ? 0.004f : -0.004f);
        float nx = side ? 1.0f : -1.0f;
        for (u32 k = 0; k <= BRIDGE_SEG; k++) {
            u32 rt = mbAddVert(mb, x, yv[k] + RL, zv[k], nx, 0, 0, tv[k], 1);
            u32 de = side ? top[k][1] : top[k][0];
            if (k > 0) {
                u32 rtPrev = rt - 1;
                u32 dePrev = side ? top[k - 1][1] : top[k - 1][0];
                mbQuad(mb, dePrev, de, rt, rtPrev);
            }
        }
    }
    // Support posts down from the deck at three stations.
    for (u32 g = 0; g < 3; g++) {
        float z = zv[(g * BRIDGE_SEG) / 2];
        float y = yv[(g * BRIDGE_SEG) / 2];
        for (u32 side = 0; side < 2; side++) {
            float x = side ? HW - 0.012f : -HW + 0.012f;
            mbBox(mb, x, z, y - 0.10f, y, 0.012f, 0.012f);
        }
    }
}

#undef BRIDGE_SEG

// ── Merged mesh buffer + variant metadata ─────────────────────────────────

// Pack one Mesh component (one authored object) into the merged buffer at the
// given offsets, and compute its local AABB.  Mirrors the PropsVertex packing
// in VulkanScene.c (positions straight; normal/tangent/uv/joints/weights
// unpacked from prim->attributes gated by prim->attributeMask).  Each vertex
// also carries the primitive's material base-color texture-array index (texId).
//
// The authored objects are at real-world scale (Blender metres), but the
// scatter applies a uniform `scale` == target height in metres (authored for
// unit-height placeholders).  So each object is normalised to unit height here
// (base y=0, top y=1, centred in x/z) — identical to the procedural
// placeholders — so the same `scale` logic applies and the trees read at the
// intended size instead of 5-12x oversized.
static void appendSceneMesh(Mesh* mesh,
                            PropsVertex* mergedVerts,
                            u32 vertOffset,
                            u32* mergedIdx,
                            u32 idxOffset,
                            float outBoundsMin[3],
                            float outBoundsMax[3],
                            u32* outVertCount,
                            u32* outIdxCount) {
    // First pass: original AABB (drives the unit-height normalisation).
    float omin[3] = {1e9f, 1e9f, 1e9f};
    float omax[3] = {-1e9f, -1e9f, -1e9f};
    for (u32 p = 0; p < arraySize(mesh->primitives); p++) {
        Primitive* prim = &mesh->primitives[p];
        for (u32 v = 0; v < prim->vertexCount; v++) {
            for (u32 c = 0; c < 3; c++) {
                float val = prim->positions[v * 3 + c];
                if (val < omin[c]) omin[c] = val;
                if (val > omax[c]) omax[c] = val;
            }
        }
    }
    info(
        "azgaarProps: deciduous obj AABB min=(%.2f,%.2f,%.2f) max=(%.2f,%.2f,%.2f) "
        "extents=(%.2f,%.2f,%.2f)",
        (double)omin[0],
        (double)omin[1],
        (double)omin[2],
        (double)omax[0],
        (double)omax[1],
        (double)omax[2],
        (double)(omax[0] - omin[0]),
        (double)(omax[1] - omin[1]),
        (double)(omax[2] - omin[2]));
    float height = omax[1] - omin[1];
    if (height <= 0.0f) height = 1.0f;
    float invH    = 1.0f / height;
    float centerX = (omin[0] + omax[0]) * 0.5f;
    float centerZ = (omin[2] + omax[2]) * 0.5f;

    u32 vOff      = vertOffset;
    u32 iOff      = idxOffset;
    float bmin[3] = {1e9f, 1e9f, 1e9f};
    float bmax[3] = {-1e9f, -1e9f, -1e9f};
    for (u32 p = 0; p < arraySize(mesh->primitives); p++) {
        Primitive* prim = &mesh->primitives[p];
        u32 primVertOff = vOff;
        // This primitive's material selects the base-color texture in the global
        // set 0 `textures` array.  Procedural species (no material) use
        // NO_PROPS_TEX so the shader keeps the plain per-instance tint.
        Material* mat = getMaterialById(prim->materialId);
        u32 texId     = (mat != NULL) ? mat->colorTexture : NO_PROPS_TEX;
        for (u32 v = 0; v < prim->vertexCount; v++) {
            PropsVertex sv = {};
            sv.texId       = texId;
            sv.position[0] = (prim->positions[v * 3 + 0] - centerX) * invH;
            sv.position[1] = (prim->positions[v * 3 + 1] - omin[1]) * invH;
            sv.position[2] = (prim->positions[v * 3 + 2] - centerZ) * invH;
            if (prim->attributeMask & (1 << cgltf_attribute_type_normal)) {
                const int16_t* n =
                    (const int16_t*)(prim->attributes[cgltf_attribute_type_normal] + v * 8);
                sv.normal[0] = (float)n[0] / 32767.0f;
                sv.normal[1] = (float)n[1] / 32767.0f;
                sv.normal[2] = (float)n[2] / 32767.0f;
            }
            if (prim->attributeMask & (1 << cgltf_attribute_type_tangent)) {
                const int8_t* t =
                    (const int8_t*)(prim->attributes[cgltf_attribute_type_tangent] + v * 4);
                sv.tangent[0] = (float)t[0] / 127.0f;
                sv.tangent[1] = (float)t[1] / 127.0f;
                sv.tangent[2] = (float)t[2] / 127.0f;
                sv.tangent[3] = (float)t[3] / 127.0f;
            }
            if (prim->attributeMask & (1 << cgltf_attribute_type_texcoord)) {
                const uint16_t* uv =
                    (const uint16_t*)(prim->attributes[cgltf_attribute_type_texcoord] + v * 4);
                sv.uv[0] = (float)uv[0] / 65535.0f;
                sv.uv[1] = (float)uv[1] / 65535.0f;
            }
            if (prim->attributeMask & (1 << cgltf_attribute_type_joints)) {
                memcpy(&sv.joints, prim->attributes[cgltf_attribute_type_joints] + v * 4, 4);
            }
            if (prim->attributeMask & (1 << cgltf_attribute_type_weights)) {
                memcpy(&sv.weights, prim->attributes[cgltf_attribute_type_weights] + v * 4, 4);
            }
            if (prim->attributeMask & (1 << cgltf_attribute_type_color)) {
                // Per-part colour (white = tintable, brown trunk = not tinted).
                u32 comps   = (u32)(arraySize(prim->colors) / prim->vertexCount);  // 3 or 4
                sv.color[0] = prim->colors[(size_t)v * comps + 0];
                sv.color[1] = prim->colors[(size_t)v * comps + 1];
                sv.color[2] = prim->colors[(size_t)v * comps + 2];
            }
            mergedVerts[vOff] = sv;
            for (u32 c = 0; c < 3; c++) {
                float val = sv.position[c];
                if (val < bmin[c]) bmin[c] = val;
                if (val > bmax[c]) bmax[c] = val;
            }
            vOff++;
        }
        for (u32 i = 0; i < prim->indexCount; i++) {
            mergedIdx[iOff + i] = prim->indices[i] + primVertOff;
        }
        iOff += prim->indexCount;
    }
    memcpy(outBoundsMin, bmin, sizeof(bmin));
    memcpy(outBoundsMax, bmax, sizeof(bmax));
    *outVertCount = vOff - vertOffset;
    *outIdxCount  = iOff - idxOffset;
}

// Appends every mesh of a loaded scene (one per variant) to the merged
// buffer, recording per-object index offsets / counts / local AABBs.
static void appendSceneMeshes(SparseSet* meshes,
                              PropsVertex* mergedVerts,
                              u32* vOff,
                              u32* mergedIdx,
                              u32* iOff,
                              u32* idxOffsets,
                              u32* vCounts,
                              u32* iCounts,
                              float* bMin,
                              float* bMax) {
    for (u32 i = 0; i < meshes->size; i++) {
        Mesh* mesh     = static_cast<Mesh*>(ssGetDataByIndex(meshes, i));
        idxOffsets[i] = *iOff;
        appendSceneMesh(mesh,
                        mergedVerts,
                        *vOff,
                        mergedIdx,
                        *iOff,
                        &bMin[i * 3],
                        &bMax[i * 3],
                        &vCounts[i],
                        &iCounts[i]);
        *vOff += vCounts[i];
        *iOff += iCounts[i];
    }
}

// Records the cull sphere (AABB circumsphere, unit space) for one variant row.
static void propsStoreVariantSphere(u32 row, const float bmin[3], const float bmax[3]) {
    for (u32 c = 0; c < 3; c++) {
        g_variantSphereC[row * 3 + c] = (bmin[c] + bmax[c]) * 0.5f;
    }
    float hx              = (bmax[0] - bmin[0]) * 0.5f;
    float hy              = (bmax[1] - bmin[1]) * 0.5f;
    float hz              = (bmax[2] - bmin[2]) * 0.5f;
    g_variantSphereR[row] = sqrtf(hx * hx + hy * hy + hz * hz);
}

static void buildAllMeshes(PropVariantRange** outVariants,
                           u32* outVariantCount,
                           u32* outVertCount,
                           u32* outIdxCount,
                           void** outVerts,
                           void** outIdx) {
    // Build each species into a temp builder, track the total, then concatenate.
    static const u32 kVertCap = 256;
    static const u32 kIdxCap  = 640;  // bridge deck + skirts + rails + posts
    u32 svCount[AZGAAR_PROP_COUNT];
    u32 siCount[AZGAAR_PROP_COUNT];

    MeshBuilder* builders[AZGAAR_PROP_COUNT];
    for (u32 s = 0; s < AZGAAR_PROP_COUNT; s++) {
        builders[s]  = static_cast<MeshBuilder*>(memoryAlloc(sizeof(MeshBuilder)));
        mbInit(builders[s], kVertCap, kIdxCap);
    }
    buildGrassTuft(builders[AZGAAR_PROP_GRASS_TUFT]);
    buildConifer(builders[AZGAAR_PROP_CONIFER]);
    buildConiferFar(builders[AZGAAR_PROP_CONIFER_FAR]);
    buildDeciduous(builders[AZGAAR_PROP_DECIDUOUS]);
    buildDeciduousFar(builders[AZGAAR_PROP_DECIDUOUS_FAR]);
    buildAcacia(builders[AZGAAR_PROP_ACACIA]);
    buildPalm(builders[AZGAAR_PROP_PALM]);
    buildCactus(builders[AZGAAR_PROP_CACTUS]);
    buildDeadTree(builders[AZGAAR_PROP_DEAD_TREE]);
    buildReed(builders[AZGAAR_PROP_REED]);
    buildShrub(builders[AZGAAR_PROP_SHRUB]);
    buildRock(builders[AZGAAR_PROP_ROCK]);
    buildFlower(builders[AZGAAR_PROP_FLOWER]);
    buildHut(builders[AZGAAR_PROP_HUT]);
    buildHouse(builders[AZGAAR_PROP_HOUSE]);
    buildTower(builders[AZGAAR_PROP_TOWER]);
    buildWall(builders[AZGAAR_PROP_WALL]);
    buildTemple(builders[AZGAAR_PROP_TEMPLE]);
    buildDock(builders[AZGAAR_PROP_DOCK]);
    buildGate(builders[AZGAAR_PROP_GATE]);
    buildVolcano(builders[AZGAAR_PROP_VOLCANO]);
    buildLighthouse(builders[AZGAAR_PROP_LIGHTHOUSE]);
    buildLighthouseCap(builders[AZGAAR_PROP_LIGHTHOUSE_CAP]);
    buildRuinColumn(builders[AZGAAR_PROP_RUIN_COLUMN]);
    buildRuinArch(builders[AZGAAR_PROP_RUIN_ARCH]);
    buildMineFrame(builders[AZGAAR_PROP_MINE_FRAME]);
    buildBridge(builders[AZGAAR_PROP_BRIDGE]);

    // Count the authored deciduous objects (one Mesh component per variant).
    // The scene load is async, so only extract once the scene is fully
    // parsed (ready); otherwise fall back to the procedural placeholder.
    u32 decVertTotal = 0, decIdxTotal = 0, decCount = 0;
    SparseSet* decMeshes = NULL;
    if (g_deciduousScene && g_deciduousScene->ready) {
        decMeshes = getComponents(g_deciduousScene, Mesh);
        if (decMeshes) {
            decCount = decMeshes->size;
            for (u32 i = 0; i < decMeshes->size; i++) {
                Mesh* mesh  = static_cast<Mesh*>(ssGetDataByIndex(decMeshes, i));
                for (u32 p = 0; p < arraySize(mesh->primitives); p++) {
                    decVertTotal += mesh->primitives[p].vertexCount;
                    decIdxTotal += mesh->primitives[p].indexCount;
                }
            }
        }
    }
    u32 decFarVertTotal = 0, decFarIdxTotal = 0, decFarCount = 0;
    SparseSet* decFarMeshes = NULL;
    if (g_deciduousFarScene && g_deciduousFarScene->ready) {
        decFarMeshes = getComponents(g_deciduousFarScene, Mesh);
        if (decFarMeshes) {
            decFarCount = decFarMeshes->size;
            for (u32 i = 0; i < decFarMeshes->size; i++) {
                Mesh* mesh  = static_cast<Mesh*>(ssGetDataByIndex(decFarMeshes, i));
                for (u32 p = 0; p < arraySize(mesh->primitives); p++) {
                    decFarVertTotal += mesh->primitives[p].vertexCount;
                    decFarIdxTotal += mesh->primitives[p].indexCount;
                }
            }
        }
    }
    bool useFar = (decFarCount > 0);

    // Per-species variant counts: near = N objects, far = M objects (the
    // simplified _far scene) or N when the far LOD is absent; others = 1.
    // If a species' authored model file exists but its scene is not ready
    // yet, the count is 0: the procedural placeholder must not render
    // while the real model is still loading (avoids the placeholder→model
    // pop).  No model file → keep the placeholder (count 1).
    for (u32 s = 0; s < AZGAAR_PROP_COUNT; s++) g_variantCount[s] = 1;
    if (g_deciduousPath) {
        g_variantCount[AZGAAR_PROP_DECIDUOUS] = (decCount > 0) ? decCount : 0;
    }
    if (g_deciduousFarPath) {
        g_variantCount[AZGAAR_PROP_DECIDUOUS_FAR] = useFar ? decFarCount : 0;
    }

    u32 totalVerts = 0, totalIdx = 0;
    for (u32 s = 0; s < AZGAAR_PROP_COUNT; s++) {
        if (g_variantCount[s] == 0)
            continue;  // awaiting its authored model: no placeholder geometry
        svCount[s] = builders[s]->vertCount;
        siCount[s] = builders[s]->idxCount;
        totalVerts += svCount[s];
        totalIdx += siCount[s];
    }
    totalVerts += decVertTotal + (useFar ? decFarVertTotal : 0);
    totalIdx += decIdxTotal + (useFar ? decFarIdxTotal : 0);

    PropsVertex* mergedVerts = static_cast<PropsVertex*>(memoryAlloc(sizeof(PropsVertex) * totalVerts));
    u32* mergedIdx = static_cast<u32*>(memoryAlloc(sizeof(u32) * totalIdx));
    u32 vOff = 0, iOff = 0;
    u32 spIdxOffset[AZGAAR_PROP_COUNT];
    for (u32 s = 0; s < AZGAAR_PROP_COUNT; s++) {
        if (g_variantCount[s] == 0)
            continue;  // awaiting its authored model: no placeholder geometry
        spIdxOffset[s] = iOff;
        memcpy(mergedVerts + vOff, builders[s]->verts, sizeof(PropsVertex) * svCount[s]);
        // Remap this species' indices into the merged vertex space and append.
        for (u32 i = 0; i < siCount[s]; i++) {
            mergedIdx[iOff + i] = builders[s]->idx[i] + vOff;
        }
        vOff += svCount[s];
        iOff += siCount[s];
    }

    // Append the authored objects (near scene, then the simplified far scene
    // when present) after the procedural data.
    u32* decIdxOffsets  = static_cast<u32*>((decCount > 0) ? memoryAlloc(sizeof(u32) * decCount) : NULL);
    u32* decVCounts     = static_cast<u32*>((decCount > 0) ? memoryAlloc(sizeof(u32) * decCount) : NULL);
    u32* decICounts     = static_cast<u32*>((decCount > 0) ? memoryAlloc(sizeof(u32) * decCount) : NULL);
    float* decBMin      = static_cast<float*>((decCount > 0) ? memoryAlloc(sizeof(float) * 3 * decCount) : NULL);
    float* decBMax      = static_cast<float*>((decCount > 0) ? memoryAlloc(sizeof(float) * 3 * decCount) : NULL);
    if (decMeshes && decCount > 0) {
        appendSceneMeshes(decMeshes,
                          mergedVerts,
                          &vOff,
                          mergedIdx,
                          &iOff,
                          decIdxOffsets,
                          decVCounts,
                          decICounts,
                          decBMin,
                          decBMax);
    }
    u32* decFarIdxOffsets  = static_cast<u32*>((useFar) ? memoryAlloc(sizeof(u32) * decFarCount) : NULL);
    u32* decFarVCounts     = static_cast<u32*>((useFar) ? memoryAlloc(sizeof(u32) * decFarCount) : NULL);
    u32* decFarICounts     = static_cast<u32*>((useFar) ? memoryAlloc(sizeof(u32) * decFarCount) : NULL);
    float* decFarBMin      = static_cast<float*>((useFar) ? memoryAlloc(sizeof(float) * 3 * decFarCount) : NULL);
    float* decFarBMax      = static_cast<float*>((useFar) ? memoryAlloc(sizeof(float) * 3 * decFarCount) : NULL);
    if (decFarMeshes && useFar) {
        appendSceneMeshes(decFarMeshes,
                          mergedVerts,
                          &vOff,
                          mergedIdx,
                          &iOff,
                          decFarIdxOffsets,
                          decFarVCounts,
                          decFarICounts,
                          decFarBMin,
                          decFarBMax);
    }

    // Build the flat (species, variant) metadata table (+ cull spheres).
    u32 totalRows = 0;
    for (u32 s = 0; s < AZGAAR_PROP_COUNT; s++) totalRows += g_variantCount[s];
    PropVariantRange* variants = static_cast<PropVariantRange*>(memoryAlloc(sizeof(PropVariantRange) * totalRows));
    memoryFree(g_variantSphereC);
    memoryFree(g_variantSphereR);
    g_variantSphereC     = static_cast<float*>(memoryAlloc(sizeof(float) * 3 * totalRows));
    g_variantSphereR     = static_cast<float*>(memoryAlloc(sizeof(float) * totalRows));
    g_variantSphereRows = totalRows;
    u32 row             = 0;
    for (u32 s = 0; s < AZGAAR_PROP_COUNT; s++) {
        u32 vc = g_variantCount[s];
        if (vc == 0) continue;  // species awaiting its authored model: no row, no placeholder
        bool isDeciduous = (s == AZGAAR_PROP_DECIDUOUS || s == AZGAAR_PROP_DECIDUOUS_FAR);
        if (isDeciduous && decCount > 0) {
            // One row per authored object.  The far LOD uses the simplified
            // _far scene when present, else reuses the near geometry.
            bool useFarData = (s == AZGAAR_PROP_DECIDUOUS_FAR) && useFar;
            u32* off        = useFarData ? decFarIdxOffsets : decIdxOffsets;
            u32* icnt       = useFarData ? decFarICounts : decICounts;
            float* bmin     = useFarData ? decFarBMin : decBMin;
            float* bmax     = useFarData ? decFarBMax : decBMax;
            for (u32 v = 0; v < vc; v++) {
                PropVariantRange* vr = &variants[row];
                vr->species          = s;
                vr->variant          = v;
                vr->indexOffset      = off[v];
                vr->indexCount       = icnt[v];
                for (u32 c = 0; c < 3; c++) {
                    vr->boundsMin[c] = bmin[v * 3 + c];
                    vr->boundsMax[c] = bmax[v * 3 + c];
                }
                vr->swayFactor = kSpecies[s].sway;
                vr->flags      = kSpecies[s].flags;
                vr->lodRole    = propsLodRole(s);
                propsStoreVariantSphere(row, vr->boundsMin, vr->boundsMax);
                row++;
            }
        } else {
            // Single procedural variant (all non-deciduous species, or deciduous
            // when the model is absent → placeholder fallback).
            PropVariantRange* vr = &variants[row];
            vr->species          = s;
            vr->variant          = 0;
            vr->indexOffset      = spIdxOffset[s];
            vr->indexCount       = siCount[s];
            float bmin[3]        = {1e9f, 1e9f, 1e9f};
            float bmax[3]        = {-1e9f, -1e9f, -1e9f};
            for (u32 v = 0; v < svCount[s]; v++) {
                for (u32 c = 0; c < 3; c++) {
                    float val = builders[s]->verts[v].position[c];
                    if (val < bmin[c]) bmin[c] = val;
                    if (val > bmax[c]) bmax[c] = val;
                }
            }
            for (u32 c = 0; c < 3; c++) {
                vr->boundsMin[c] = bmin[c];
                vr->boundsMax[c] = bmax[c];
            }
            vr->swayFactor = kSpecies[s].sway;
            vr->flags      = kSpecies[s].flags;
            vr->lodRole    = propsLodRole(s);
            propsStoreVariantSphere(row, bmin, bmax);
            row++;
        }
    }

    *outVertCount    = totalVerts;
    *outIdxCount     = totalIdx;
    *outVerts        = mergedVerts;
    *outIdx          = mergedIdx;
    *outVariants     = variants;
    *outVariantCount = totalRows;

    memoryFree(decIdxOffsets);
    memoryFree(decVCounts);
    memoryFree(decICounts);
    memoryFree(decBMin);
    memoryFree(decBMax);
    memoryFree(decFarIdxOffsets);
    memoryFree(decFarVCounts);
    memoryFree(decFarICounts);
    memoryFree(decFarBMin);
    memoryFree(decFarBMax);

    for (u32 s = 0; s < AZGAAR_PROP_COUNT; s++) {
        mbFree(builders[s]);
        memoryFree(builders[s]);
    }
}

// ── Deterministic RNG + fBm clumping noise (world-anchored) ────────────────

static u32 propsHash3(u32 a, u32 b, u32 c) {
    u32 h = a * 0x8da6b343u ^ b * 0xc2b2ae35u ^ c * 0x27d4eb2fu;
    h     = (h ^ (h >> 15)) * 0x2c1b3c6du;
    h     = (h ^ (h >> 12)) * 0x297a2d39u;
    return h ^ (h >> 15);
}

// Stable [0,1) random from a tile seed + texel index + salt.  Pure function of
// (mapSeed, tileX, tileZ, texX, texZ, salt) → eviction + regeneration is
// bit-identical.
static float propsRand(u32 tileSeed, u32 tx, u32 tz, u32 salt) {
    u32 h = propsHash3(tileSeed ^ (tx * 0x9E3779B9u) ^ (tz * 0x85EBCA77u), salt, 0x9E3779B9u);
    return (float)(h >> 8) / 16777216.0f;
}

// Two-octave value noise in WORLD space (lattice 10 m / 20 m) for the
// vegetation clumping gate (plan workstream B step 5).  Fixed seed so the
// patch pattern is map-stable.  Output in [0,1].
static const u32 kFbmSeed = 0x9E3779B9u;

static float smoothstep01(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

static float valueNoise10(float x, float z, u32 seed) {
    i32 xi   = (i32)floorf(x);
    i32 zi   = (i32)floorf(z);
    float xf = x - (float)xi;
    float zf = z - (float)zi;
    float u  = xf * xf * (3.0f - 2.0f * xf);
    float v  = zf * zf * (3.0f - 2.0f * zf);
    u32 kx = (u32)xi, kz = (u32)zi;
    float a      = (float)((propsHash3(kx, kz, seed) >> 8) & 0xFFFFFF) / 16777216.0f;
    float b      = (float)((propsHash3(kx + 1, kz, seed) >> 8) & 0xFFFFFF) / 16777216.0f;
    float c      = (float)((propsHash3(kx, kz + 1, seed) >> 8) & 0xFFFFFF) / 16777216.0f;
    float d      = (float)((propsHash3(kx + 1, kz + 1, seed) >> 8) & 0xFFFFFF) / 16777216.0f;
    float top    = a + (b - a) * u;
    float bottom = c + (d - c) * u;
    return top + (bottom - top) * v;  // [0,1]
}

static float propsClumpNoise(float wx, float wz) {
    float n = 0.6f * valueNoise10(wx * 0.1f, wz * 0.1f, kFbmSeed);
    n += 0.3f * valueNoise10(wx * 0.05f, wz * 0.05f, kFbmSeed + 1013904223u);
    return n;  // ~[0,0.9]
}

// ── Road distance hash (section-37 routes, "no trees on roads" gate) ──────
// A coarse uniform-grid hash of road centreline points (world space, `bucket`
// m buckets).  Query checks the 3x3 neighbourhood for any point within `maxD`.
typedef struct RoadHash {
    u32 bucket;        // bucket edge (m)
    float invBucket;   // 1 / bucket
    float minX, minZ;  // world-space origin of bucket (0,0)
    u32 gridW, gridH;  // bucket grid dimensions
    u32* starts;       // bucket -> index into points (0 = empty)
    u32 bucketCount;
    float* pts;  // [x, z] pairs
    u32 pointCount;
} RoadHash;

static void roadHashBuild(RoadHash* rh, const AzgaarWorld* world, float bucket) {
    *rh = (RoadHash){0};
    if (!world || world->routeCount == 0) return;
    rh->bucket    = (u32)bucket;
    rh->invBucket = 1.0f / bucket;
    // Count land routes (roads + trails; searoutes have no ground clearance).
    u32 totalPoints = 0;
    for (u32 r = 0; r < world->routeCount; r++) {
        AzgaarRoute* route = &world->routes[r];
        if (route->group == AZGAAR_ROUTE_SEAROUTE) continue;
        totalPoints += route->pointCount;
    }
    if (totalPoints == 0) return;
    rh->pointCount = totalPoints;
    rh->pts         = static_cast<float*>(memoryAlloc(sizeof(float) * 2 * totalPoints));

    // World extent → bucket grid dimensions (map is metres; use a padded box).
    float halfW     = (float)(world->widthPx * 0.5) * (float)world->metersPerPixel + 512.0f;
    float halfH     = (float)(world->heightPx * 0.5) * (float)world->metersPerPixel + 512.0f;
    rh->minX        = -halfW;  // map is centred at world origin
    rh->minZ        = -halfH;
    u32 gridW       = (u32)(2.0f * halfW / bucket) + 1u;
    u32 gridH       = (u32)(2.0f * halfH / bucket) + 1u;
    rh->gridW       = gridW;
    rh->gridH       = gridH;
    rh->bucketCount = gridW * gridH;
    rh->starts       = static_cast<u32*>(memoryAlloc(sizeof(u32) * rh->bucketCount));
    memset(rh->starts, 0, sizeof(u32) * rh->bucketCount);

    // Two passes: count points per bucket, prefix-sum to starts, then place.
    u32* counts = static_cast<u32*>(memoryAlloc(sizeof(u32) * rh->bucketCount));
    memset(counts, 0, sizeof(u32) * rh->bucketCount);
    u32 write = 0;
    for (u32 r = 0; r < world->routeCount; r++) {
        AzgaarRoute* route = &world->routes[r];
        if (route->group == AZGAAR_ROUTE_SEAROUTE) continue;
        for (u32 p = 0; p < route->pointCount; p++) {
            float wx, wz;
            azgaarMapToWorld(world, route->points[p].x, route->points[p].y, &wx, &wz);
            i32 bx = (i32)((wx - rh->minX) * rh->invBucket);
            i32 bz = (i32)((wz - rh->minZ) * rh->invBucket);
            if (bx < 0) bx = 0;
            if (bz < 0) bz = 0;
            if (bx >= (i32)gridW) bx = (i32)gridW - 1;
            if (bz >= (i32)gridH) bz = (i32)gridH - 1;
            u32 b                  = (u32)bz * gridW + (u32)bx;
            rh->pts[write * 2]     = wx;
            rh->pts[write * 2 + 1] = wz;
            counts[b]++;
            write++;
        }
    }
    u32 acc = 0;
    for (u32 b = 0; b < rh->bucketCount; b++) {
        rh->starts[b] = acc;
        acc += counts[b];
    }
    // Second pass: place each point into its bucket (stable order not required).
    u32* cursor = static_cast<u32*>(memoryAlloc(sizeof(u32) * rh->bucketCount));
    memcpy(cursor, rh->starts, sizeof(u32) * rh->bucketCount);
    write = 0;
    for (u32 r = 0; r < world->routeCount; r++) {
        AzgaarRoute* route = &world->routes[r];
        if (route->group == AZGAAR_ROUTE_SEAROUTE) continue;
        for (u32 p = 0; p < route->pointCount; p++) {
            float wx, wz;
            azgaarMapToWorld(world, route->points[p].x, route->points[p].y, &wx, &wz);
            i32 bx = (i32)((wx - rh->minX) * rh->invBucket);
            i32 bz = (i32)((wz - rh->minZ) * rh->invBucket);
            if (bx < 0) bx = 0;
            if (bz < 0) bz = 0;
            if (bx >= (i32)gridW) bx = (i32)gridW - 1;
            if (bz >= (i32)gridH) bz = (i32)gridH - 1;
            u32 b                      = (u32)bz * gridW + (u32)bx;
            rh->pts[cursor[b] * 2]     = wx;
            rh->pts[cursor[b] * 2 + 1] = wz;
            cursor[b]++;
        }
    }
    memoryFree(counts);
    memoryFree(cursor);
}

// True when (wx, wz) is within `maxD` metres of any road centreline point.
static bool roadNear(RoadHash* rh, float wx, float wz, float maxD) {
    if (!rh || rh->pointCount == 0) return false;
    u32 gridW = rh->gridW, gridH = rh->gridH;
    if (gridW == 0 || gridH == 0) return false;
    i32 bx      = (i32)((wx - rh->minX) * rh->invBucket);
    i32 bz      = (i32)((wz - rh->minZ) * rh->invBucket);
    float d2max = maxD * maxD;
    for (i32 oz = -1; oz <= 1; oz++) {
        for (i32 ox = -1; ox <= 1; ox++) {
            i32 nx = bx + ox, nz = bz + oz;
            if (nx < 0 || nz < 0 || nx >= (i32)gridW || nz >= (i32)gridH) continue;
            u32 b  = (u32)nz * gridW + (u32)nx;
            u32 lo = rh->starts[b];
            u32 hi = (b + 1 < rh->bucketCount) ? rh->starts[b + 1] : rh->pointCount;
            for (u32 i = lo; i < hi; i++) {
                float dx = rh->pts[i * 2] - wx;
                float dz = rh->pts[i * 2 + 1] - wz;
                if (dx * dx + dz * dz < d2max) return true;
            }
        }
    }
    return false;
}

// ── Per-biome species mix (from the biome table's icons / iconsDensity) ──

typedef struct BiomeSpecies {
    u32 species[AZGAAR_PROP_COUNT];
    u32 weight[AZGAAR_PROP_COUNT];
    u32 count;
    u32 totalWeight;
} BiomeSpecies;

#define AZGAAR_PROPS_MAX_BIOMES 32

// Per-species within-patch base density (instances/m^2 before the biome's
// iconsDensity/120 scale).  Grass is dense, forests moderate, deserts sparse
// (workstream B density table, expressed per species so it generalises across
// maps rather than hardcoding per-biome species).
static const float kSpeciesDensity[AZGAAR_PROP_COUNT] = {
    [AZGAAR_PROP_GRASS_TUFT]    = 0.06f,
    [AZGAAR_PROP_CONIFER]       = 0.008f,
    [AZGAAR_PROP_CONIFER_FAR]   = 0.008f,
    [AZGAAR_PROP_DECIDUOUS]     = 0.008f,
    [AZGAAR_PROP_DECIDUOUS_FAR] = 0.008f,
    [AZGAAR_PROP_ACACIA]        = 0.008f,
    [AZGAAR_PROP_PALM]          = 0.008f,
    [AZGAAR_PROP_CACTUS]        = 0.001f,
    [AZGAAR_PROP_DEAD_TREE]     = 0.0002f,
    [AZGAAR_PROP_REED]          = 0.015f,
    [AZGAAR_PROP_SHRUB]         = 0.004f,
    [AZGAAR_PROP_ROCK]          = 0.0f,
    [AZGAAR_PROP_FLOWER]        = 0.004f,
    // Buildings never enter the biome scatter; AzgaarSettlements places them.
    [AZGAAR_PROP_HUT]    = 0.0f,
    [AZGAAR_PROP_HOUSE]  = 0.0f,
    [AZGAAR_PROP_TOWER]  = 0.0f,
    [AZGAAR_PROP_WALL]   = 0.0f,
    [AZGAAR_PROP_TEMPLE] = 0.0f,
    [AZGAAR_PROP_DOCK]   = 0.0f,
    [AZGAAR_PROP_GATE]   = 0.0f,
    // Landmarks are placed by AzgaarLandmarks from section-35 markers.
    [AZGAAR_PROP_VOLCANO]        = 0.0f,
    [AZGAAR_PROP_LIGHTHOUSE]     = 0.0f,
    [AZGAAR_PROP_LIGHTHOUSE_CAP] = 0.0f,
    [AZGAAR_PROP_RUIN_COLUMN]    = 0.0f,
    [AZGAAR_PROP_RUIN_ARCH]      = 0.0f,
    [AZGAAR_PROP_MINE_FRAME]     = 0.0f,
    [AZGAAR_PROP_BRIDGE]         = 0.0f,
};

// Per-species minimum separation (m) for the overlap gate.  Trees get the
// largest value (placeholder canopy radius reaches ~3.4 m at max scale) so
// trees on the ~4 m candidate lattice stop interpenetrating; grass/reed/
// shrub are small; buildings/landmarks are 0 (placed by their own modules,
// never by the biome scatter).
static const float kMinDist[AZGAAR_PROP_COUNT] = {
    [AZGAAR_PROP_GRASS_TUFT] = 1.5f,     [AZGAAR_PROP_CONIFER] = 5.0f,
    [AZGAAR_PROP_CONIFER_FAR] = 5.0f,    [AZGAAR_PROP_DECIDUOUS] = 5.0f,
    [AZGAAR_PROP_DECIDUOUS_FAR] = 5.0f,  [AZGAAR_PROP_ACACIA] = 5.0f,
    [AZGAAR_PROP_PALM] = 5.0f,           [AZGAAR_PROP_CACTUS] = 2.0f,
    [AZGAAR_PROP_DEAD_TREE] = 4.0f,      [AZGAAR_PROP_REED] = 1.5f,
    [AZGAAR_PROP_SHRUB] = 1.5f,          [AZGAAR_PROP_ROCK] = 3.0f,
    [AZGAAR_PROP_FLOWER] = 1.0f,         [AZGAAR_PROP_HUT] = 0.0f,
    [AZGAAR_PROP_HOUSE] = 0.0f,          [AZGAAR_PROP_TOWER] = 0.0f,
    [AZGAAR_PROP_WALL] = 0.0f,           [AZGAAR_PROP_TEMPLE] = 0.0f,
    [AZGAAR_PROP_DOCK] = 0.0f,           [AZGAAR_PROP_GATE] = 0.0f,
    [AZGAAR_PROP_VOLCANO] = 0.0f,        [AZGAAR_PROP_LIGHTHOUSE] = 0.0f,
    [AZGAAR_PROP_LIGHTHOUSE_CAP] = 0.0f, [AZGAAR_PROP_RUIN_COLUMN] = 0.0f,
    [AZGAAR_PROP_RUIN_ARCH] = 0.0f,      [AZGAAR_PROP_MINE_FRAME] = 0.0f,
    [AZGAAR_PROP_BRIDGE] = 0.0f,
};

// Canopy radius as a fraction of the instance's height (scale), read from the
// placeholder mesh builders (conifer cone 0.38, deciduous blob 0.42, acacia
// disc 0.55, palm fronds 0.40, cactus 0.14, dead tree 0.12).  The overlap
// gate uses this to reject two trees whose canopy discs would intersect
// (center distance < r_cand + r_placed).  Non-tree species keep the fixed
// kMinDist floor only (zero canopy factor).
static const float kCanopyFactor[AZGAAR_PROP_COUNT] = {
    [AZGAAR_PROP_GRASS_TUFT] = 0.0f,     [AZGAAR_PROP_CONIFER] = 0.55f,
    [AZGAAR_PROP_CONIFER_FAR] = 0.50f,   [AZGAAR_PROP_DECIDUOUS] = 0.80f,
    [AZGAAR_PROP_DECIDUOUS_FAR] = 0.70f, [AZGAAR_PROP_ACACIA] = 0.70f,
    [AZGAAR_PROP_PALM] = 0.60f,          [AZGAAR_PROP_CACTUS] = 0.14f,
    [AZGAAR_PROP_DEAD_TREE] = 0.12f,     [AZGAAR_PROP_REED] = 0.0f,
    [AZGAAR_PROP_SHRUB] = 0.0f,          [AZGAAR_PROP_ROCK] = 0.0f,
    [AZGAAR_PROP_FLOWER] = 0.0f,         [AZGAAR_PROP_HUT] = 0.0f,
    [AZGAAR_PROP_HOUSE] = 0.0f,          [AZGAAR_PROP_TOWER] = 0.0f,
    [AZGAAR_PROP_WALL] = 0.0f,           [AZGAAR_PROP_TEMPLE] = 0.0f,
    [AZGAAR_PROP_DOCK] = 0.0f,           [AZGAAR_PROP_GATE] = 0.0f,
    [AZGAAR_PROP_VOLCANO] = 0.0f,        [AZGAAR_PROP_LIGHTHOUSE] = 0.0f,
    [AZGAAR_PROP_LIGHTHOUSE_CAP] = 0.0f, [AZGAAR_PROP_RUIN_COLUMN] = 0.0f,
    [AZGAAR_PROP_RUIN_ARCH] = 0.0f,      [AZGAAR_PROP_MINE_FRAME] = 0.0f,
    [AZGAAR_PROP_BRIDGE] = 0.0f,
};

#define AZGAAR_PROPS_TILE_CAP 500000u  // hard per-tile instance cap (plan B)

// ── Placed-instance hash (overlap gate) ───────────────────────────────────
// Per-tile uniform-grid hash of instances already placed during the scatter.
// Before committing a candidate, the gate rejects it if any placed instance
// is within the max of the two species' minimum separations, so trees (and
// rocks) on the ~4 m candidate lattice stop interpenetrating.  The hash is
// job-local (rebuilt per tile), keeping the scatter deterministic.
typedef struct PlacedHash {
    u32 bucket;  // bucket edge (m)
    float invBucket;
    float minX, minZ;  // tile origin (world)
    u32 gridW, gridH;
    u32 bucketCount;
    u32* head;     // bucket -> first point index (0xFFFFFFFF = empty)
    float* pts;    // [x, z] pairs
    u32* sp;       // species id per point
    float* scale;  // instance height (m) per point (drives canopy radius)
    i32* next;     // next point index within the same bucket
    u32 count;
} PlacedHash;

static void placedHashBuild(PlacedHash* ph, float minX, float minZ, float sizeM) {
    *ph                = (PlacedHash){0};
    const float bucket = 8.0f;
    ph->bucket         = (u32)bucket;
    ph->invBucket      = 1.0f / bucket;
    ph->minX           = minX;
    ph->minZ           = minZ;
    ph->gridW          = (u32)(sizeM / bucket) + 1u;
    ph->gridH          = (u32)(sizeM / bucket) + 1u;
    ph->bucketCount    = ph->gridW * ph->gridH;
    ph->head            = static_cast<u32*>(memoryAlloc(sizeof(u32) * ph->bucketCount));
    memset(ph->head, 0xFFFFFFFF, sizeof(u32) * ph->bucketCount);
    const u32 cap = AZGAAR_PROPS_TILE_CAP;
    ph->pts        = static_cast<float*>(memoryAlloc(sizeof(float) * 2 * cap));
    ph->sp         = static_cast<u32*>(memoryAlloc(sizeof(u32) * cap));
    ph->scale      = static_cast<float*>(memoryAlloc(sizeof(float) * cap));
    ph->next       = static_cast<i32*>(memoryAlloc(sizeof(i32) * cap));
    ph->count     = 0;
}

static void placedHashFree(PlacedHash* ph) {
    memoryFree(ph->head);
    memoryFree(ph->pts);
    memoryFree(ph->sp);
    memoryFree(ph->scale);
    memoryFree(ph->next);
    *ph = (PlacedHash){0};
}

static u32 placedBucket(PlacedHash* ph, float x, float z) {
    i32 bx = (i32)((x - ph->minX) * ph->invBucket);
    i32 bz = (i32)((z - ph->minZ) * ph->invBucket);
    if (bx < 0) bx = 0;
    if (bz < 0) bz = 0;
    if (bx >= (i32)ph->gridW) bx = (i32)ph->gridW - 1;
    if (bz >= (i32)ph->gridH) bz = (i32)ph->gridH - 1;
    return (u32)bz * ph->gridW + (u32)bx;
}

static void placedHashInsert(PlacedHash* ph, float x, float z, u32 species, float scale) {
    u32 i              = ph->count;
    ph->pts[i * 2]     = x;
    ph->pts[i * 2 + 1] = z;
    ph->sp[i]          = species;
    ph->scale[i]       = scale;
    u32 b              = placedBucket(ph, x, z);
    ph->next[i]        = (i32)ph->head[b];
    ph->head[b]        = i;
    ph->count++;
}

// True if (x, z) is too close to a placed instance.  Required separation is
// the max of the fixed per-species floor (kMinDist) and the sum of the two
// canopy radii (kCanopyFactor * scale), so two trees whose canopy discs would
// intersect are rejected.  Non-tree species (canopy factor 0) fall back to
// the kMinDist floor only.
static bool placedTooClose(PlacedHash* ph, float x, float z, u32 candSpecies, float candScale) {
    if (ph->count == 0) return false;
    i32 bx = (i32)((x - ph->minX) * ph->invBucket);
    i32 bz = (i32)((z - ph->minZ) * ph->invBucket);
    for (i32 oz = -1; oz <= 1; oz++) {
        for (i32 ox = -1; ox <= 1; ox++) {
            i32 nx = bx + ox, nz = bz + oz;
            if (nx < 0 || nz < 0 || nx >= (i32)ph->gridW || nz >= (i32)ph->gridH) continue;
            u32 b = (u32)nz * ph->gridW + (u32)nx;
            for (i32 i = (i32)ph->head[b]; i >= 0; i = ph->next[i]) {
                float dx = ph->pts[i * 2] - x;
                float dz = ph->pts[i * 2 + 1] - z;
                // Fixed floor (small species) + canopy-disc intersection (trees).
                // Ground-level candidates (grass, flower, reed, shrub) only
                // respect their own min distance — a placed tree's large
                // minDist/canopy must not block grass from growing at its base.
                float sep = kMinDist[candSpecies];
                if (kCanopyFactor[candSpecies] > 0.0f) {
                    if (kMinDist[ph->sp[i]] > sep) sep = kMinDist[ph->sp[i]];
                    float need = kCanopyFactor[candSpecies] * candScale +
                                 kCanopyFactor[ph->sp[i]] * ph->scale[i];
                    if (need > sep) sep = need;
                }
                if (sep > 0.0f && dx * dx + dz * dz < sep * sep) return true;
            }
        }
    }
    return false;
}

static u32 iconToSpecies(const char* name) {
    if (!name || !name[0]) return (u32)-1;
    if (strcmp(name, "grass") == 0) return AZGAAR_PROP_GRASS_TUFT;
    if (strcmp(name, "conifer") == 0) return AZGAAR_PROP_CONIFER;
    if (strcmp(name, "deciduous") == 0) return AZGAAR_PROP_DECIDUOUS;
    if (strcmp(name, "acacia") == 0) return AZGAAR_PROP_ACACIA;
    if (strcmp(name, "palm") == 0) return AZGAAR_PROP_PALM;
    if (strcmp(name, "cactus") == 0) return AZGAAR_PROP_CACTUS;
    if (strcmp(name, "deadTree") == 0) return AZGAAR_PROP_DEAD_TREE;
    if (strcmp(name, "swamp") == 0) return AZGAAR_PROP_REED;
    if (strcmp(name, "shrub") == 0) return AZGAAR_PROP_SHRUB;
    if (strcmp(name, "flower") == 0) return AZGAAR_PROP_FLOWER;
    if (strcmp(name, "rock") == 0) return AZGAAR_PROP_ROCK;
    return (u32)-1;  // dune and others: no placeholder mesh yet
}

// Precompute the per-biome species mix (weighted by icon repetition) from the
// world's biome table.  Grassy biomes get a small flower sprinkle added.
static void precomputeBiomeSpecies(const AzgaarWorld* world, BiomeSpecies* out, u32 count) {
    for (u32 b = 0; b < count && b < world->biomeCount; b++) {
        out[b]                   = (BiomeSpecies){0};
        const AzgaarBiome* biome = &world->biomes[b];
        if (biome->iconCount == 0) continue;
        // Count each species' weight (repetition).  Fold duplicates into one slot.
        for (u32 k = 0; k < biome->iconCount; k++) {
            u32 sp = iconToSpecies(biome->icons[k]);
            if (sp == (u32)-1 || sp >= AZGAAR_PROP_COUNT) continue;
            u32 slot = (u32)-1;
            for (u32 s = 0; s < out[b].count; s++) {
                if (out[b].species[s] == sp) {
                    slot = s;
                    break;
                }
            }
            if (slot == (u32)-1) {
                slot                 = out[b].count++;
                out[b].species[slot] = sp;
            }
            out[b].weight[slot]++;
            out[b].totalWeight++;
        }
        // Grassy biomes: add a flower sprinkle (10% of the grass weight).
        u32 grassW = 0;
        for (u32 s = 0; s < out[b].count; s++) {
            if (out[b].species[s] == AZGAAR_PROP_GRASS_TUFT) grassW = out[b].weight[s];
        }
        if (grassW > 0) {
            u32 fslot = (u32)-1;
            for (u32 s = 0; s < out[b].count; s++) {
                if (out[b].species[s] == AZGAAR_PROP_FLOWER) {
                    fslot = s;
                    break;
                }
            }
            if (fslot == (u32)-1) {
                fslot                 = out[b].count++;
                out[b].species[fslot] = AZGAAR_PROP_FLOWER;
            }
            out[b].weight[fslot] += (u32)(grassW * 0.1f + 0.5f);
            out[b].totalWeight += (u32)(grassW * 0.1f + 0.5f);
        }
    }
}

// ── Per-tile scatter state + job ───────────────────────────────────────────

typedef struct PropsTileState {
    i32 tileX, tileZ;
    u64 readyStamp;    // readyStamp this tile was built for
    u32 propsVersion;  // propsVersion this tile was scattered with
    float camX, camZ;  // build-time camera (bakes the near/far LOD; re-scatter when it moves)
    bool inUse;
    bool building;            // a scatter job is in flight for this tile
    PropInstance* instances;  // grouped-by-species, persistent (uploaded)
    u32 instanceCount;
    PropTileRange* ranges;
    u32 rangeCount;
    u32 perSpecies[AZGAAR_PROP_COUNT];  // debug counts
    // Per-instance culling (v2): the full set above is the source of truth;
    // cullInstances is the compacted set currently uploaded to the pass
    // (cullCount == instanceCount means the full set is uploaded).
    PropInstance* cullInstances;
    u32 cullCount;
    PropTileRange* cullRanges;
    u32 cullRangeCount;
    bool needsCull;  // a cull is pending (fresh scatter)
    bool culling;    // a cull job is in flight
    // Camera the cullInstances upload was captured for (scatter-time compact
    // or last cull job): within PROPS_CULL_MOVE / PROPS_CULL_ROT of it the
    // uploaded set is still fresh and the cull pass skips re-culling.
    float cullCamPos[3];
    float cullYaw, cullPitch;
    bool cullCamValid;
} PropsTileState;

static const AzgaarWorld* g_world = NULL;
static u32 g_mapSeed;
static ThreadPool* g_pool;
static RoadHash g_roadHash;
static BiomeSpecies g_biomeSpecies[AZGAAR_PROPS_MAX_BIOMES];
static Array(PropsTileState) g_tiles = {};
static Thread g_stateLock            = {.mutex = PTHREAD_MUTEX_INITIALIZER};
static bool g_initialized;
static bool g_propsDebug;
static bool g_propsDisabled;

static void propsTileDrop(PropsTileState* t) {
    memoryFree(t->instances);
    memoryFree(t->ranges);
    memoryFree(t->cullInstances);
    memoryFree(t->cullRanges);
    *t = (PropsTileState){0};
}

// Drops all CPU tile state (called when the variant table is rebuilt so the
// update loop re-scatters every resident tile with the new variant counts).
static void propsInvalidateTiles(void) {
    threadLock(&g_stateLock);
    for (u32 i = 0; i < arraySize(g_tiles); i++) {
        if (g_tiles[i].inUse) {
            propsTileDrop(&g_tiles[i]);
            arrayDeleteSlow(g_tiles, i);
            i--;
        }
    }
    threadUnlock(&g_stateLock);
}

// ── Per-instance culling (v2) ─────────────────────────────────────────────
// The scatter produces the full per-tile instance set (source of truth); the
// cull stage compacts it to what the current camera can see (frustum +
// per-species distance cap) and uploads the compacted set.  Re-culling is
// dirty-checked: only when the camera has moved more than PROPS_CULL_MOVE or
// rotated more than PROPS_CULL_ROT since the last cull, and only for tiles
// whose inflated AABB actually straddles the frustum edge or a distance cap.
// The margin keeps stale culls between re-culls from popping (screen-space
// error shrinks with distance, so the far field is safe).

// Per-species distance caps (metres): trees 800, grass/reed/flower/shrub/
// cactus 400, rocks 500, buildings + landmarks 2000.
static const float kCullDist[AZGAAR_PROP_COUNT] = {
    [AZGAAR_PROP_GRASS_TUFT]     = 400.0f,
    [AZGAAR_PROP_CONIFER]        = 800.0f,
    [AZGAAR_PROP_CONIFER_FAR]    = 800.0f,
    [AZGAAR_PROP_DECIDUOUS]      = 800.0f,
    [AZGAAR_PROP_DECIDUOUS_FAR]  = 800.0f,
    [AZGAAR_PROP_ACACIA]         = 800.0f,
    [AZGAAR_PROP_PALM]           = 800.0f,
    [AZGAAR_PROP_CACTUS]         = 400.0f,
    [AZGAAR_PROP_DEAD_TREE]      = 800.0f,
    [AZGAAR_PROP_REED]           = 400.0f,
    [AZGAAR_PROP_SHRUB]          = 400.0f,
    [AZGAAR_PROP_ROCK]           = 500.0f,
    [AZGAAR_PROP_FLOWER]         = 400.0f,
    [AZGAAR_PROP_HUT]            = 2000.0f,
    [AZGAAR_PROP_HOUSE]          = 2000.0f,
    [AZGAAR_PROP_TOWER]          = 2000.0f,
    [AZGAAR_PROP_WALL]           = 2000.0f,
    [AZGAAR_PROP_TEMPLE]         = 2000.0f,
    [AZGAAR_PROP_DOCK]           = 2000.0f,
    [AZGAAR_PROP_GATE]           = 2000.0f,
    [AZGAAR_PROP_VOLCANO]        = 2000.0f,
    [AZGAAR_PROP_LIGHTHOUSE]     = 2000.0f,
    [AZGAAR_PROP_LIGHTHOUSE_CAP] = 2000.0f,
    [AZGAAR_PROP_RUIN_COLUMN]    = 2000.0f,
    [AZGAAR_PROP_RUIN_ARCH]      = 2000.0f,
    [AZGAAR_PROP_MINE_FRAME]     = 2000.0f,
    [AZGAAR_PROP_BRIDGE]         = 2000.0f,
};

#define PROPS_CULL_MARGIN 40.0f  // conservative margin on frustum + caps
#define PROPS_CULL_MOVE 8.0f     // re-cull when the camera moves more than this
#define PROPS_CULL_ROT (5.0f * (float)M_PI / 180.0f)  // ... or rotates more than this
// Frustum sweep between re-culls: a point at distance d moves ~d*sin(ROT) under
// a ROT rotation (plus MOVE of translation).  Inflates the per-instance frustum
// test so distant trees stay "warm" and don't pop as the frustum sweeps.
#define PROPS_CULL_ROT_SWEEP sinf(PROPS_CULL_ROT)

// Distance (m) at which trees switch to their far (simplified) LOD variant.
// It also doubles as the camera-movement threshold that triggers a tile
// re-scatter, so the baked near/far LOD tracks the live camera as the player
// moves (otherwise resident tiles keep the LOD they were scattered with).
#define PROPS_LOD_DIST 100.0f

// Hard LOD switch threshold (m): near LOD inside, far LOD outside.  Pushed to
// the GPU each frame (azgaar_props.vert steps on this live camera distance).
// Replaces the old near->far cross-fade band, which alpha-blended both LODs in
// a 40 m ring and forced the whole props pass into blend state.
#define PROPS_LOD_SWITCH 100.0f

// Hysteresis margin (m) around the switch.  The cull stage keeps BOTH LOD
// instances within [SWITCH-margin, SWITCH+margin] so a tree at the boundary
// does not flicker as the camera moves between re-culls; the vertex shader
// then shows only one (the hidden side collapses to a point).  Must cover the
// max camera movement between re-culls (PROPS_CULL_MOVE).
#define PROPS_LOD_CULL_MARGIN 16.0f

// Distance from a camera (cx,cz) to a tile's world AABB (origin..origin+size on
// both axes); 0 when the camera is inside the tile.  Used to decide which tiles
// are close enough for their LOD to matter.
static float propsDistCamToTile(float cx, float cz, float originX, float originZ, float size) {
    float dx = (cx < originX)          ? (originX - cx)
               : (cx > originX + size) ? (cx - (originX + size))
                                       : 0.0f;
    float dz = (cz < originZ)          ? (originZ - cz)
               : (cz > originZ + size) ? (cz - (originZ + size))
                                       : 0.0f;
    return sqrtf(dx * dx + dz * dz);
}

typedef struct CullCam {
    float pos[3];
    float planes[6][4];
    float yaw, pitch;
    bool valid;
} CullCam;

static CullCam g_cullCam = {};

// Whole-map global instance sets (settlement buildings / landmarks).  The
// owning module registers its full set once; the cull stage compacts it like
// the tiles and re-uploads it as the camera moves.
typedef struct PropsGlobalSet {
    bool inUse;
    PropInstance* instances;
    u32 instanceCount;
    PropTileRange* ranges;
    u32 rangeCount;
    float aabbMin[3], aabbMax[3];
    PropInstance* cullInstances;
    u32 cullCount;
    PropTileRange* cullRanges;
    u32 cullRangeCount;
    bool needsCull;
    bool culling;
} PropsGlobalSet;

static PropsGlobalSet g_globalSet   = {};  // settlement buildings
static PropsGlobalSet g_landmarkSet = {};  // landmarks

// One cull job (runs on the pool thread; the whole body is under g_stateLock,
// same lock order as the scatter job: g_stateLock → uploadLock).
typedef struct CullJob {
    bool isGlobal;
    bool landmarks;
    i32 tileX, tileZ;
    u64 readyStamp;
    float camPos[3];
    float planes[6][4];
    float yaw, pitch;
} CullJob;

// AABB vs frustum-plane helpers (plane inside test: dot(n,p)+w >= 0).
static bool propsAabbOutsideFrustum(const float bmin[3],
                                    const float bmax[3],
                                    const float planes[6][4]) {
    for (int i = 0; i < 6; i++) {
        float px = planes[i][0] >= 0.0f ? bmax[0] : bmin[0];
        float py = planes[i][1] >= 0.0f ? bmax[1] : bmin[1];
        float pz = planes[i][2] >= 0.0f ? bmax[2] : bmin[2];
        if (planes[i][0] * px + planes[i][1] * py + planes[i][2] * pz + planes[i][3] < 0.0f) {
            return true;
        }
    }
    return false;
}

static bool propsAabbInsideFrustum(const float bmin[3],
                                   const float bmax[3],
                                   const float planes[6][4]) {
    for (int i = 0; i < 6; i++) {
        float px = planes[i][0] >= 0.0f ? bmin[0] : bmax[0];
        float py = planes[i][1] >= 0.0f ? bmin[1] : bmax[1];
        float pz = planes[i][2] >= 0.0f ? bmin[2] : bmax[2];
        if (planes[i][0] * px + planes[i][1] * py + planes[i][2] * pz + planes[i][3] < 0.0f) {
            return false;
        }
    }
    return true;
}

// Squared distance from p to the farthest point of the AABB.
static float propsAabbFarDistSq(const float p[3], const float bmin[3], const float bmax[3]) {
    float dx = (p[0] < bmin[0])   ? (bmin[0] - p[0])
               : (p[0] > bmax[0]) ? (p[0] - bmax[0])
                                  : fmaxf(p[0] - bmin[0], bmax[0] - p[0]);
    float dy = (p[1] < bmin[1])   ? (bmin[1] - p[1])
               : (p[1] > bmax[1]) ? (p[1] - bmax[1])
                                  : fmaxf(p[1] - bmin[1], bmax[1] - p[1]);
    float dz = (p[2] < bmin[2])   ? (bmin[2] - p[2])
               : (p[2] > bmax[2]) ? (p[2] - bmax[2])
                                  : fmaxf(p[2] - bmin[2], bmax[2] - p[2]);
    return dx * dx + dy * dy + dz * dz;
}

// Squared distance from p to the closest point of the AABB (0 if p is inside).
static float propsAabbNearDistSq(const float p[3], const float bmin[3], const float bmax[3]) {
    float dx = (p[0] < bmin[0]) ? (bmin[0] - p[0]) : (p[0] > bmax[0]) ? (p[0] - bmax[0]) : 0.0f;
    float dy = (p[1] < bmin[1]) ? (bmin[1] - p[1]) : (p[1] > bmax[1]) ? (p[1] - bmax[1]) : 0.0f;
    float dz = (p[2] < bmin[2]) ? (bmin[2] - p[2]) : (p[2] > bmax[2]) ? (p[2] - bmax[2]) : 0.0f;
    return dx * dx + dy * dy + dz * dz;
}

// Compacts `src` (grouped by (species, variant) per `ranges`) into `dst`,
// keeping instances inside the frustum + per-species distance cap.  Fills
// `outRanges` (capacity >= rangeCount); returns the survivor counts via
// outCount / outRangeCount.
static void propsCullCompact(const PropInstance* src,
                             const PropTileRange* ranges,
                             u32 rangeCount,
                             PropInstance* dst,
                             PropTileRange* outRanges,
                             const float camPos[3],
                             const float planes[6][4],
                             u32* outCount,
                             u32* outRangeCount) {
    u32 base[AZGAAR_PROP_COUNT];
    u32 a = 0;
    for (u32 s = 0; s < AZGAAR_PROP_COUNT; s++) {
        base[s] = a;
        a += g_variantCount[s];
    }

    u32 w = 0, rc = 0;
    for (u32 r = 0; r < rangeCount; r++) {
        const PropTileRange* rng = &ranges[r];
        u32 sp                   = rng->species;
        if (sp >= AZGAAR_PROP_COUNT) continue;
        u32 c0 = rng->start, c1 = rng->start + rng->count;
        float cap = kCullDist[sp] + PROPS_CULL_MARGIN;
        cap *= cap;
        // LOD hard-switch gate (XZ distance, matching azgaar_props.vert): the
        // hidden LOD side collapses to a point on the GPU, so it needs no
        // upload outside the hysteresis ring — the GPU would only process its
        // geometry to collapse it.  Near is kept inside SWITCH+margin, far is
        // kept outside SWITCH-margin; both are kept in between (the VS picks).
        float lodMinSq = 0.0f, lodMaxSq = 0.0f;
        u32 role = propsLodRole(sp);
        if (role == 0) {
            float m  = PROPS_LOD_SWITCH + PROPS_LOD_CULL_MARGIN;
            lodMaxSq = m * m;
        } else if (role == 1) {
            float m  = PROPS_LOD_SWITCH - PROPS_LOD_CULL_MARGIN;
            lodMinSq = m * m;
        }
        u32 c = 0;
        for (u32 i = c0; i < c1; i++) {
            const PropInstance* inst = &src[i];
            float dx                 = inst->pos[0] - camPos[0];
            float dy                 = inst->pos[1] - camPos[1];
            float dz                 = inst->pos[2] - camPos[2];
            if (dx * dx + dy * dy + dz * dz > cap) continue;
            float xzsq = dx * dx + dz * dz;
            if ((lodMaxSq > 0.0f && xzsq > lodMaxSq) || (lodMinSq > 0.0f && xzsq < lodMinSq)) {
                continue;
            }
            u32 row = base[sp] + inst->variant;
            if (row < g_variantSphereRows) {
                float sx = inst->pos[0] + inst->scale * g_variantSphereC[row * 3 + 0];
                float sy = inst->pos[1] + inst->scale * g_variantSphereC[row * 3 + 1];
                float sz = inst->pos[2] + inst->scale * g_variantSphereC[row * 3 + 2];
                // Distance-scaled frustum margin: covers the frustum sweep
                // between re-culls (rotation ~d*sin(ROT), translation ~MOVE).
                float cdx   = sx - camPos[0];
                float cdy   = sy - camPos[1];
                float cdz   = sz - camPos[2];
                float cdist = sqrtf(cdx * cdx + cdy * cdy + cdz * cdz);
                float rad   = inst->scale * g_variantSphereR[row] + cdist * PROPS_CULL_ROT_SWEEP +
                              PROPS_CULL_MOVE;
                bool out    = false;
                for (int p = 0; p < 6; p++) {
                    const float* pl = planes[p];
                    if (pl[0] * sx + pl[1] * sy + pl[2] * sz + pl[3] < -rad) {
                        out = true;
                        break;
                    }
                }
                if (out) continue;
            }
            dst[w + c] = *inst;
            c++;
        }
        if (c > 0) {
            outRanges[rc] =
                (PropTileRange){.species = sp, .variant = rng->variant, .start = w, .count = c};
            rc++;
        }
        w += c;
    }
    *outCount      = w;
    *outRangeCount = rc;
}

// One cull job.  The compact runs under g_stateLock (it reads the tile's
// instance source), the GPU upload runs WITHOUT the lock on this worker
// thread (non-blocking submit; the pass adopts it once its fence signals),
// and the compacted buffers are job-owned until the hand-over (the tile
// could be evicted mid-upload).
static void propsCullJob(void* userData) {
    CullJob* job        = (CullJob*)userData;
    double cT0          = nanos();
    static int cHitchOn = -1;
    if (cHitchOn < 0) cHitchOn = getenv("ENGINE_HITCH_DEBUG") != NULL;
    if (job->isGlobal) {
        PropsGlobalSet* gs    = job->landmarks ? &g_landmarkSet : &g_globalSet;
        PropInstance* cullOut = NULL;
        PropTileRange* cullRO = NULL;
        u32 w = 0, rc = 0;
        float aabbMin[3], aabbMax[3];
        bool valid = false;
        threadLock(&g_stateLock);
        if (gs->inUse && gs->culling) {
            gs->culling = false;
            if (gs->instanceCount > 0) {
                cullOut  = static_cast<PropInstance*>(memoryAlloc(sizeof(PropInstance) * gs->instanceCount));
                cullRO   = static_cast<PropTileRange*>(memoryAlloc(sizeof(PropTileRange) * gs->rangeCount));
                propsCullCompact(gs->instances,
                                 gs->ranges,
                                 gs->rangeCount,
                                 cullOut,
                                 cullRO,
                                 job->camPos,
                                 job->planes,
                                 &w,
                                 &rc);
                memcpy(aabbMin, gs->aabbMin, sizeof(aabbMin));
                memcpy(aabbMax, gs->aabbMax, sizeof(aabbMax));
                valid = true;
                if (g_propsDebug) {
                    info("azgaarProps: cull %s: %u -> %u instances",
                         job->landmarks ? "landmarks" : "settlements",
                         gs->instanceCount,
                         w);
                }
            }
        }
        threadUnlock(&g_stateLock);

        if (valid) {
            if (job->landmarks) {
                vulkanAzgaarPropsSetLandmarks(cullOut, w, cullRO, rc, aabbMin, aabbMax);
            } else {
                vulkanAzgaarPropsSetGlobal(cullOut, w, cullRO, rc, aabbMin, aabbMax);
            }
            threadLock(&g_stateLock);
            if (gs->inUse) {  // not cleared while the upload was in flight
                memoryFree(gs->cullInstances);
                gs->cullInstances = cullOut;
                memoryFree(gs->cullRanges);
                gs->cullRanges     = cullRO;
                gs->cullCount      = w;
                gs->cullRangeCount = rc;
                cullOut            = NULL;
                cullRO             = NULL;
            }
            threadUnlock(&g_stateLock);
        }
        memoryFree(cullOut);
        memoryFree(cullRO);
    } else {
        PropInstance* cullOut = NULL;
        PropTileRange* cullRO = NULL;
        u32 w = 0, rc = 0;
        bool valid = false;
        threadLock(&g_stateLock);
        PropsTileState* tile = NULL;
        for (u32 i = 0; i < arraySize(g_tiles); i++) {
            if (g_tiles[i].inUse && g_tiles[i].tileX == job->tileX &&
                g_tiles[i].tileZ == job->tileZ) {
                tile = &g_tiles[i];
                break;
            }
        }
        if (tile && tile->inUse && !tile->building && tile->readyStamp == job->readyStamp &&
            tile->instanceCount > 0) {
            tile->culling = false;
            cullOut        = static_cast<PropInstance*>(memoryAlloc(sizeof(PropInstance) * tile->instanceCount));
            cullRO         = static_cast<PropTileRange*>(memoryAlloc(sizeof(PropTileRange) * tile->rangeCount));
            propsCullCompact(tile->instances,
                             tile->ranges,
                             tile->rangeCount,
                             cullOut,
                             cullRO,
                             job->camPos,
                             job->planes,
                             &w,
                             &rc);
            valid = true;
            if (g_propsDebug) {
                info("azgaarProps: cull tile(%d,%d): %u -> %u instances",
                     job->tileX,
                     job->tileZ,
                     tile->instanceCount,
                     w);
            }
        } else if (tile) {
            tile->culling = false;  // tile re-scattered / evicted mid-job
        }
        threadUnlock(&g_stateLock);

        if (valid) {
            vulkanAzgaarPropsSetTile(job->tileX,
                                     job->tileZ,
                                     job->readyStamp,
                                     cullOut,
                                     w,
                                     cullRO,
                                     rc);
            threadLock(&g_stateLock);
            PropsTileState* t2 = NULL;
            for (u32 i = 0; i < arraySize(g_tiles); i++) {
                if (g_tiles[i].inUse && g_tiles[i].tileX == job->tileX &&
                    g_tiles[i].tileZ == job->tileZ && g_tiles[i].readyStamp == job->readyStamp &&
                    !g_tiles[i].building) {
                    t2 = &g_tiles[i];
                    break;
                }
            }
            if (t2) {  // tile not dropped / re-scattered while the upload ran
                memoryFree(t2->cullInstances);
                t2->cullInstances = cullOut;
                memoryFree(t2->cullRanges);
                t2->cullRanges     = cullRO;
                t2->cullCount      = w;
                t2->cullRangeCount = rc;
                memcpy(t2->cullCamPos, job->camPos, sizeof(job->camPos));
                t2->cullYaw      = job->yaw;
                t2->cullPitch    = job->pitch;
                t2->cullCamValid = true;
                cullOut          = NULL;
                cullRO           = NULL;
            }
            threadUnlock(&g_stateLock);
        }
        memoryFree(cullOut);
        memoryFree(cullRO);
    }
    if (cHitchOn)
        info("HITCH: cull job %s(%d,%d) %.1f ms (worker)",
             job->isGlobal ? (job->landmarks ? "landmarks" : "global") : "tile",
             job->tileX,
             job->tileZ,
             (nanos() - cT0) / 1e6);
    memoryFree(job);
}

// Per-frame cull pass (dirty-checked).  Evaluates each resident tile + the
// global sets against the current camera and enqueues cull jobs where the
// uploaded set no longer matches what is visible.
static void propsCullUpdate(HeightmapTerrain* ht, const HeightmapTileView* views, u32 n) {
    Entity* camEntity = cameraGetEntity();
    if (!camEntity) return;
    Camera* cam = getComponent(camEntity->scene, Camera, camEntity->id);
    if (!cam) return;
    Transform* t = getComponent(camEntity->scene, Transform, camEntity->id);
    if (!t) return;
    float camPos[3] = {t->pos[0], t->pos[1], t->pos[2]};

    // Dirty check: re-cull only when the camera moved / rotated enough.
    bool dirty = !g_cullCam.valid;
    if (!dirty) {
        float dx     = camPos[0] - g_cullCam.pos[0];
        float dy     = camPos[1] - g_cullCam.pos[1];
        float dz     = camPos[2] - g_cullCam.pos[2];
        float dyaw   = cam->yaw - g_cullCam.yaw;
        float dpitch = cam->pitch - g_cullCam.pitch;
        dirty        = (dx * dx + dy * dy + dz * dz >= PROPS_CULL_MOVE * PROPS_CULL_MOVE) ||
                       (dyaw * dyaw + dpitch * dpitch >= PROPS_CULL_ROT * PROPS_CULL_ROT);
    }

    float planes[6][4];
    for (int i = 0; i < 6; i++) {
        for (int c = 0; c < 4; c++) planes[i][c] = cam->cameraUbo.frustumPlanes[i][c];
    }

    float tileSize = ht->tileSizeMeters;
    float yMax     = (g_world && g_world->maxLandHeightM > 0.0f) ? g_world->maxLandHeightM + 64.0f
                                                                 : 64.0f;

    u32 enqueued = 0;
    threadLock(&g_stateLock);
    for (u32 k = 0; k < arraySize(g_tiles); k++) {
        PropsTileState* tile = &g_tiles[k];
        if (!tile->inUse || tile->building || tile->instanceCount == 0 || tile->culling) continue;
        if (!dirty && !tile->needsCull) continue;
        bool resident = false;
        for (u32 v = 0; v < n; v++) {
            if (views[v].tileX == tile->tileX && views[v].tileZ == tile->tileZ) {
                resident = true;
                break;
            }
        }
        if (!resident) continue;

        float half    = 64.0f + PROPS_CULL_MARGIN;
        float bmin[3] = {tile->tileX * tileSize - half, -20.0f, tile->tileZ * tileSize - half};
        float bmax[3] = {tile->tileX * tileSize + tileSize + half,
                         yMax,
                         tile->tileZ * tileSize + tileSize + half};

        // (1) Fully outside the frustum → the pass culls the whole tile;
        //     keep whatever is uploaded.
        if (propsAabbOutsideFrustum(bmin, bmax, planes)) {
            tile->needsCull = false;
            continue;
        }
        // (2) Entirely beyond the farthest species cap (the nearest point of the
        //     inflated box is already beyond it) → upload an empty set.
        float maxCap = 0.0f;
        for (u32 s = 0; s < AZGAAR_PROP_COUNT; s++) {
            if (tile->perSpecies[s] > 0 && kCullDist[s] > maxCap) maxCap = kCullDist[s];
        }
        maxCap += PROPS_CULL_MARGIN;
        if (propsAabbNearDistSq(camPos, bmin, bmax) > maxCap * maxCap) {
            if (tile->cullCount != 0) {
                tile->cullCount      = 0;
                tile->cullRangeCount = 0;
                vulkanAzgaarPropsSetTile(tile->tileX,
                                         tile->tileZ,
                                         tile->readyStamp,
                                         NULL,
                                         0,
                                         NULL,
                                         0);
            }
            tile->needsCull = false;
            continue;
        }
        // (3) Fully inside the frustum, entirely within the cap, with the full
        //     set already uploaded → nothing to do.
        if (propsAabbInsideFrustum(bmin, bmax, planes) &&
            propsAabbFarDistSq(camPos, bmin, bmax) <= maxCap * maxCap &&
            tile->cullCount == tile->instanceCount) {
            tile->needsCull = false;
            continue;
        }
        // (3b) A non-empty partial upload captured for a camera close enough
        //      to the current one (position AND rotation within the re-cull
        //      thresholds): the compact's sweep margins already cover the
        //      delta → keep it, no re-cull.
        if (tile->cullCount > 0 && tile->cullCamValid) {
            float fx   = camPos[0] - tile->cullCamPos[0];
            float fy   = camPos[1] - tile->cullCamPos[1];
            float fz   = camPos[2] - tile->cullCamPos[2];
            float dyaw = cam->yaw - tile->cullYaw;
            float dpit = cam->pitch - tile->cullPitch;
            if (fx * fx + fy * fy + fz * fz <= PROPS_CULL_MOVE * PROPS_CULL_MOVE &&
                dyaw * dyaw + dpit * dpit <= PROPS_CULL_ROT * PROPS_CULL_ROT) {
                tile->needsCull = false;
                continue;
            }
        }
        // (4) Straddling the frustum edge / a cap, or a stale partial upload →
        //     enqueue a per-instance cull.
        tile->needsCull = false;
        tile->culling   = true;
        CullJob* job = static_cast<CullJob*>(memoryAlloc(sizeof(CullJob)));
        job->isGlobal   = false;
        job->landmarks  = false;
        job->tileX      = tile->tileX;
        job->tileZ      = tile->tileZ;
        job->readyStamp = tile->readyStamp;
        memcpy(job->camPos, camPos, sizeof(camPos));
        memcpy(job->planes, planes, sizeof(planes));
        job->yaw   = cam->yaw;
        job->pitch = cam->pitch;
        threadPoolAddWork(g_pool, propsCullJob, job);
        enqueued++;
    }

    // Global sets (settlement buildings / landmarks).
    PropsGlobalSet* sets[2] = {&g_globalSet, &g_landmarkSet};
    for (u32 s = 0; s < 2; s++) {
        PropsGlobalSet* gs = sets[s];
        if (!gs->inUse || gs->instanceCount == 0 || gs->culling) continue;
        if (!dirty && !gs->needsCull) continue;
        float bmin[3] = {gs->aabbMin[0] - PROPS_CULL_MARGIN,
                         gs->aabbMin[1] - PROPS_CULL_MARGIN,
                         gs->aabbMin[2] - PROPS_CULL_MARGIN};
        float bmax[3] = {gs->aabbMax[0] + PROPS_CULL_MARGIN,
                         gs->aabbMax[1] + PROPS_CULL_MARGIN,
                         gs->aabbMax[2] + PROPS_CULL_MARGIN};
        if (propsAabbOutsideFrustum(bmin, bmax, planes)) {
            gs->needsCull = false;
            continue;
        }
        float maxCap = 0.0f;
        for (u32 r = 0; r < gs->rangeCount; r++) {
            u32 sp = gs->ranges[r].species;
            if (sp < AZGAAR_PROP_COUNT && kCullDist[sp] > maxCap) maxCap = kCullDist[sp];
        }
        maxCap += PROPS_CULL_MARGIN;
        if (propsAabbNearDistSq(camPos, bmin, bmax) > maxCap * maxCap) {
            if (gs->cullCount != 0) {
                gs->cullCount      = 0;
                gs->cullRangeCount = 0;
                if (gs == &g_landmarkSet) {
                    vulkanAzgaarPropsSetLandmarks(NULL, 0, NULL, 0, gs->aabbMin, gs->aabbMax);
                } else {
                    vulkanAzgaarPropsSetGlobal(NULL, 0, NULL, 0, gs->aabbMin, gs->aabbMax);
                }
            }
            gs->needsCull = false;
            continue;
        }
        if (propsAabbInsideFrustum(bmin, bmax, planes) &&
            propsAabbFarDistSq(camPos, bmin, bmax) <= maxCap * maxCap &&
            gs->cullCount == gs->instanceCount) {
            gs->needsCull = false;
            continue;
        }
        gs->needsCull   = false;
        gs->culling     = true;
        CullJob* job = static_cast<CullJob*>(memoryAlloc(sizeof(CullJob)));
        job->isGlobal   = true;
        job->landmarks  = (gs == &g_landmarkSet);
        job->tileX      = 0;
        job->tileZ      = 0;
        job->readyStamp = 0;
        memcpy(job->camPos, camPos, sizeof(camPos));
        memcpy(job->planes, planes, sizeof(planes));
        job->yaw   = cam->yaw;
        job->pitch = cam->pitch;
        threadPoolAddWork(g_pool, propsCullJob, job);
        enqueued++;
    }
    threadUnlock(&g_stateLock);

    if (dirty) {
        g_cullCam.pos[0] = camPos[0];
        g_cullCam.pos[1] = camPos[1];
        g_cullCam.pos[2] = camPos[2];
        memcpy(g_cullCam.planes, planes, sizeof(planes));
        g_cullCam.yaw   = cam->yaw;
        g_cullCam.pitch = cam->pitch;
        g_cullCam.valid = true;
    }
    if (g_propsDebug && enqueued > 0) {
        info("azgaarProps: cull: %u jobs enqueued (dirty=%d)", enqueued, (int)dirty);
    }
}

void azgaarPropsRegisterGlobal(const PropInstance* instances,
                               u32 instanceCount,
                               const PropTileRange* ranges,
                               u32 rangeCount,
                               const float aabbMin[3],
                               const float aabbMax[3],
                               bool landmarks) {
    PropsGlobalSet* gs = landmarks ? &g_landmarkSet : &g_globalSet;
    threadLock(&g_stateLock);
    memoryFree(gs->instances);
    memoryFree(gs->ranges);
    memoryFree(gs->cullInstances);
    memoryFree(gs->cullRanges);
    gs->instances  = static_cast<PropInstance*>((instanceCount > 0) ? memoryAlloc(sizeof(PropInstance) * instanceCount) : NULL);
    if (instanceCount > 0) memcpy(gs->instances, instances, sizeof(PropInstance) * instanceCount);
    gs->instanceCount = instanceCount;
    gs->ranges         = static_cast<PropTileRange*>((rangeCount > 0) ? memoryAlloc(sizeof(PropTileRange) * rangeCount) : NULL);
    if (rangeCount > 0) memcpy(gs->ranges, ranges, sizeof(PropTileRange) * rangeCount);
    gs->rangeCount = rangeCount;
    memcpy(gs->aabbMin, aabbMin, sizeof(gs->aabbMin));
    memcpy(gs->aabbMax, aabbMax, sizeof(gs->aabbMax));
    gs->cullInstances  = NULL;
    gs->cullRanges     = NULL;
    gs->cullCount      = instanceCount;  // the full set is uploaded below
    gs->cullRangeCount = rangeCount;
    gs->inUse          = true;
    gs->needsCull      = true;
    gs->culling        = false;
    threadUnlock(&g_stateLock);
    // Initial full-set upload (the cull pass compacts it on the next pass).
    if (landmarks) {
        vulkanAzgaarPropsSetLandmarks(instances,
                                      instanceCount,
                                      ranges,
                                      rangeCount,
                                      aabbMin,
                                      aabbMax);
    } else {
        vulkanAzgaarPropsSetGlobal(instances, instanceCount, ranges, rangeCount, aabbMin, aabbMax);
    }
}

void azgaarPropsClearGlobal(bool landmarks) {
    PropsGlobalSet* gs = landmarks ? &g_landmarkSet : &g_globalSet;
    threadLock(&g_stateLock);
    bool wasInUse = gs->inUse;
    if (wasInUse) {
        memoryFree(gs->instances);
        memoryFree(gs->ranges);
        memoryFree(gs->cullInstances);
        memoryFree(gs->cullRanges);
        *gs = (PropsGlobalSet){0};
    }
    threadUnlock(&g_stateLock);
    if (wasInUse) {
        if (landmarks)
            vulkanAzgaarPropsClearLandmarks();
        else
            vulkanAzgaarPropsClearGlobal();
    }
}

// One scatter job (runs on the pool thread).  Fills the tile's grouped
// instance buffer + ranges, then enqueues the GPU upload.  Deterministic.
typedef struct ScatterJob {
    i32 tileX, tileZ;
    u64 readyStamp;
    u32 propsVersion;    // propsVersion captured at enqueue time
    float camX, camZ;    // build-time camera (near-player guarantee + distance LOD)
    float camPos[3];     // full build-time camera position (cull capture)
    float planes[6][4];  // build-time frustum planes (cull capture)
    float yaw, pitch;    // build-time camera rotation (cull capture)
    bool havePlanes;     // false when no camera was available at enqueue
} ScatterJob;

static void propsScatterJob(void* userData) {
    ScatterJob* job    = (ScatterJob*)userData;
    static int hitchOn = -1;
    if (hitchOn < 0) hitchOn = getenv("ENGINE_HITCH_DEBUG") != NULL;
    double jobT0         = nanos();
    HeightmapTerrain* ht = heightmapTerrainGetActive();
    if (!ht || !g_world) {
        if (hitchOn)
            info("HITCH: scatter tile(%d,%d) aborted %.1f ms",
                 job->tileX,
                 job->tileZ,
                 (nanos() - jobT0) / 1e6);
        return;
    }

    // Lock-safe copy of the tile's 512^2 CPU height grid (heap: ~1 MB).
    float* heights = static_cast<float*>(memoryAlloc(sizeof(float) * (size_t)HEIGHTMAP_TEX * HEIGHTMAP_TEX));
    if (!heightmapTerrainCopyTile(ht, job->tileX, job->tileZ, heights)) {
        memoryFree(heights);
        memoryFree(job);
        if (hitchOn)
            info("HITCH: scatter tile(%d,%d) evicted-mid-job %.1f ms",
                 job->tileX,
                 job->tileZ,
                 (nanos() - jobT0) / 1e6);
        return;  // tile evicted mid-job; it will be re-scattered when it returns.
    }

    const u32 TEX        = HEIGHTMAP_TEX;
    const float tileSize = ht->tileSizeMeters;
    const float tsz      = tileSize / (float)(TEX - 1u);  // texel world size (m)
    const float originX  = (float)job->tileX * tileSize;
    const float originZ  = (float)job->tileZ * tileSize;
    const u32 tileSeed =
        g_mapSeed ^ (u32)(job->tileX * 374761393u) ^ (u32)(job->tileZ * 668265263u);
    const float seaY    = azgaarSeaLevelMeters(g_world);
    const float maxLand = g_world->maxLandHeightM;
    const float halfW   = (float)(g_world->widthPx * 0.5f) * (float)g_world->metersPerPixel;
    const float halfH   = (float)(g_world->heightPx * 0.5f) * (float)g_world->metersPerPixel;
    const float mpp     = (float)g_world->metersPerPixel;
    // Per-tile hash of instances placed so far (overlap gate, see PlacedHash).
    PlacedHash ph;
    placedHashBuild(&ph, originX, originZ, tileSize);
    // The world's biome grid (nearest-cell id field) for per-texel biome lookup.
    const u8* biomeGrid  = g_world->biomeGrid;
    const u32 bw         = g_world->climateGridWidth;
    const u32 bh         = g_world->climateGridHeight;
    const float invMppPx = 1.0f / mpp;  // world m -> map px (via azgaarMapToWorld inverse)

    // Unsorted temp scatter, then group by species into the tile's buffer.
    PropInstance* temp = static_cast<PropInstance*>(memoryAlloc(sizeof(PropInstance) * AZGAAR_PROPS_TILE_CAP));
    u32 tempCount                     = 0;
    u32 perSpecies[AZGAAR_PROP_COUNT] = {};

    for (u32 tz = 0; tz < TEX; tz++) {
        const float* row  = heights + (size_t)tz * TEX;
        const u32 tzN     = (tz + 1u < TEX) ? tz + 1u : TEX - 1u;
        const u32 tzS     = (tz > 0u) ? tz - 1u : 0u;
        const float* rowN = heights + (size_t)tzN * TEX;
        const float* rowS = heights + (size_t)tzS * TEX;
        for (u32 tx = 0; tx < TEX; tx++) {
            float wx = originX + (float)tx * tsz;
            float wz = originZ + (float)tz * tsz;

            float h = row[tx];
            if (h < seaY + 0.5f) continue;  // water / below the grass line

            // Slope (finite difference over the grid; clamped at the borders).
            const u32 txL = (tx > 0u) ? tx - 1u : 0u;
            const u32 txR = (tx + 1u < TEX) ? tx + 1u : TEX - 1u;
            float dhdx    = (row[txR] - row[txL]) * (0.5f / tsz);
            float dhdz    = (rowN[tx] - rowS[tx]) * (0.5f / tsz);
            float slope   = sqrtf(dhdx * dhdx + dhdz * dhdz);

            // Nearest-cell biome (from the world's biome grid, in map px).
            u32 biome = AZGAAR_BIOME_NONE;
            if (biomeGrid && bw > 1 && bh > 1) {
                // world -> map px (inverse of azgaarMapToWorld).
                float mapX = (halfW - wx) * invMppPx;
                float mapY = (halfH - wz) * invMppPx;
                u32 gx     = (u32)(mapX * ((float)bw / (float)g_world->widthPx) + 0.5f);
                u32 gy     = (u32)(mapY * ((float)bh / (float)g_world->heightPx) + 0.5f);
                if (gx >= bw) gx = bw - 1u;
                if (gy >= bh) gy = bh - 1u;
                biome = (u32)biomeGrid[(size_t)gy * bw + gx];
            }

            // Distance from the build-time camera (near-player guarantee).
            float dxc  = wx - job->camX;
            float dzc  = wz - job->camZ;
            float dist = sqrtf(dxc * dxc + dzc * dzc);

            // Clumping gate (world-anchored fBm) + near-player guarantee.
            float clump     = propsClumpNoise(wx, wz);
            float clumpKeep = clump > 0.55f ? 1.0f : 0.0f;
            float near      = 1.0f - smoothstep01(250.0f, 600.0f, dist);
            float keep      = clumpKeep > near ? clumpKeep : near;

            // Sacred-forest landmark discs (workstream E): force keep + 3x
            // density inside the 300 m radius.  Read-only query (the module
            // publishes its disc count last), so it is pool-thread safe.
            float forestBoost = azgaarLandmarksForestBoost(wx, wz);
            if (forestBoost > 1.5f) keep = 1.0f;

            // ── Grass undergrowth (runs before the main pass so its `continue` can't skip it) ──
            // Independent density roll, 2 m offset to avoid the tree at the
            // texel centre.  No overlap gate: grass-vs-grass overlap is
            // visually harmless and the offset avoids the tree.
            if (biome < g_world->biomeCount && biome < AZGAAR_PROPS_MAX_BIOMES &&
                g_biomeSpecies[biome].count > 0 && g_variantCount[AZGAAR_PROP_GRASS_TUFT] > 0 &&
                slope <= kSpecies[AZGAAR_PROP_GRASS_TUFT].slopeMax &&
                !roadNear(&g_roadHash, wx, wz, 20.0f)) {
                float underD = 0.03f * (tsz * tsz);
                if (propsRand(tileSeed, tx, tz, 0xD1) < underD) {
                    const PropSpeciesDef* gdef = &kSpecies[AZGAAR_PROP_GRASS_TUFT];
                    float baseAng = propsRand(tileSeed, tx, tz, 0xD6) * 2.0f * (float)M_PI;
                    float bc[3];
                    azgaarWorldBiomeColor(g_world, biome, bc);
                    for (u32 gi = 0; gi < 2; gi++) {
                        float ang    = baseAng + (float)gi * (float)M_PI;
                        float gwx    = wx + cosf(ang) * 2.0f;
                        float gwz    = wz + sinf(ang) * 2.0f;
                        float gScale = gdef->baseMin + (gdef->baseMax - gdef->baseMin) *
                                                           propsRand(tileSeed, tx, tz, 0xD2 + gi);
                        float j      = 0.6f + 0.8f * propsRand(tileSeed, tx, tz, 0xD3 + gi);
                        PropInstance inst = {};
                        inst.pos[0]       = gwx;
                        inst.pos[1]       = h;
                        inst.pos[2]       = gwz;
                        inst.yaw      = propsRand(tileSeed, tx, tz, 0xD4 + gi) * 2.0f * (float)M_PI;
                        inst.scale    = gScale;
                        inst.color[0] = bc[0] * j;
                        inst.color[1] = bc[1] * j;
                        inst.color[2] = bc[2] * j;
                        inst.phase    = propsRand(tileSeed, tx, tz, 0xD5 + gi) * 2.0f * (float)M_PI;
                        inst.species  = AZGAAR_PROP_GRASS_TUFT;
                        inst.variant  = 0;
                        if (tempCount < AZGAAR_PROPS_TILE_CAP) {
                            temp[tempCount++] = inst;
                            perSpecies[AZGAAR_PROP_GRASS_TUFT]++;
                        }
                    }
                }
            }

            // ── Vegetation ──
            if (keep > 0.0f && biome < g_world->biomeCount && biome < AZGAAR_PROPS_MAX_BIOMES &&
                g_biomeSpecies[biome].count > 0) {
                BiomeSpecies* bs = &g_biomeSpecies[biome];
                // Weighted species pick (icon repetition = weight).
                u32 roll = (u32)(propsRand(tileSeed, tx, tz, 0xAB) * (float)bs->totalWeight);
                u32 acc = 0, chosen = bs->species[0];
                for (u32 s = 0; s < bs->count; s++) {
                    acc += bs->weight[s];
                    if (roll < acc) {
                        chosen = bs->species[s];
                        break;
                    }
                    chosen = bs->species[s];
                }
                // Near LOD instance (the far LOD is emitted as a separate
                // instance below; the GPU cross-fades between them by distance).
                u32 sp = chosen;
                if (g_variantCount[sp] == 0)
                    continue;  // awaiting its authored model: no placeholder instances
                const PropSpeciesDef* def = &kSpecies[sp];
                if (slope > def->slopeMax) continue;                 // too steep for this species
                if (roadNear(&g_roadHash, wx, wz, 20.0f)) continue;  // road clearing
                float density =
                    kSpeciesDensity[sp] * ((float)g_world->biomes[biome].iconsDensity / 120.0f);
                if (density <= 0.0f) continue;
                float expected = density * keep * forestBoost * (tsz * tsz);
                if (expected <= 0.0f) continue;
                if (propsRand(tileSeed, tx, tz, 0xCD) >= expected) continue;
                float instScale = def->baseMin +
                                  (def->baseMax - def->baseMin) * propsRand(tileSeed, tx, tz, 0xE2);
                if (placedTooClose(&ph, wx, wz, sp, instScale)) continue;  // overlap gate

                PropInstance inst = {};
                inst.pos[0]       = wx;
                inst.pos[1]       = h;
                inst.pos[2]       = wz;
                inst.yaw          = propsRand(tileSeed, tx, tz, 0xE1) * 2.0f * (float)M_PI;
                inst.scale        = instScale;
                // Colour: biome tint x jitter (wider range so trees vary).
                float bc[3];
                azgaarWorldBiomeColor(g_world, biome, bc);
                float j       = 0.6f + 0.8f * propsRand(tileSeed, tx, tz, 0xE3);
                inst.color[0] = bc[0] * j;
                inst.color[1] = bc[1] * j;
                inst.color[2] = bc[2] * j;
                inst.phase    = propsRand(tileSeed, tx, tz, 0xE4) * 2.0f * (float)M_PI;
                inst.species  = sp;
                // Deterministic per-instance variant pick (pure function of the tile
                // seed + texel, so eviction + regeneration is bit-identical).
                u32 vc       = g_variantCount[sp];
                inst.variant = (vc > 1) ? (u32)(propsRand(tileSeed, tx, tz, 0xE5) * (float)vc) : 0;
                if (tempCount < AZGAAR_PROPS_TILE_CAP) {
                    temp[tempCount++] = inst;
                    if (sp < AZGAAR_PROP_COUNT) perSpecies[sp]++;
                    placedHashInsert(&ph, wx, wz, sp, instScale);
                    // Far LOD: a second instance with the simplified far mesh,
                    // same transform.  The GPU cross-fades near/far by live
                    // camera distance (see azgaar_props.vert).
                    u32 farSp = kSpecies[chosen].farVariant;
                    if (farSp != sp && g_variantCount[farSp] > 0 &&
                        tempCount < AZGAAR_PROPS_TILE_CAP) {
                        PropInstance finst = inst;
                        finst.species      = farSp;
                        u32 fvc            = g_variantCount[farSp];
                        finst.variant     = (fvc > 1) ? (inst.variant < fvc ? inst.variant : 0) : 0;
                        temp[tempCount++] = finst;
                        if (farSp < AZGAAR_PROP_COUNT) perSpecies[farSp]++;
                    }
                }
            }

            // ── Rocks (non-biome, on steep / high ground) ──
            if ((slope > 0.35f || h > 0.5f * maxLand) && !roadNear(&g_roadHash, wx, wz, 12.0f)) {
                float rockD = 0.001f * (tsz * tsz);
                if (propsRand(tileSeed, tx, tz, 0xF1) < rockD) {
                    float rockScale = 0.5f + 2.5f * propsRand(tileSeed, tx, tz, 0xF3);
                    if (placedTooClose(&ph, wx, wz, AZGAAR_PROP_ROCK, rockScale))
                        continue;  // overlap gate
                    PropInstance inst = {};
                    inst.pos[0]       = wx;
                    inst.pos[1]       = h;
                    inst.pos[2]       = wz;
                    inst.yaw          = propsRand(tileSeed, tx, tz, 0xF2) * 2.0f * (float)M_PI;
                    inst.scale        = rockScale;
                    inst.color[0]     = 0.45f;
                    inst.color[1]     = 0.43f;
                    inst.color[2]     = 0.40f;
                    inst.phase        = 0.0f;
                    inst.species      = AZGAAR_PROP_ROCK;
                    if (tempCount < AZGAAR_PROPS_TILE_CAP) {
                        temp[tempCount++] = inst;
                        perSpecies[AZGAAR_PROP_ROCK]++;
                        placedHashInsert(&ph, wx, wz, AZGAAR_PROP_ROCK, rockScale);
                    }
                }
            }
        }
    }

    // ── Phase A (lock): resolve/claim this tile's state entry ──
    // `building` stays true until the GPU hand-over (Phase D) so the entry
    // cannot be evicted or culled while the upload is in flight.
    threadLock(&g_stateLock);
    {
        PropsTileState* tile = NULL;
        for (u32 i = 0; i < arraySize(g_tiles); i++) {
            if (g_tiles[i].inUse && g_tiles[i].tileX == job->tileX &&
                g_tiles[i].tileZ == job->tileZ) {
                tile = &g_tiles[i];
                break;
            }
        }
        if (tile && tile->readyStamp != job->readyStamp) {
            // Tile was regenerated while the job ran; drop the stale entry so it
            // is re-scattered (the update loop will re-enqueue it).
            propsTileDrop(tile);
            tile = NULL;
        }
        if (!tile) {
            arrayPut(g_tiles, (PropsTileState){0});
            tile = &g_tiles[arraySize(g_tiles) - 1u];
        }
        tile->tileX        = job->tileX;
        tile->tileZ        = job->tileZ;
        tile->readyStamp   = job->readyStamp;
        tile->propsVersion = job->propsVersion;
        tile->camX         = job->camX;
        tile->camZ         = job->camZ;
        tile->inUse        = true;
        tile->building     = true;  // released in Phase D after the hand-over
    }
    threadUnlock(&g_stateLock);

    // ── Phase B (no lock): group the unsorted instances by (species, variant)
    // ── into job-owned buffers; the GPU upload (Phase C) must not hold
    // ── g_stateLock (it fence-waits on the worker), and it must not read
    // ── tile-owned memory (eviction could free it mid-upload).
    u32 n = tempCount;
    if (tempCount >= AZGAAR_PROPS_TILE_CAP) {
        warn("azgaarProps: tile(%d,%d) hit the %u instance cap (truncated)",
             job->tileX,
             job->tileZ,
             AZGAAR_PROPS_TILE_CAP);
    }
    // Group by (species, variant).  Only deciduous / deciduous_far carry more
    // than one variant; every other species uses variant 0.
    u32 totalV = 0;
    for (u32 s = 0; s < AZGAAR_PROP_COUNT; s++) totalV += g_variantCount[s];
    u32* counts = static_cast<u32*>(memoryAlloc(sizeof(u32) * totalV));
    memset(counts, 0, sizeof(u32) * totalV);
    u32 base[AZGAAR_PROP_COUNT];
    {
        u32 a = 0;
        for (u32 s = 0; s < AZGAAR_PROP_COUNT; s++) {
            base[s] = a;
            a += g_variantCount[s];
        }
    }
    for (u32 i = 0; i < n; i++) {
        u32 sp = temp[i].species;
        u32 vv = temp[i].variant;
        if (sp < AZGAAR_PROP_COUNT) {
            counts[base[sp] + vv]++;
            perSpecies[sp]++;
        }
    }

    // Build the list of (species, variant) pairs with count > 0 + prefix offsets.
    typedef struct SVPair {
        u32 species;
        u32 variant;
        u32 count;
        u32 offset;
    } SVPair;

    SVPair* pairs = static_cast<SVPair*>(memoryAlloc(sizeof(SVPair) * totalV));
    u32 pairCount = 0;
    u32 acc       = 0;
    for (u32 s = 0; s < AZGAAR_PROP_COUNT; s++) {
        for (u32 v = 0; v < g_variantCount[s]; v++) {
            u32 c = counts[base[s] + v];
            if (c > 0) {
                pairs[pairCount].species = s;
                pairs[pairCount].variant = v;
                pairs[pairCount].count   = c;
                pairs[pairCount].offset  = acc;
                acc += c;
                pairCount++;
            }
        }
    }

    // Job-owned grouped buffers: handed to the tile in Phase D, freed here if
    // the tile was evicted / invalidated in the meantime.
    PropInstance* grouped     = static_cast<PropInstance*>((n > 0) ? memoryAlloc(sizeof(PropInstance) * n) : NULL);
PropTileRange* rangesArr  = (pairCount > 0) ? static_cast<PropTileRange*>(memoryAlloc(sizeof(PropTileRange) * pairCount)) : NULL;

    if (n > 0) {
        // Place each instance into its (species, variant) group.
        u32* cursor = static_cast<u32*>(memoryAlloc(sizeof(u32) * totalV));
        u32* flatOffset = static_cast<u32*>(memoryAlloc(sizeof(u32) * totalV));
        memset(cursor, 0, sizeof(u32) * totalV);
        memset(flatOffset, 0, sizeof(u32) * totalV);
        for (u32 p = 0; p < pairCount; p++) {
            flatOffset[base[pairs[p].species] + pairs[p].variant] = pairs[p].offset;
        }
        for (u32 i = 0; i < n; i++) {
            u32 sp       = temp[i].species;
            u32 vv       = temp[i].variant;
            u32 flat     = base[sp] + vv;
            u32 dst      = flatOffset[flat] + cursor[flat]++;
            grouped[dst] = temp[i];
        }
        memoryFree(cursor);
        memoryFree(flatOffset);

        for (u32 p = 0; p < pairCount; p++) {
            rangesArr[p] = (PropTileRange){
                .species = pairs[p].species,
                .variant = pairs[p].variant,
                .start   = pairs[p].offset,
                .count   = pairs[p].count,
            };
        }
    }
    memoryFree(counts);
    memoryFree(pairs);

    // ── Phase C (no lock): compact the grouped set against the enqueue-time
    // ── camera and upload ONLY the visible subset (non-blocking; the pass
    // ── adopts it in preUpdate once the copy's fence signals).  The full
    // ── grouped set stays the CPU source of truth for later re-culls.
    // ── Without a captured camera we fall back to uploading the full set
    // ── and letting the cull pass compact it on its first pass.
    PropInstance* cullOut = NULL;
    PropTileRange* cullRO = NULL;
    u32 cullW = 0, cullRC = 0;
    if (job->havePlanes && n > 0) {
        cullOut  = static_cast<PropInstance*>(memoryAlloc(sizeof(PropInstance) * n));
        cullRO   = static_cast<PropTileRange*>(memoryAlloc(sizeof(PropTileRange) * (pairCount > 0 ? pairCount : 1)));
        propsCullCompact(grouped,
                         rangesArr,
                         pairCount,
                         cullOut,
                         cullRO,
                         job->camPos,
                         job->planes,
                         &cullW,
                         &cullRC);
        vulkanAzgaarPropsSetTile(job->tileX,
                                 job->tileZ,
                                 job->readyStamp,
                                 cullOut,
                                 cullW,
                                 cullRO,
                                 cullRC);
    } else {
        vulkanAzgaarPropsSetTile(job->tileX,
                                 job->tileZ,
                                 job->readyStamp,
                                 grouped,
                                 n,
                                 rangesArr,
                                 pairCount);
    }

    // ── Phase D (lock): hand the grouped buffers over to the tile, or free
    // ── them if the tile was evicted / invalidated while the upload ran.
    threadLock(&g_stateLock);
    PropsTileState* tile = NULL;
    for (u32 i = 0; i < arraySize(g_tiles); i++) {
        if (g_tiles[i].inUse && g_tiles[i].tileX == job->tileX && g_tiles[i].tileZ == job->tileZ &&
            g_tiles[i].readyStamp == job->readyStamp && g_tiles[i].building) {
            tile = &g_tiles[i];
            break;
        }
    }
    if (tile) {
        memoryFree(tile->instances);
        tile->instances = grouped;
        grouped         = NULL;
        memoryFree(tile->ranges);
        tile->ranges        = rangesArr;
        rangesArr           = NULL;
        tile->rangeCount    = pairCount;
        tile->instanceCount = n;
        for (u32 s = 0; s < AZGAAR_PROP_COUNT; s++) {
            tile->perSpecies[s] = perSpecies[s];
        }
        if (job->havePlanes && cullOut) {
            // A compacted set is already uploaded: no first cull needed, and
            // the upload is fresh for the capture camera (see cullCamValid).
            memoryFree(tile->cullInstances);
            memoryFree(tile->cullRanges);
            tile->cullInstances  = cullOut;
            cullOut              = NULL;
            tile->cullRanges     = cullRO;
            cullRO               = NULL;
            tile->cullCount      = cullW;
            tile->cullRangeCount = cullRC;
            memcpy(tile->cullCamPos, job->camPos, sizeof(tile->cullCamPos));
            tile->cullYaw      = job->yaw;
            tile->cullPitch    = job->pitch;
            tile->cullCamValid = true;
            tile->needsCull    = false;
        } else {
            // Fallback: the full set is the uploaded set; mark the tile for
            // its first per-instance cull (the cull pass compacts it).
            memoryFree(tile->cullInstances);
            memoryFree(tile->cullRanges);
            tile->cullInstances  = NULL;
            tile->cullRanges     = NULL;
            tile->cullCount      = tile->instanceCount;
            tile->cullRangeCount = tile->rangeCount;
            tile->needsCull      = true;
            tile->cullCamValid   = false;
        }
        tile->building = false;
        if (g_propsDebug) {
            u32 total = tile->instanceCount;
            info("azgaarProps: tile(%d,%d) built %u instances in %.1f KB (species: %u/%u/%u...)",
                 job->tileX,
                 job->tileZ,
                 total,
                 (float)(total * sizeof(PropInstance)) / 1024.0f,
                 tile->perSpecies[AZGAAR_PROP_GRASS_TUFT],
                 tile->perSpecies[AZGAAR_PROP_CONIFER],
                 tile->perSpecies[AZGAAR_PROP_DECIDUOUS]);
            if (job->havePlanes && cullW != total) {
                info("azgaarProps: tile(%d,%d) scatter-cull %u -> %u instances",
                     job->tileX,
                     job->tileZ,
                     total,
                     cullW);
            }
        }
    }
    threadUnlock(&g_stateLock);

    memoryFree(grouped);
    memoryFree(rangesArr);
    memoryFree(cullOut);
    memoryFree(cullRO);
    placedHashFree(&ph);
    memoryFree(temp);
    memoryFree(heights);
    if (hitchOn)
        info("HITCH: scatter tile(%d,%d) %u insts total %.1f ms (worker)",
             job->tileX,
             job->tileZ,
             tempCount,
             (nanos() - jobT0) / 1e6);
    memoryFree(job);
}

// ── Per-frame poll: enqueue READY tiles not yet built, push wind ─────────

static void propsPushWind(const AzgaarWorld* world) {
    // v1 uses winds[0] (plan F: seasons later).  Falls back to a default
    // direction when the map has no authored wind.  When the weather module
    // is live, the direction and a strength modulation come from its shared
    // gust (plans/azgaar-weather-gpu-particles.md D8) so grass sway, water
    // ripples and particle drift stay coherent.
    float deg      = world && world->winds[0] != 0.0f ? world->winds[0] : 45.0f;
    float rad      = deg * (float)M_PI / 180.0f;
    float strength = 0.15f;
    float speed    = 1.4f;  // sway angular speed (rad/s)
    float gustDirX, gustDirZ, gustSpeed;
    if (azgaarWeatherGetWind(&gustDirX, &gustDirZ, &gustSpeed)) {
        rad      = atan2f(gustDirZ, gustDirX);
        strength = 0.12f + 0.06f * gustSpeed;  // 2.6..4.4 m/s → 0.17..0.38
    }
    // Test overrides (TAA debugging): freeze the sway with strength 0.
    const char* sEnv = getenv("ENGINE_PROPS_WIND_STRENGTH");
    if (sEnv && *sEnv) {
        strength = (float)atof(sEnv);
    }
    const char* pEnv = getenv("ENGINE_PROPS_WIND_SPEED");
    if (pEnv && *pEnv) {
        speed = (float)atof(pEnv);
    }
    float enabled              = (!g_propsDisabled) ? 1.0f : 0.0f;
    VulkanAzgaarPropsData data = {
        .wind    = {cosf(rad), sinf(rad), speed, strength},
        .density = {1.0f, 1.0f, 1.0f, enabled},
        .lod     = {PROPS_LOD_SWITCH, PROPS_LOD_SWITCH, 0.0f, 0.0f},
    };
    vulkanResourceSetAzgaarPropsData(&data);
}

// ── Lifecycle ─────────────────────────────────────────────────────────────

void azgaarPropsInit(const AzgaarWorld* world) {
    if (!world) return;
    azgaarPropsDestroy();  // idempotent

    g_world         = world;
    g_propsDebug    = getenv("ENGINE_AZGAAR_PROPS_DEBUG") != NULL;
    g_propsDisabled = getenv("ENGINE_AZGAAR_PROPS_DISABLED") != NULL;

    // Map seed for deterministic scatter (FNV-1a of the map name, same as the
    // heightmap detail seed).
    u32 h = 2166136261u;
    for (const char* p = world->mapName; *p; p++) {
        h ^= (u32)(unsigned char)*p;
        h *= 16777619u;
    }
    g_mapSeed = h ? h : 1u;

    precomputeBiomeSpecies(world, g_biomeSpecies, AZGAAR_PROPS_MAX_BIOMES);
    roadHashBuild(&g_roadHash, world, 20.0f);

    // Recursively locate + load the authored deciduous model (falls back to
    // the procedural mesh if the .dat is not present in any pak).
    azgaarLoadDeciduousModel();

    // Build the merged species-mesh buffer + variant table, push to the engine pass.
    u32 vCount = 0, iCount = 0, variantCount = 0;
    void* verts                = NULL;
    void* idx                  = NULL;
    PropVariantRange* variants = NULL;
    buildAllMeshes(&variants, &variantCount, &vCount, &iCount, &verts, &idx);
    vulkanAzgaarPropsSetMeshes(verts, vCount, idx, iCount);
    vulkanAzgaarPropsSetVariants(variants, variantCount);
    memoryFree(verts);
    memoryFree(idx);
    memoryFree(variants);

    // Reuse the pool from a previous world: azgaarPropsDestroy() must not
    // destroy it (thpool's keepalive flag is process-wide — destroying any
    // pool kills the workers of every pool, see azgaarPropsDestroy).
    if (!g_pool) g_pool = threadPoolInit(3);
    g_initialized = true;
    info("azgaarProps: variant table built: %u rows, %u deciduous variants",
         variantCount,
         g_variantCount[AZGAAR_PROP_DECIDUOUS]);
    if (g_propsDebug) {
        info("azgaarProps: %u species, %u verts / %u idx; road hash %u pts; seed=0x%08x%s",
             AZGAAR_PROP_COUNT,
             vCount,
             iCount,
             g_roadHash.pointCount,
             g_mapSeed,
             g_propsDisabled ? " (DISABLED)" : "");
    }
}

void azgaarPropsUpdate(void) {
    if (!g_initialized || !g_world) return;
    static int hitchOn = -1;
    if (hitchOn < 0) hitchOn = getenv("ENGINE_HITCH_DEBUG") != NULL;
    u32 enqNew = 0, enqVer = 0, enqLod = 0;
    propsPushWind(g_world);

    HeightmapTerrain* ht = heightmapTerrainGetActive();
    if (!ht || !ht->initialized) return;

    u32 cap                  = ht->windowSize * ht->windowSize + 4u;
    HeightmapTileView* views = static_cast<HeightmapTileView*>(memoryAlloc(sizeof(HeightmapTileView) * cap));
    u32 n                    = heightmapTerrainSnapshotTiles(ht, views, cap);

    // If the authored deciduous model file exists but its scene is not ready
    // yet, defer the first scatter until the model is loaded, so the real
    // trees are present from the first frame (prevents the placeholder→model
    // pop).  Other vegetation (grass / rocks) is held for this frame too —
    // acceptable at world start, behind the loading screen.
    bool modelPending = false;
    if (g_deciduousPath && (!g_deciduousScene || !g_deciduousScene->ready)) modelPending = true;
    if (g_deciduousFarPath && (!g_deciduousFarScene || !g_deciduousFarScene->ready))
        modelPending = true;
    if (modelPending) {
        memoryFree(views);
        return;
    }

    // Get the build-time camera position (for the near-player guarantee) plus the
    // frustum / rotation captured for the scatter-time cull.
    float camX = 0.0f, camZ = 0.0f;
    float camPos[3]    = {0.0f, 0.0f, 0.0f};
    float planes[6][4] = {};
    float camYaw = 0.0f, camPitch = 0.0f;
    bool havePlanes   = false;
    Entity* camEntity = cameraGetEntity();
    if (camEntity) {
        Transform* t = getComponent(camEntity->scene, Transform, camEntity->id);
        if (t) {
            camX = t->pos[0];
            camZ = t->pos[2];
            memcpy(camPos, t->pos, sizeof(camPos));
        }
        Camera* cam = getComponent(camEntity->scene, Camera, camEntity->id);
        if (cam) {
            for (int i = 0; i < 6; i++) {
                for (int c = 0; c < 4; c++) planes[i][c] = cam->cameraUbo.frustumPlanes[i][c];
            }
            camYaw     = cam->yaw;
            camPitch   = cam->pitch;
            havePlanes = true;
        }
    }

    for (u32 i = 0; i < n; i++) {
        HeightmapTileView* v = &views[i];
        if (!g_propsDisabled) {
            // Enqueue scatter for READY tiles not yet built / building for this stamp.
            PropsTileState* tile = NULL;
            threadLock(&g_stateLock);
            for (u32 k = 0; k < arraySize(g_tiles); k++) {
                if (g_tiles[k].inUse && g_tiles[k].tileX == v->tileX &&
                    g_tiles[k].tileZ == v->tileZ) {
                    tile = &g_tiles[k];
                    break;
                }
            }
            // Re-scatter when the camera has moved far enough that this tile's
            // baked near/far LOD could have changed.  Only tiles within the LOD
            // range of either the current or the build-time camera are affected
            // (far tiles are always far-LOD and need no update).
            bool camStale = false;
            if (tile && tile->inUse && !tile->building) {
                float dx = camX - tile->camX;
                float dz = camZ - tile->camZ;
                if (dx * dx + dz * dz > PROPS_LOD_DIST * PROPS_LOD_DIST) {
                    float tsz     = ht->tileSizeMeters;
                    float nearNow = propsDistCamToTile(camX, camZ, v->originX, v->originZ, tsz);
                    float nearThen =
                        propsDistCamToTile(tile->camX, tile->camZ, v->originX, v->originZ, tsz);
                    camStale = (nearNow < PROPS_LOD_DIST) || (nearThen < PROPS_LOD_DIST);
                }
            }
            bool needsBuild = (!tile || tile->readyStamp != v->readyStamp ||
                               tile->propsVersion != g_propsVersion || !tile->inUse || camStale);
            u32 reason      = 0;  // 1 = new stamp, 2 = variant-table rebuild, 3 = LOD cam stale
            if (needsBuild) {
                if (!tile || !tile->inUse || tile->readyStamp != v->readyStamp)
                    reason = 1;
                else if (tile->propsVersion != g_propsVersion)
                    reason = 2;
                else
                    reason = 3;
            }
            threadUnlock(&g_stateLock);
            if (needsBuild) {
                threadLock(&g_stateLock);
                // Re-check + claim the tile (avoid double-claim by the poll).
                bool claimed = false;
                for (u32 k = 0; k < arraySize(g_tiles); k++) {
                    if (g_tiles[k].inUse && g_tiles[k].tileX == v->tileX &&
                        g_tiles[k].tileZ == v->tileZ && g_tiles[k].readyStamp == v->readyStamp &&
                        !g_tiles[k].building) {
                        g_tiles[k].building = true;
                        claimed             = true;
                        break;
                    }
                }
                if (!claimed) {
                    PropsTileState ns = {.tileX        = v->tileX,
                                         .tileZ        = v->tileZ,
                                         .readyStamp   = v->readyStamp,
                                         .propsVersion = g_propsVersion,
                                         .inUse        = true,
                                         .building     = true};
                    arrayPut(g_tiles, ns);
                    claimed = true;
                }
                threadUnlock(&g_stateLock);
                if (claimed && g_pool) {
                    ScatterJob* job = static_cast<ScatterJob*>(memoryAlloc(sizeof(ScatterJob)));
                    job->tileX        = v->tileX;
                    job->tileZ        = v->tileZ;
                    job->readyStamp   = v->readyStamp;
                    job->propsVersion = g_propsVersion;
                    job->camX         = camX;
                    job->camZ         = camZ;
                    memcpy(job->camPos, camPos, sizeof(camPos));
                    memcpy(job->planes, planes, sizeof(planes));
                    job->yaw        = camYaw;
                    job->pitch      = camPitch;
                    job->havePlanes = havePlanes;
                    threadPoolAddWork(g_pool, propsScatterJob, job);
                    if (reason == 1)
                        enqNew++;
                    else if (reason == 2)
                        enqVer++;
                    else
                        enqLod++;
                }
            }
        }
    }

    if (hitchOn && (enqNew + enqVer + enqLod) > 0) {
        info("HITCH: scatter enqueue %u jobs (new=%u lod=%u ver=%u)",
             (unsigned)(enqNew + enqVer + enqLod),
             (unsigned)enqNew,
             (unsigned)enqLod,
             (unsigned)enqVer);
    }

    // Drop CPU state for tiles that left the window (their GPU buffer is
    // cleared by the engine pass' ClearAll on world teardown; here we just
    // free the CPU arrays so a 25-tile window doesn't retain evicted data).
    threadLock(&g_stateLock);
    for (i32 i = (i32)arraySize(g_tiles) - 1; i >= 0; i--) {
        PropsTileState* t = &g_tiles[i];
        if (!t->inUse) continue;
        bool resident = false;
        for (u32 k = 0; k < n; k++) {
            if (views[k].tileX == t->tileX && views[k].tileZ == t->tileZ) {
                resident = true;
                break;
            }
        }
        if (!resident && !t->building && !t->culling) {
            i32 dropX = t->tileX, dropZ = t->tileZ;
            propsTileDrop(t);
            arrayDeleteSlow(g_tiles, (u32)i);
            // Free the engine pass' GPU instance buffer for the evicted tile so
            // flying across many tiles doesn't accumulate GPU memory.
            vulkanAzgaarPropsClearTile(dropX, dropZ);
        }
    }
    threadUnlock(&g_stateLock);

    // Per-instance culling (dirty-checked): compact the full tile / global
    // sets to what the current camera can see and re-upload.
    if (!g_propsDisabled) {
        propsCullUpdate(ht, views, n);
    }

    memoryFree(views);
}

void azgaarPropsDestroy(void) {
    // Wait for in-flight scatter jobs (they touch g_tiles + the pass), then
    // free everything and clear the engine pass.
    //
    // NOTE: do NOT threadPoolDestroy(g_pool) here.  thpool's keepalive flag
    // is a process-wide global: thpool_destroy() sets it to 0 and every pool
    // worker in the process (default + transform pools included) exits its
    // loop forever.  On the next world entry the scene-parse jobs queued on
    // the default pool would never run and the loading screen would stall.
    // Keep the (3) idle threads alive for the process lifetime instead.
    if (g_pool) {
        threadPoolWait(g_pool);
    }
    threadLock(&g_stateLock);
    for (u32 i = 0; i < arraySize(g_tiles); i++) {
        if (g_tiles[i].inUse) propsTileDrop(&g_tiles[i]);
    }
    arrayFree(g_tiles);
    threadUnlock(&g_stateLock);

    // Drop the global sets (the owning modules may already have cleared them;
    // inUse makes this idempotent).
    azgaarPropsClearGlobal(false);
    azgaarPropsClearGlobal(true);

    memoryFree(g_roadHash.pts);
    memoryFree(g_roadHash.starts);
    g_roadHash = (RoadHash){0};

    vulkanAzgaarPropsClearAll();
    vulkanAzgaarPropsSetMeshes(NULL, 0, NULL, 0);
    vulkanAzgaarPropsSetVariants(NULL, 0);
    VulkanAzgaarPropsData off = {.wind    = {0, 0, 0, 0},
                                 .density = {0, 0, 0, 0},
                                 .lod     = {PROPS_LOD_SWITCH, PROPS_LOD_SWITCH, 0.0f, 0.0f}};
    vulkanResourceSetAzgaarPropsData(&off);

    if (g_deciduousScene) {
        rendererSceneDestroy(g_deciduousScene);
        sceneDestroy(g_deciduousScene);
        g_deciduousScene = NULL;
    }
    if (g_deciduousPath) {
        memoryFree(g_deciduousPath);
        g_deciduousPath = NULL;
    }
    if (g_deciduousFarScene) {
        rendererSceneDestroy(g_deciduousFarScene);
        sceneDestroy(g_deciduousFarScene);
        g_deciduousFarScene = NULL;
    }
    if (g_deciduousFarPath) {
        memoryFree(g_deciduousFarPath);
        g_deciduousFarPath = NULL;
    }
    memoryFree(g_variantSphereC);
    memoryFree(g_variantSphereR);
    g_variantSphereC    = NULL;
    g_variantSphereR    = NULL;
    g_variantSphereRows = 0;

    g_world       = NULL;
    g_initialized = false;
}