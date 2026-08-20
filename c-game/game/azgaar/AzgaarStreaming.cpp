#include "AzgaarStreaming.h"
#include "azgaar/AzgaarStreaming.h"
#include "azgaar/AzgaarWorld.h"
#include "azgaar/AzgaarProps.h"
#include "loadingAzgaar/LoadingAzgaar.h"
#include "logger/Logger.h"
#include "ecs/system/window/WindowSystem.h"
#include "ecs/Ecs.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/transform/TransformComponent.h"
#include "ecs/system/transform/TransformSystem.h"
#include "player/Player.h"
#include "timer/Timer.h"
#include "renderer/vulkan/pass/heightmap_terrain/VulkanHeightmapTerrainPass.h"

namespace game {

AzgaarStreamingSystem azgaarStreamingSystem;

AzgaarStreamingSystem::AzgaarStreamingSystem() : engine::System("azgaarStreaming") {}

static const AzgaarWorld* streamWorld;

// TEMP DEBUG: ENGINE_CAM_TELEPORT="x,y,z,afterMs" teleports the player
// (and with them the camera + heightmap streaming window) after afterMs
// milliseconds — used to reach a specific biome/settlement/river for targeted
// screenshots.  Y is the desired camera height; the player is placed 40 m
// below it and physics settles it onto the terrain.
static void tempCameraTeleport(void) {
    static char* env = nullptr;
    if (!env) env = getenv("ENGINE_CAM_TELEPORT");
    if (!env || !*env) return;
    static double startMs = -1;
    static char done      = 0;
    double now            = utils::millies();
    if (startMs < 0) startMs = now;
    float x, y, z, afterMs;
    if (sscanf(env, "%f,%f,%f,%f", &x, &y, &z, &afterMs) != 4) return;
    if (done || now - startMs < afterMs) return;

    vec3 pos = {x, y - 40.0f, z};
    if (playerTeleportTo(pos)) {
        done = 1;
        utils::info("TEMP player teleport -> (%.0f,%.0f,%.0f)", static_cast<double>(x), static_cast<double>(y), static_cast<double>(z));
    }

    // Also snap the camera itself so the view (and the streaming window)
    // re-centers immediately instead of waiting for the controller.
    engine::Entity* cam = engine::cameraGetEntity();
    if (!cam) return;
    engine::Transform* t = getComponent(cam->scene, engine::Transform, cam->id);
    if (!t) return;
    t->pos[0] = x;
    t->pos[1] = y;
    t->pos[2] = z;
    engine::transformActivate(cam->scene, cam->id);
}

void AzgaarStreamingSystem::added() {
    streamWorld = loadingAzgaarGetWorld();
    if (!streamWorld) {
        utils::warn("azgaarStreaming: no retained world available; streaming disabled");
        return;
    }
    // Build the vegetation / landmark system (merged species meshes, road hash,
    // scatter pool).  Tiles scatter lazily as the active heightmap fills.
    azgaarPropsInit(streamWorld);
    utils::info("azgaarStreaming: started");
}

void AzgaarStreamingSystem::removed() {
    azgaarPropsDestroy();
    streamWorld = nullptr;
}

void AzgaarStreamingSystem::update() {
    // Drive the props scatter (polls the active heightmap for READY tiles,
    // enqueues deterministic scatter jobs, pushes wind each frame).
    azgaarPropsUpdate();

    // Terrain debug toggles for the heightmap pass.
    //   Ctrl+W : wireframe overlay
    //   Ctrl+H : debug height ramp
    if (engine::input.pressed == KEY_W && engine::input.ctrl) {
        engine::vulkanHeightmapTerrainSetWireFrameEnabled(!engine::vulkanHeightmapTerrainIsWireFrameEnabled());
        utils::info("terrain wireframe toggled (heightmap)");
    }

    if (engine::input.pressed == KEY_H && engine::input.ctrl) {
        bool enabled = !engine::vulkanHeightmapTerrainIsDebugHeightRampEnabled();
        engine::vulkanHeightmapTerrainSetDebugHeightRampEnabled(enabled);
        utils::info("heightmap debug height ramp %s", enabled ? "enabled" : "disabled");
    }

    if (!streamWorld) return;
    tempCameraTeleport();
}}  // namespace game
