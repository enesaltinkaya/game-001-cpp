#include "azgaar/AzgaarLandmarks.h"
#include "azgaar/AzgaarProps.h"
#include "azgaar/AzgaarRivers.h"
#include "azgaar/AzgaarWorld.h"
#include "renderer/decal/Decal.h"
#include "renderer/vulkan/pass/azgaar_props/VulkanAzgaarPropsPass.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * AzgaarLandmarks (workstream E, plans/azgaar-world-population.md)
 * -----------------------------------------------------------------
 * One pass over world->markers at load: each modelled kind becomes
 * deterministic props (seeded by map name + marker id) uploaded to the
 * azgaar_props pass' landmark slot; hot-springs/water-sources become pool
 * decals; sacred-forests publish 300 m density discs for the scatter's
 * azgaarLandmarksForestBoost query.
 *
 * Kill switch: ENGINE_AZGAAR_LANDMARKS_DISABLED=1.
 */

// ── Deterministic RNG (seeded by map name + marker id) ────────────────────

static u32 landHash3(u32 a, u32 b, u32 c) {
    u32 h = a * 0x8da6b343u ^ b * 0xc2b2ae35u ^ c * 0x27d4eb2fu;
    h = (h ^ (h >> 15)) * 0x2c1b3c6du;
    h = (h ^ (h >> 12)) * 0x297a2d39u;
    return h ^ (h >> 15);
}

static u32 landMapSeed(const char* name) {
    u32 h = 2166136261u; // FNV-1a (same seed as the props scatter / settlements)
    if (name) {
        for (const char* p = name; *p; p++) {
            h ^= (u32)(unsigned char)*p;
            h *= 16777619u;
        }
    }
    return h ? h : 1u;
}

// Stable [0,1) random from (marker seed, salt).
static float landRand(u32 seed, u32 salt) {
    u32 h = landHash3(seed, salt, 0x85ebca6bu);
    return (float)(h >> 8) / 16777216.0f;
}

static float landClamp01Jit(u32 seed, u32 salt, float lo, float hi) {
    return lo + (hi - lo) * landRand(seed, salt);
}

// ── State ──────────────────────────────────────────────────────────────────

static const AzgaarWorld* g_world = NULL;
static bool               g_disabled = false;
static u32                g_mapSeed;

static PropInstance*  g_instances = NULL;
static u32            g_instanceCount = 0;
static PropTileRange* g_ranges = NULL;
static u32            g_rangeCount = 0;

// Pool decal handles (hot-springs / water-sources).
static u32* g_decals = NULL; // Array(u32)

// Sacred-forest density discs.  `g_discCount` is published LAST (pool threads
// read it via azgaarLandmarksForestBoost while tiles stream).
typedef struct LandDisc {
    float wx, wz;
} LandDisc;
static LandDisc* g_discs = NULL;
static volatile u32 g_discCount = 0;
#define LANDMARK_FOREST_DISC_R 300.0f
#define LANDMARK_FOREST_BOOST 3.0f

// ── Instance staging ───────────────────────────────────────────────────────

static void landPush(PropInstance* temp, u32 cap, u32* w,
                     float x, float y, float z, float yaw, float scale, u32 species,
                     float cr, float cg, float cb) {
    if (*w >= cap) return;
    PropInstance* p = &temp[(*w)++];
    p->pos[0]   = x;
    p->pos[1]   = y;
    p->pos[2]   = z;
    p->yaw      = yaw;
    p->scale    = scale;
    p->color[0] = cr;
    p->color[1] = cg;
    p->color[2] = cb;
    p->phase    = 0.0f;
    p->species  = species;
}

static float landGroundY(float (*groundAt)(void*, float, float), void* ud, float wx, float wz) {
    return groundAt ? groundAt(ud, wx, wz) : 0.0f;
}

// ── Per-kind generation ────────────────────────────────────────────────────

// Volcano: one cone+crater instance, 420..850 m tall (clamped to the species
// range), grey rock tinted with deterministic jitter.
static void landGenVolcano(const AzgaarMarker* m, u32 seed,
                           float (*groundAt)(void*, float, float), void* ud,
                           PropInstance* temp, u32 cap, u32* w) {
    float jit   = landClamp01Jit(seed, 1, 0.90f, 1.10f);
    float scale = landClamp01Jit(seed, 2, 420.0f, 850.0f);
    if (scale > 900.0f) scale = 900.0f;
    float gy = landGroundY(groundAt, ud, m->wx, m->wz);
    landPush(temp, cap, w, m->wx, gy, m->wz,
             landRand(seed, 3) * 2.0f * M_PI, scale, AZGAAR_PROP_VOLCANO,
             0.42f * jit, 0.40f * jit, 0.38f * jit);
}

// Lighthouse v1: grey tower + near-white cap instance standing on its top
// (22% of the total height), reading as the lit lantern from afar.
static void landGenLighthouse(const AzgaarMarker* m, u32 seed,
                              float (*groundAt)(void*, float, float), void* ud,
                              PropInstance* temp, u32 cap, u32* w) {
    float h   = landClamp01Jit(seed, 1, 22.0f, 30.0f);
    float jit = landClamp01Jit(seed, 2, 0.92f, 1.08f);
    float yaw = landRand(seed, 3) * 2.0f * M_PI;
    float gy  = landGroundY(groundAt, ud, m->wx, m->wz);
    landPush(temp, cap, w, m->wx, gy, m->wz, yaw, h, AZGAAR_PROP_LIGHTHOUSE,
             0.82f * jit, 0.80f * jit, 0.76f * jit);
    landPush(temp, cap, w, m->wx, gy + 0.78f * h, m->wz, yaw, 0.22f * h,
             AZGAAR_PROP_LIGHTHOUSE_CAP, 0.98f, 0.97f, 0.92f);
}

// Ruins: 3-5 half-buried broken columns/arches in a golden-angle cluster.
static void landGenRuins(const AzgaarMarker* m, u32 seed,
                         float (*groundAt)(void*, float, float), void* ud,
                         PropInstance* temp, u32 cap, u32* w) {
    u32   n  = 3u + (u32)(landRand(seed, 1) * 3.0f); // 3..5
    float ga = 2.39996f;                              // golden angle
    for (u32 k = 0; k < n; k++) {
        u32   salt = 10u + k * 7u;
        float ang  = ga * (float)k + landRand(seed, salt) * 0.5f;
        float rad  = 4.0f + 18.0f * sqrtf(landRand(seed, salt + 1u));
        float px   = m->wx + cosf(ang) * rad;
        float pz   = m->wz + sinf(ang) * rad;
        float gy   = landGroundY(groundAt, ud, px, pz);
        float sink = 0.3f + 1.2f * landRand(seed, salt + 2u); // half-buried
        float jit  = landClamp01Jit(seed, salt + 3u, 0.85f, 1.15f);
        bool  col  = landRand(seed, salt + 4u) < 0.7f;
        float sc   = col ? landClamp01Jit(seed, salt + 5u, 2.5f, 4.5f)
                         : landClamp01Jit(seed, salt + 5u, 3.0f, 5.0f);
        landPush(temp, cap, w, px, gy - sink, pz,
                 landRand(seed, salt + 6u) * 2.0f * M_PI, sc,
                 col ? AZGAAR_PROP_RUIN_COLUMN : AZGAAR_PROP_RUIN_ARCH,
                 0.55f * jit, 0.53f * jit, 0.50f * jit);
    }
}

// Mine: timber headframe + 2-3 scattered rocks.
static void landGenMine(const AzgaarMarker* m, u32 seed,
                        float (*groundAt)(void*, float, float), void* ud,
                        PropInstance* temp, u32 cap, u32* w) {
    float jit = landClamp01Jit(seed, 1, 0.90f, 1.10f);
    float sc  = landClamp01Jit(seed, 2, 4.0f, 6.0f);
    float gy  = landGroundY(groundAt, ud, m->wx, m->wz);
    landPush(temp, cap, w, m->wx, gy, m->wz,
             landRand(seed, 3) * 2.0f * M_PI, sc, AZGAAR_PROP_MINE_FRAME,
             0.45f * jit, 0.33f * jit, 0.20f * jit);
    u32   n  = 2u + (u32)(landRand(seed, 4) * 2.0f); // 2..3
    float ga = 2.39996f;
    for (u32 k = 0; k < n; k++) {
        u32   salt = 20u + k * 5u;
        float ang  = ga * (float)k + landRand(seed, salt);
        float rad  = 2.0f + 3.0f * landRand(seed, salt + 1u);
        float px   = m->wx + cosf(ang) * rad;
        float pz   = m->wz + sinf(ang) * rad;
        float rjit = landClamp01Jit(seed, salt + 2u, 0.85f, 1.15f);
        landPush(temp, cap, w, px, landGroundY(groundAt, ud, px, pz), pz,
                 landRand(seed, salt + 3u) * 2.0f * M_PI,
                 landClamp01Jit(seed, salt + 4u, 2.0f, 6.0f), AZGAAR_PROP_ROCK,
                 0.50f * rjit, 0.50f * rjit, 0.50f * rjit);
    }
}

// Bridge: one plank-bridge instance (deck authored unit-length along local
// +Z) across the river at the marker cell.  The river hash gives the span
// and the local flow direction sets the yaw.
static void landGenBridge(const AzgaarWorld* world, const AzgaarMarker* m, u32 seed,
                          float (*groundAt)(void*, float, float), void* ud,
                          PropInstance* temp, u32 cap, u32* w) {
    AzgaarRiverNearHit hits[16];
    u32 nh = azgaarRiversNear(m->wx, m->wz, 60.0f, hits, 16);
    if (nh == 0) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            warn("azgaarLandmarks: bridge marker %u has no river within 60 m; skipped", m->id);
        }
        return;
    }
    u32 nearest = 0;
    float bestD = -1.0f;
    for (u32 i = 0; i < nh; i++) {
        float dx = hits[i].wx - m->wx;
        float dz = hits[i].wz - m->wz;
        float d  = dx * dx + dz * dz;
        if (bestD < 0.0f || d < bestD) {
            bestD  = d;
            nearest = i;
        }
    }
    // Farthest point of the SAME river = local flow direction.
    u32 farthest = nearest;
    float farD = -1.0f;
    for (u32 i = 0; i < nh; i++) {
        if (hits[i].riverId != hits[nearest].riverId) continue;
        float dx = hits[i].wx - hits[nearest].wx;
        float dz = hits[i].wz - hits[nearest].wz;
        float d  = dx * dx + dz * dz;
        if (d > farD) {
            farD     = d;
            farthest = i;
        }
    }
    float dirX = hits[farthest].wx - hits[nearest].wx;
    float dirZ = hits[farthest].wz - hits[nearest].wz;
    float len  = sqrtf(dirX * dirX + dirZ * dirZ);
    if (len < 0.5f) {
        dirX = 1.0f;
        dirZ = 0.0f;
        len  = 1.0f;
    }
    dirX /= len;
    dirZ /= len;

    float span = hits[nearest].widthM + 8.0f; // river width + bank margins
    if (span < 8.0f) span = 8.0f;
    if (span > 48.0f) span = 48.0f;

    float seaY = azgaarSeaLevelMeters(world);
    float gy   = landGroundY(groundAt, ud, hits[nearest].wx, hits[nearest].wz);
    float y    = (gy > seaY ? gy : seaY) + 0.35f; // deck clears the banks/water
    float jit  = landClamp01Jit(seed, 1, 0.90f, 1.10f);
    // Deck is authored along local +Z; rotate the flow yaw by 90° so the
    // deck spans PERPENDICULAR to the river, not along it.
    landPush(temp, cap, w, hits[nearest].wx, y, hits[nearest].wz,
             atan2f(-dirZ, dirX), span, AZGAAR_PROP_BRIDGE,
             0.42f * jit, 0.31f * jit, 0.19f * jit);
}

// Hot-spring / water-source: a small round pool decal (steam is skipped in
// v1 — the props pass has no billboarded alpha quads).
static void landGenPoolDecal(const AzgaarMarker* m, u32 seed, bool hot,
                             float (*groundAt)(void*, float, float), void* ud) {
    float r  = hot ? landClamp01Jit(seed, 1, 3.0f, 6.0f)
                   : landClamp01Jit(seed, 1, 4.0f, 8.0f);
    float gy = landGroundY(groundAt, ud, m->wx, m->wz);

    DecalInstance d = {};
    d.position[0]   = m->wx;
    d.position[1]   = gy + 40.0f; // tall projector like the river decals
    d.position[2]   = m->wz;
    d.halfExtents[0] = r;
    d.halfExtents[1] = 140.0f;
    d.halfExtents[2] = r;
    glm_quat_identity(d.rotation);
    glm_vec4_copy((vec4){hot ? 0.36f : 0.25f, hot ? 0.52f : 0.40f, 0.55f,
                         hot ? 0.85f : 0.80f}, d.color);
    d.textureId      = DECAL_PROCEDURAL_CIRCLE_TEXTURE;
    d.flags          = DECAL_FLAG_GROUND_ONLY;
    d.opacity        = 1.0f;
    d.normalThreshold = 0.28f;
    d.edgeFeather    = 0.5f;
    d.uvScale[0]     = 1.0f;
    d.uvScale[1]     = 1.0f;
    u32 handle = decalAdd(&d);
    if (handle != DECAL_INVALID_HANDLE) arrayPut(g_decals, handle);
}

// ── Public API ─────────────────────────────────────────────────────────────

void azgaarLandmarksInit(const AzgaarWorld* world,
                         float (*groundAt)(void* userData, float wx, float wz),
                         void* groundUserData) {
    if (!world) return;
    azgaarLandmarksClear();
    g_world    = world;
    g_mapSeed  = landMapSeed(world->mapName);
    g_disabled = getenv("ENGINE_AZGAAR_LANDMARKS_DISABLED") != NULL;
    if (g_disabled || world->markerCount == 0 || !world->markers) return;

    // Budget the temp buffer exactly: worst case per marker kind.
    u32 cap = 0;
    for (u32 i = 0; i < world->markerCount; i++) {
        switch (world->markers[i].kind) {
        case AZGAAR_MARKER_VOLCANO: cap += 1; break;
        case AZGAAR_MARKER_LIGHTHOUSE: cap += 2; break;
        case AZGAAR_MARKER_RUINS: cap += 5; break;
        case AZGAAR_MARKER_MINE: cap += 4; break; // frame + 3 rocks
        case AZGAAR_MARKER_BRIDGE: cap += 1; break;
        default: break; // decals / sacred discs add no instances
        }
    }
    if (cap == 0) return;

    PropInstance* temp = static_cast<PropInstance*>(memoryAlloc(sizeof(PropInstance) * cap));
    u32 write = 0;
    u32 volcanoes = 0, lighthouses = 0, ruins = 0, mines = 0, bridges = 0, pools = 0,
        sacred = 0;
    for (u32 i = 0; i < world->markerCount; i++) {
        const AzgaarMarker* m = &world->markers[i];
        u32 seed = g_mapSeed ^ m->id * 374761393u;
        switch (m->kind) {
        case AZGAAR_MARKER_VOLCANO:
            landGenVolcano(m, seed, groundAt, groundUserData, temp, cap, &write);
            volcanoes++;
            break;
        case AZGAAR_MARKER_LIGHTHOUSE:
            landGenLighthouse(m, seed, groundAt, groundUserData, temp, cap, &write);
            lighthouses++;
            break;
        case AZGAAR_MARKER_RUINS:
            landGenRuins(m, seed, groundAt, groundUserData, temp, cap, &write);
            ruins++;
            break;
        case AZGAAR_MARKER_MINE:
            landGenMine(m, seed, groundAt, groundUserData, temp, cap, &write);
            mines++;
            break;
        case AZGAAR_MARKER_BRIDGE:
            landGenBridge(world, m, seed, groundAt, groundUserData, temp, cap, &write);
            bridges++;
            break;
        case AZGAAR_MARKER_HOT_SPRING:
            landGenPoolDecal(m, seed, true, groundAt, groundUserData);
            pools++;
            break;
        case AZGAAR_MARKER_WATER_SOURCE:
            landGenPoolDecal(m, seed, false, groundAt, groundUserData);
            pools++;
            break;
        case AZGAAR_MARKER_SACRED_FOREST: {
            // Publish discs with the count LAST (pool threads poll it).
            LandDisc* discs = static_cast<LandDisc*>(memoryAlloc(sizeof(LandDisc) * (u32)(sacred + 1u)));
            if (g_discs) memcpy(discs, g_discs, sizeof(LandDisc) * sacred);
            memoryFree(g_discs);
            g_discs = discs;
            g_discs[sacred].wx = m->wx;
            g_discs[sacred].wz = m->wz;
            sacred++;
            g_discCount = sacred;
            break;
        }
        default: break;
        }
    }
    g_instanceCount = write;

    // Group the unsorted instances by species, then upload to the pass.
    u32 perSpecies[AZGAAR_PROP_COUNT] = {};
    for (u32 i = 0; i < g_instanceCount; i++) {
        u32 sp = temp[i].species;
        if (sp < AZGAAR_PROP_COUNT) perSpecies[sp]++;
    }
    if (g_instanceCount > 0) {
        memoryFree(g_instances);
        memoryFree(g_ranges);
        g_instances  = static_cast<PropInstance*>(memoryAlloc(sizeof(PropInstance) * g_instanceCount));
        u32 offsets[AZGAAR_PROP_COUNT] = {};
        u32 acc = 0;
        for (u32 s = 0; s < AZGAAR_PROP_COUNT; s++) {
            offsets[s] = acc;
            acc += perSpecies[s];
        }
        u32 cursor[AZGAAR_PROP_COUNT] = {};
        for (u32 i = 0; i < g_instanceCount; i++) {
            u32 sp = temp[i].species;
            u32 dst = offsets[sp] + cursor[sp]++;
            g_instances[dst] = temp[i];
        }
        g_ranges  = static_cast<PropTileRange*>(memoryAlloc(sizeof(PropTileRange) * AZGAAR_PROP_COUNT));
        u32 rc = 0;
        for (u32 s = 0; s < AZGAAR_PROP_COUNT; s++) {
            if (perSpecies[s] > 0) {
                g_ranges[rc] = (PropTileRange){.species = s, .start = offsets[s], .count = perSpecies[s]};
                rc++;
            }
        }
        g_rangeCount = rc;

        float halfW = (float)(world->widthPx * 0.5) * (float)world->metersPerPixel + 40.0f;
        float halfH = (float)(world->heightPx * 0.5) * (float)world->metersPerPixel + 40.0f;
        float aabbMin[3] = {-halfW, -20.0f, -halfH};
        float aabbMax[3] = {halfW, world->maxLandHeightM + 900.0f + 20.0f, halfH};
        azgaarPropsRegisterGlobal(g_instances, g_instanceCount,
                                      g_ranges, g_rangeCount, aabbMin, aabbMax, true);
    }
    memoryFree(temp);

    info("azgaarLandmarks: %u instances in %u species ranges "
         "(volcanoes=%u lighthouses=%u ruins=%u mines=%u bridges=%u pool-decals=%u sacred-forests=%u)",
         g_instanceCount, g_rangeCount, volcanoes, lighthouses, ruins, mines, bridges,
         pools, sacred);
    // TEMP DEBUG: dump the marquee landmark transforms for screenshot setup.
    for (u32 i = 0; i < g_instanceCount; i++) {
        u32 sp = g_instances[i].species;
        if (sp == AZGAAR_PROP_VOLCANO || sp == AZGAAR_PROP_LIGHTHOUSE ||
            sp == AZGAAR_PROP_BRIDGE) {
            info("TEMP landmark species=%u pos=(%.1f, %.1f, %.1f) yaw=%.1f scale=%.1f",
                 sp, g_instances[i].pos[0], g_instances[i].pos[1], g_instances[i].pos[2],
                 (double)g_instances[i].yaw, g_instances[i].scale);
        }
    }
}

void azgaarLandmarksClear(void) {
    azgaarPropsClearGlobal(true);
    memoryFree(g_instances);
    g_instances     = NULL;
    g_instanceCount = 0;
    memoryFree(g_ranges);
    g_ranges     = NULL;
    g_rangeCount = 0;
    if (g_decals) {
        for (u32 i = 0; i < arraySize(g_decals); i++) {
            decalRemove(g_decals[i]);
        }
        arrayFree(g_decals);
    }
    g_discCount = 0; // before freeing: queries fall back to 1.0 immediately
    memoryFree(g_discs);
    g_discs     = NULL;
    g_world     = NULL;
    g_disabled  = false;
}

float azgaarLandmarksForestBoost(float wx, float wz) {
    u32 count = g_discCount; // published last by init
    if (count == 0 || !g_discs) return 1.0f;
    for (u32 i = 0; i < count; i++) {
        float dx = wx - g_discs[i].wx;
        float dz = wz - g_discs[i].wz;
        if (dx * dx + dz * dz < LANDMARK_FOREST_DISC_R * LANDMARK_FOREST_DISC_R) {
            return LANDMARK_FOREST_BOOST;
        }
    }
    return 1.0f;
}
