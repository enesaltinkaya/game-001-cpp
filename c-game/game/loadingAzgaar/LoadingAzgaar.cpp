#include "LoadingAzgaar.h"
#include "LoadingAzgaar.h"
#include "azgaar/AzgaarHeightmapSource.h"
#include "azgaar/AzgaarRoadDecals.h"
#include "azgaar/AzgaarRoadCorridor.h"
#include "azgaar/AzgaarWater.h"
#include "azgaar/AzgaarWeather.h"
#include "azgaar/AzgaarRivers.h"
#include "azgaar/AzgaarLandmarks.h"
#include "azgaar/AzgaarSettlements.h"
#include "azgaar/AzgaarWorld.h"
#include "gameState/GameState.h"
#include "player/Player.h"
#include "ecs/system/scene/SceneParser.h"
#include "ecs/system/scene/SceneSystem.h"
#include "ecs/system/heightmap/HeightmapTerrain.h"
#include "ecs/system/transform/TransformComponent.h"
#include "ecs/system/transform/TransformDb.h"
#include "ecs/system/window/WindowSystem.h"
#include "renderer/Renderer.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/resources/VulkanBuffer.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "rmlui/wrapper/src/crmlui.h"
#include "timer/Timer.h"
#include "futuretask/FutureTask.h"

// Forward declare to avoid including AnimatorComponent.h (EventCallback name clash with crmlui.h)
namespace engine {
void animationPlayBlendedByName(const char* entityName,
                                const char* clipName,
                                float speed,
                                bool loop,
                                float blendDuration);
}  // namespace engine

namespace game {

LoadingAzgaarSystem loadingAzgaarSystem;

LoadingAzgaarSystem::LoadingAzgaarSystem() : engine::System("loadingAzgaar") {}

enum AzgaarLoadStage {
    AZGAAR_LOAD_STAGE_MAP,
    AZGAAR_LOAD_STAGE_WORLD_DATA,
    AZGAAR_LOAD_STAGE_TERRAIN,
    AZGAAR_LOAD_STAGE_ANIMATIONS,
    AZGAAR_LOAD_STAGE_READY,
    AZGAAR_LOAD_STAGE_ERROR,
};

static const char* const stageTexts[] = {
    [AZGAAR_LOAD_STAGE_MAP]        = "Loading Azgaar map...",
    [AZGAAR_LOAD_STAGE_WORLD_DATA] = "Parsing Azgaar world data...",
    [AZGAAR_LOAD_STAGE_TERRAIN]    = "Generating Azgaar terrain tiles...",
    [AZGAAR_LOAD_STAGE_ANIMATIONS] = "Loading animations...",
    [AZGAAR_LOAD_STAGE_READY]      = "Azgaar world loaded...",
    [AZGAAR_LOAD_STAGE_ERROR]      = "Failed to load Azgaar world.",
};

static const char* const azgaarMapPath = "azgaar/Chilerel 2026-08-11-15-35.map";

static AzgaarLoadStage loadStage;
static char cancelled;
static void* document;
static void* model;
static char stageTextBuf[128];
static char* stageTextPtr = stageTextBuf;
// Shown below the stage text; only non-empty on the error stage ("Press ESC
// to return") since a running load can no longer be cancelled.
static char hintBuf[64];
static char* hintPtr = hintBuf;
static AzgaarWorld azgaarWorld;
static bool worldLoaded;  // azgaarWorld holds valid data
// Climate textures (workstream A): static per-world RGBA8 uploads sampled by
// the heightmap terrain pass for biome tint / snow / beach blending.
static engine::VulkanImage climateBiomeColorImg;
static engine::VulkanImage climateFieldImg;
static AzgaarHeightmapSource azgaarHeightmapSrc;
// Heightmap host for the loading stage: the window is generated while the
// loading screen is up, then activated as the active heightmap world at the
// READY stage (the engine's heightmap system follows the player camera from
// the first gameplay frame).
static engine::HeightmapTerrain loadHeightmap;
static float loadSpawnX, loadSpawnZ;
static i32 loadCenterTileX, loadCenterTileZ;
static bool heightmapAttached;

static void azgaarHeightmapDetach(void);
static void azgaarHeightmapVerifyGrid(void* userData);
static engine::Scene* loadedAnimationsScene;
static char animationsSignalEmitted;
static char keepAssetsOnExit;
// Key of the deferred heavy-init future task (0 = none pending/ran).
static int startLoadTaskKey;

static void emitAnimationsLoadedIfTerrainReady(void) {
    if (animationsSignalEmitted) return;
    if (loadStage != AZGAAR_LOAD_STAGE_ANIMATIONS) return;
    if (!loadedAnimationsScene) return;

    animationsSignalEmitted = 1;
    utils::signalEmit("animationsLoaded", nullptr);
}

static void checkReady(void) {
    emitAnimationsLoadedIfTerrainReady();
    if (loadStage == AZGAAR_LOAD_STAGE_ANIMATIONS && loadedAnimationsScene &&
        getPlayerScene() != nullptr) {
        loadStage = AZGAAR_LOAD_STAGE_READY;
    }
}

static void azgaarAnimationsLoaded(engine::Scene* scene, void* _) {
    static_cast<void>(_);
    if (cancelled) {
        engine::rendererSceneDestroy(scene);
        engine::sceneDestroy(scene);
        return;
    }

    loadedAnimationsScene = scene;
    scene->alwaysVisible  = true;
    engine::animationPlayBlendedByName("eve_animator", "female_walk", 0.1f, true, 1.0f);
    checkReady();
}

const char* loadingAzgaarStageText(void) {
    return stageTexts[loadStage];
}

const AzgaarWorld* loadingAzgaarGetWorld(void) {
    return worldLoaded ? &azgaarWorld : nullptr;
}

AzgaarHeightmapSource* loadingAzgaarGetHeightmapSource(void) {
    return worldLoaded ? &azgaarHeightmapSrc : nullptr;
}

// ── Climate textures + terrain climate state (workstream A) ────────────
// Packs the world's biome-colour and climate grids into two RGBA8 textures,
// uploads them once per world load, and points the terrain pass' TerrainData
// at them (plus map bounds for the world->map-UV transform and the blend
// thresholds, env-overridable).  Mirrored by azgaarClimateDestroy on release.
static void azgaarClimateDestroy(void) {
    if (climateBiomeColorImg.img) engine::vulkanDestroyImage(&climateBiomeColorImg, VK_NULL_HANDLE);
    if (climateFieldImg.img) engine::vulkanDestroyImage(&climateFieldImg, VK_NULL_HANDLE);
    climateBiomeColorImg = engine::VulkanImage{};
    climateFieldImg      = engine::VulkanImage{};
    // Clear the SceneBuffer slots so nothing samples stale (freed) pool
    // indices after the world is gone.
    engine::vulkanResourceSetTerrainClimateTextures(0, 0);
    engine::vulkanResourceSetTerrainClimateParams(0.0f, 0.0f, 0.0f, 0.0f);
}

static engine::VulkanImage azgaarClimateUploadTexture(const char* name,
                                              VkFormat format,
                                              const u8* pixels,
                                              u32 width,
                                              u32 height) {
    engine::VulkanImage img = vulkanCreateImage(.name   = name,
                                        .format = format,
                                        .usage  = VK_IMAGE_USAGE_SAMPLED_BIT |
                                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                        .width  = (int)width,
                                        .height = (int)height);
    if (!img.img) {
        utils::warn("loadingAzgaar: climate texture creation failed: %s", name);
        return img;
    }

    engine::VulkanCommand* cmd = engine::vulkanTransientBegin();
    vulkanTransition(cmd, &img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);
    vulkanCopy(.cmd        = cmd,
               .source.data = (void*)pixels,
               .target.img  = &img,
               .size        = static_cast<u32>(width) * height * 4u);
    vulkanTransition(cmd, &img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    engine::vulkanTransientEnd(cmd, 1);
    return img;
}

static float azgaarEnvFloat(const char* name, float fallback) {
    const char* v = getenv(name);
    if (!v || !*v) return fallback;
    float f = static_cast<float>(atof(v));
    return f;
}

static void azgaarClimateUpload(const AzgaarWorld* world) {
    azgaarClimateDestroy();

    u32 w = 0, h = 0;
    std::vector<u8> biomePixels = azgaarWorldPackBiomeColorTexture(world, &w, &h);
    std::vector<u8> fieldPixels = azgaarWorldPackClimateTexture(world, &w, &h);
    if (biomePixels.empty() || fieldPixels.empty()) {
        utils::warn("loadingAzgaar: no climate grids; terrain climate blending stays off");
        return;
    }

    // Biome colours are authored display-referred values -> sRGB view so the
    // GPU decodes them to linear like the other albedo textures; the climate
    // field carries exact byte-encoded scalars -> UNORM roundtrip.
    climateBiomeColorImg = azgaarClimateUploadTexture("azgaar_biome_color",
                                                      VK_FORMAT_R8G8B8A8_SRGB,
                                                      biomePixels.data(),
                                                      w,
                                                      h);
    climateFieldImg = azgaarClimateUploadTexture("azgaar_climate",
                                                  VK_FORMAT_R8G8B8A8_UNORM,
                                                  fieldPixels.data(),
                                                  w,
                                                  h);

    if (!climateBiomeColorImg.img || !climateFieldImg.img) {
        azgaarClimateDestroy();
        return;
    }

    engine::vulkanResourceSetTerrainClimateTextures(climateBiomeColorImg.sampledPoolIndex,
                                             climateFieldImg.sampledPoolIndex);

    // Blend thresholds (deg C / m above sea level).  Same env-var pattern as
    // ENGINE_AZGAAR_HM_SIGMA.  ENGINE_AZGAAR_CLIMATE_DISABLED turns every
    // climate fetch off (kill switch / A-B timing baseline).
    float snowLo = azgaarEnvFloat("ENGINE_AZGAAR_SNOW_LO", -1.0f);
    float snowHi = azgaarEnvFloat("ENGINE_AZGAAR_SNOW_HI", 3.0f);
    float beachH = azgaarEnvFloat("ENGINE_AZGAAR_BEACH_H", 2.5f);
    if (getenv("ENGINE_AZGAAR_CLIMATE_DISABLED")) {
        snowLo = snowHi = beachH = 0.0f;
    }
    engine::vulkanResourceSetTerrainClimateParams(snowLo, snowHi, beachH, beachH > 0.0f ? 1.0f : 0.0f);

    // Map bounds in world space: the terrain pass derives the map-UV (for
    // both climate textures) and the altitude rock band (worldMax.y) from
    // these.  azgaarMapToWorld centres the map at the world origin.
    float halfW = static_cast<float>(world->widthPx * 0.5) * static_cast<float>(world->metersPerPixel);
    float halfH = static_cast<float>(world->heightPx * 0.5) * static_cast<float>(world->metersPerPixel);
    engine::vulkanResourceSetTerrainBounds(-halfW,
                                   -AZGAAR_OCEAN_DEPTH_METERS,
                                   -halfH,
                                   halfW,
                                   world->maxLandHeightM,
                                   halfH);

    utils::info("loadingAzgaar: climate textures uploaded (biomeColor=%ux%u idx=%d, climate idx=%d), "
         "snow=[%.1f, %.1f] C, beach=%.1f m, maxLand=%.0f m%s",
         w,
         h,
         climateBiomeColorImg.sampledPoolIndex,
         climateFieldImg.sampledPoolIndex,
         snowLo,
         snowHi,
         beachH,
         world->maxLandHeightM,
         getenv("ENGINE_AZGAAR_CLIMATE_DISABLED") ? " (DISABLED)" : "");
}

void loadingAzgaarReleaseWorld(void) {
    if (!worldLoaded) {
        // A failed reload may still hold textures from the previous world
        // (azgaarWorldLoad already reset the world struct on failure).
        azgaarClimateDestroy();
        azgaarHeightmapDetach();
        return;
    }
    azgaarClimateDestroy();
    azgaarHeightmapDetach();
    azgaarWaterDestroy();
    azgaarWeatherDestroy();
    azgaarRiversClear();
    azgaarLandmarksClear();
    azgaarSettlementsClear();
    azgaarRoadDecalsClear();
    azgaarRoadCorridorClear();
    azgaarWorldDestroy(&azgaarWorld);
    worldLoaded = false;
}

// Pick the tile to center the initial generation on. Uses the player's saved
// DB transform when present (so reloading drops the player back onto terrain
// they previously stood on); otherwise falls back to the default spawn.
static void azgaarInitialCenterTile(i32* outTileX, i32* outTileZ, float* outSpawnX, float* outSpawnZ) {
    float spawnX;
    float spawnZ;
    vec3 spawn;
    playerGetSpawn(spawn);
    spawnX = spawn[0];
    spawnZ = spawn[2];

    engine::transformDbInit();
    engine::Transform saved;
    if (engine::transformDbLoad("player", &saved)) {
        spawnX = saved.pos[0];
        spawnZ = saved.pos[2];
    }

    *outTileX = engine::heightmapWorldToTileCoord(&loadHeightmap, spawnX);
    *outTileZ = engine::heightmapWorldToTileCoord(&loadHeightmap, spawnZ);
    if (outSpawnX) *outSpawnX = spawnX;
    if (outSpawnZ) *outSpawnZ = spawnZ;
    utils::info("loadingAzgaar: initial terrain center tile (%d, %d) from spawn (%.1f, %.1f)",
         *outTileX,
         *outTileZ,
         spawnX,
         spawnZ);
}

static void azgaarSnapPlayerToTerrain(void);

// Make the loading-stage heightmap host the active heightmap world. Called
// once per load, right before the gameplay transition; from the first
// gameplay frame the engine's heightmap system drives the window from the
// player camera.
static void azgaarHeightmapAttach(void) {
    engine::heightmapTerrainSetActive(&loadHeightmap);

    utils::info("azgaarHeightmap: activated (tile %.0f m, window %u) seed=0x%08x",
         HEIGHTMAP_TILE_SIZE_M,
         HEIGHTMAP_WINDOW_SIZE,
         azgaarHeightmapSrc.noiseSeed);

    // One-shot sanity probes through the public sample API. The world origin
    // is the map centre; +/-2048 m crosses one tile border.
    const float probeWx[4] = {0.0f, 2048.0f, -1024.0f, 512.0f};
    const float probeWz[4] = {0.0f, -2048.0f, 1024.0f, -512.0f};
    for (int i = 0; i < 4; ++i) {
        float y = engine::heightmapTerrainSample(&loadHeightmap, probeWx[i], probeWz[i]);
        utils::info("azgaarHeightmap: probe (%.0f, %.0f) -> %.2f m", probeWx[i], probeWz[i], y);
    }

    // The initial window takes ~1.2 s to generate on the builder thread;
    // verify the grid-vs-source agreement once it is ready.
    utils::futureTaskAdd(4000, azgaarHeightmapVerifyGrid, nullptr);
}

// Free the tile data and clear the active pointer (the host struct itself is
// a file-static and survives; re-init happens on the next load).
// Phase 1 verification: once the initial window is generated, confirm the
// CPU-grid sample path returns the same values as the source at texel-center
// coordinates (grid vertices store the source value verbatim, so the
// bilinear sampler must return it exactly). Catches coordinate-mapping bugs
// in tile origin / grid spacing before GPU and physics phases build on top.
struct HmVerifyPoint {
    i32 tileX, tileZ;
    i32 texX, texZ;
};

static void azgaarHeightmapVerifyGrid(void* _) {
    static_cast<void>(_);
    engine::HeightmapTerrain* ht = engine::heightmapTerrainGetActive();
    if (!ht) {
        utils::warn("azgaarHeightmap: verify skipped (no active heightmap terrain)");
        return;
    }

    const HmVerifyPoint points[5] = {
        {0, 0, 256, 256},
        {0, 0, 100, 401},
        {-1, 0, 300, 128},
        {-2, 2, 511, 0},   // border corner (shared with neighbours)
        {1, -2, 0, 511},   // border corner
    };

    int failures = 0;
    for (int i = 0; i < 5; ++i) {
        float step = HEIGHTMAP_TILE_SIZE_M / static_cast<float>(HEIGHTMAP_TEX - 1);
        float wx   = static_cast<float>(points[i].tileX) * HEIGHTMAP_TILE_SIZE_M + static_cast<float>(points[i].texX) * step;
        float wz   = static_cast<float>(points[i].tileZ) * HEIGHTMAP_TILE_SIZE_M + static_cast<float>(points[i].texZ) * step;

        float yGrid = engine::heightmapTerrainSample(ht, wx, wz);
        float ySrc  = azgaarHeightmapSrc.vtable.heightAt(&azgaarHeightmapSrc, wx, wz);
        float diff  = fabsf(yGrid - ySrc);
        bool ok     = (diff < 1e-3f);
        if (!ok) failures++;
        utils::info("azgaarHeightmap: verify tile(%d,%d) texel(%d,%d) grid=%.4f m source=%.4f m diff=%.6f m %s",
             points[i].tileX,
             points[i].tileZ,
             points[i].texX,
             points[i].texZ,
             yGrid,
             ySrc,
             diff,
             ok ? "OK" : "MISMATCH");
    }
    if (failures == 0) {
        utils::info("azgaarHeightmap: grid verification passed (5/5)");
    } else {
        utils::warn("azgaarHeightmap: grid verification FAILED (%d/5 mismatches)", failures);
    }
}

static void azgaarHeightmapDetach(void) {
    if (engine::heightmapTerrainGetActive() == &loadHeightmap) {
        engine::heightmapTerrainSetActive(nullptr);
    }
    // Safe on a never-initialized host (zeroed struct: the drain loop exits
    // immediately).
    engine::heightmapTerrainDestroyData(&loadHeightmap);
    heightmapAttached = false;
}

// Heavy synchronous part of the load (map read/parse, settlement clusters,
// climate uploads, heightmap host, road corridor/decals).  Runs one frame
// after loadingAzgaarOnEnter so the transition frame (menu hidden, loading
// screen shown) gets rendered before this work hitches the main thread.
static void loadingAzgaarStartLoad(void* _) {
    static_cast<void>(_);
    startLoadTaskKey = 0;
    if (cancelled) return;  // state was exited before the deferred start ran

    loadingAzgaarReleaseWorld();  // free any previously retained world
    azgaarWorld = AzgaarWorld{};

    loadStage = AZGAAR_LOAD_STAGE_WORLD_DATA;
    if (!azgaarWorldLoad(&azgaarWorld, azgaarMapPath)) {
        loadStage = AZGAAR_LOAD_STAGE_ERROR;
        return;
    }
    worldLoaded = true;
    utils::signalEmit("azgaarMapLoaded", nullptr);
    azgaarHeightmapSourceInit(&azgaarHeightmapSrc, &azgaarWorld, azgaarWorld.mapName);

    // Build the settlement building clusters and upload them to the
    // azgaar_props pass' global instance buffer (workstream D).  Runs BEFORE
    // any heightmap tile is generated, so the D8 plateau grid is live for
    // every tile (houses must not float above the ground).  Each building's
    // Y is sampled through the source' exact heightAt (natural + detail +
    // plateau) so buildings sit flush with the rendered terrain.
    azgaarSettlementsInit(&azgaarWorld, azgaarHeightmapSrc.vtable.heightAt, &azgaarHeightmapSrc);

    // Climate textures + terrain climate state (biome tint / snow / beach)
    // before any terrain tile renders.
    azgaarClimateUpload(&azgaarWorld);

    // Host the heightmap for the whole load: the window is generated in the
    // background while the loading screen is up (driven from the TERRAIN
    // stage below), so gameplay starts with a warm tile cache.
    heightmapTerrainInit(&loadHeightmap, &azgaarHeightmapSrc.vtable,
                         HEIGHTMAP_TILE_SIZE_M, HEIGHTMAP_WINDOW_SIZE);

    // Build the walkable road corridor before road decals are placed (so they
    // sample the corridor surface height).
    azgaarRoadCorridorBuild(&azgaarWorld);

    // Snap the player onto Azgaar terrain BEFORE computing the tile grid centre,
    // so the grid is centred on the player's actual position.
    azgaarSnapPlayerToTerrain();

    // TEMP (beach test): log terrain height at the saved player position vs
    // sea level, so we know whether the parked beach is dry or submerged.
    {
        engine::Transform dbgSaved;
        engine::transformDbInit();
        if (engine::transformDbLoad("player", &dbgSaved)) {
            float dbgH = azgaarHeightmapSrc.vtable.heightAt(&azgaarHeightmapSrc, dbgSaved.pos[0], dbgSaved.pos[2]);
            utils::info("TEMP beach test: player xz=(%.2f, %.2f) terrainH=%.3f seaLevel=%.3f -> %s",
                        dbgSaved.pos[0], dbgSaved.pos[2], dbgH, azgaarSeaLevelMeters(&azgaarWorld),
                        dbgH >= azgaarSeaLevelMeters(&azgaarWorld) ? "DRY" : "SUBMERGED");
        }
    }

    i32 centerTileX;
    i32 centerTileZ;
    float spawnX;
    float spawnZ;
    azgaarInitialCenterTile(&centerTileX, &centerTileZ, &spawnX, &spawnZ);
    loadSpawnX        = spawnX;
    loadSpawnZ        = spawnZ;
    loadCenterTileX   = centerTileX;
    loadCenterTileZ   = centerTileZ;
    azgaarRoadDecalsBuild(&azgaarWorld);

    loadStage = AZGAAR_LOAD_STAGE_TERRAIN;

    engine::sceneLoadCb("models/animations.dat", azgaarAnimationsLoaded, nullptr);
}

void loadingAzgaarOnEnter(void) {
    // Light state setup only (the loading GUI itself is shown by
    // LoadingAzgaarSystem::added()); the heavy init is deferred to the next
    // frame so the current frame can present the loading screen instead of
    // freezing on the still-visible menu.
    cancelled               = 0;
    loadStage               = AZGAAR_LOAD_STAGE_MAP;
    loadedAnimationsScene   = nullptr;
    animationsSignalEmitted = 0;
    keepAssetsOnExit        = 0;

    // Drop a pending start from a faster exit/re-enter cycle.
    if (startLoadTaskKey) {
        utils::futureTaskRemove(startLoadTaskKey);
    }
    startLoadTaskKey = utils::futureTaskAdd(0, loadingAzgaarStartLoad, nullptr);
}

void loadingAzgaarOnExit(void) {
    // Cancel a heavy init that has not run yet (exit during the deferral gap).
    if (startLoadTaskKey) {
        utils::futureTaskRemove(startLoadTaskKey);
        startLoadTaskKey = 0;
    }
    cancelled = 1;
    if (!keepAssetsOnExit) {
        azgaarRoadDecalsClear();
        if (loadedAnimationsScene) {
            engine::rendererSceneDestroy(loadedAnimationsScene);
            engine::sceneDestroy(loadedAnimationsScene);
            loadedAnimationsScene = nullptr;
        }
        // Cancel path: free the world now. On the gameplay path the world is
        // retained for streaming and released by gameplay teardown instead.
        loadingAzgaarReleaseWorld();
    }
}

void LoadingAzgaarSystem::added() {
    document = rmlNewDocument("gui/loading/loading.html");
    model    = rmlCreateModel("loading");
    rmlBindCharPointer(model, "stage", &stageTextPtr);
    rmlBindCharPointer(model, "hint", &hintPtr);
    rmlLoadDocument(document);
    rmlShowDocument(document);
}

void LoadingAzgaarSystem::removed() {
    if (!keepAssetsOnExit) {
        azgaarRoadDecalsClear();
        if (loadedAnimationsScene) {
            engine::rendererSceneDestroy(loadedAnimationsScene);
            engine::sceneDestroy(loadedAnimationsScene);
            loadedAnimationsScene = nullptr;
        }
        loadingAzgaarReleaseWorld();
    }

    rmlUnloadDocument(document);
    document = nullptr;
    rmlUnloadModel(model);
    model = nullptr;
}

// Use the player's saved position as-is. No snapping or height adjustment.
static void azgaarSnapPlayerToTerrain(void) {
    if (!worldLoaded) return;

    engine::transformDbInit();
    engine::Transform saved;
    if (!engine::transformDbLoad("player", &saved)) {
        return;
    }

    // Already has the saved position — use it directly, no changes needed.
}

void LoadingAzgaarSystem::update() {
    if (cancelled) return;

    if (loadStage == AZGAAR_LOAD_STAGE_TERRAIN) {
        // The global heightmap system only follows the (production) camera, so
        // the loading screen drives the window itself, anchored at the spawn
        // point. Generation queues the center tile first, so the center being
        // ready is the earliest useful gate.
        engine::heightmapTerrainUpdateWindow(&loadHeightmap, loadSpawnX, loadSpawnZ);

        engine::HeightmapTile* centerTile = engine::heightmapTerrainGetTile(&loadHeightmap, loadCenterTileX, loadCenterTileZ);
        if (centerTile && centerTile->state == engine::HEIGHTMAP_TILE_READY) {
            loadStage = AZGAAR_LOAD_STAGE_ANIMATIONS;
            // Initialise the water grid once the seabed terrain is ready
            // (water reads the depth buffer written by the terrain pass).
            azgaarWaterInit(&azgaarWorld);
            // GPU particle weather: needs the terrain depth buffer at
            // runtime, not at init — start it beside the water system so
            // snow/dust are live from the first gameplay frame.
            azgaarWeatherInit(&azgaarWorld);
            // Build the river ribbons + wet-strip decals + river-point hash,
            // and upload the ribbon mesh to the azgaar_river pass.
            azgaarRiversInit(&azgaarWorld);
            // Landmarks (workstream E) run after the river hash so bridges
            // can span their river; also publishes the sacred-forest density
            // discs the props scatter queries during gameplay.
            azgaarLandmarksInit(&azgaarWorld,
                                azgaarHeightmapSrc.vtable.heightAt,
                                &azgaarHeightmapSrc);
            emitAnimationsLoadedIfTerrainReady();
        }
    }

    checkReady();

    // ESC only escapes a *failed* load (the error screen); a running load can
    // no longer be cancelled.
    if (engine::input.pressed == KEY_ESCAPE && loadStage == AZGAAR_LOAD_STAGE_ERROR) {
        gameStateTransition(STATE_MAIN_MENU);
        return;
    }

    snprintf(stageTextBuf, sizeof(stageTextBuf), "%s", stageTexts[loadStage]);
    snprintf(hintBuf, sizeof(hintBuf), "%s",
             loadStage == AZGAAR_LOAD_STAGE_ERROR ? "Press ESC to return" : "");
    rmlUpdateDirtyAll(model);

    // Transition as soon as everything is ready — no artificial minimum
    // hold on the loading screen.
    if (loadStage == AZGAAR_LOAD_STAGE_READY) {
        keepAssetsOnExit = 1;
        if (!heightmapAttached) {
            heightmapAttached = true;
            azgaarHeightmapAttach();
        }
        gameStateSetLoadedAnimationsScene(loadedAnimationsScene);
        loadedAnimationsScene = nullptr;
        gameStateTransition(STATE_GAMEPLAY);
    }
}
}  // namespace game
