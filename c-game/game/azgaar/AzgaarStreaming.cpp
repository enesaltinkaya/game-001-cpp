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

static void added(void);
static void removed(void);
static void update(void);

struct System azgaarStreamingSystem = {
    .name    = "azgaarStreaming",
    .added   = added,
    .removed = removed,
    .update  = update,
};

static const AzgaarWorld* streamWorld;

// TEMP DEBUG: ENGINE_CAM_TELEPORT="x,y,z,afterMs" teleports the player
// (and with them the camera + heightmap streaming window) after afterMs
// milliseconds — used to reach a specific biome/settlement/river for targeted
// screenshots.  Y is the desired camera height; the player is placed 40 m
// below it and physics settles it onto the terrain.
static void tempCameraTeleport(void) {
    static char* env = NULL;
    if (!env) env = getenv("ENGINE_CAM_TELEPORT");
    if (!env || !*env) return;
    static double startMs = -1;
    static char done      = 0;
    double now            = millies();
    if (startMs < 0) startMs = now;
    float x, y, z, afterMs;
    if (sscanf(env, "%f,%f,%f,%f", &x, &y, &z, &afterMs) != 4) return;
    if (done || now - startMs < afterMs) return;

    vec3 pos = {x, y - 40.0f, z};
    if (playerTeleportTo(pos)) {
        done = 1;
        info("TEMP player teleport -> (%.0f,%.0f,%.0f)", (double)x, (double)y, (double)z);
    }

    // Also snap the camera itself so the view (and the streaming window)
    // re-centers immediately instead of waiting for the controller.
    Entity* cam = cameraGetEntity();
    if (!cam) return;
    Transform* t = getComponent(cam->scene, Transform, cam->id);
    if (!t) return;
    t->pos[0] = x;
    t->pos[1] = y;
    t->pos[2] = z;
    transformActivate(cam->scene, cam->id);
}

static void added(void) {
    streamWorld = loadingAzgaarGetWorld();
    if (!streamWorld) {
        warn("azgaarStreaming: no retained world available; streaming disabled");
        return;
    }
    // Build the vegetation / landmark system (merged species meshes, road hash,
    // scatter pool).  Tiles scatter lazily as the active heightmap fills.
    azgaarPropsInit(streamWorld);
    info("azgaarStreaming: started");
}

static void removed(void) {
    azgaarPropsDestroy();
    streamWorld = NULL;
}

static void update(void) {
    // Drive the props scatter (polls the active heightmap for READY tiles,
    // enqueues deterministic scatter jobs, pushes wind each frame).
    azgaarPropsUpdate();

    // Terrain debug toggles for the heightmap pass.
    //   Ctrl+W : wireframe overlay
    //   Ctrl+H : debug height ramp
    if (input.pressed == KEY_W && input.ctrl) {
        vulkanHeightmapTerrainSetWireFrameEnabled(!vulkanHeightmapTerrainIsWireFrameEnabled());
        info("terrain wireframe toggled (heightmap)");
    }

    if (input.pressed == KEY_H && input.ctrl) {
        bool enabled = !vulkanHeightmapTerrainIsDebugHeightRampEnabled();
        vulkanHeightmapTerrainSetDebugHeightRampEnabled(enabled);
        info("heightmap debug height ramp %s", enabled ? "enabled" : "disabled");
    }

    if (!streamWorld) return;
    tempCameraTeleport();
}