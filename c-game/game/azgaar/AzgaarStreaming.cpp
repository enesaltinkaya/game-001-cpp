#include "AzgaarStreaming.h"
#include "azgaar/AzgaarStreaming.h"
#include "azgaar/AzgaarWorld.h"
#include "azgaar/AzgaarProps.h"
#include "loadingAzgaar/LoadingAzgaar.h"
#include "logger/Logger.h"
#include "ecs/system/window/WindowSystem.h"
#include "ecs/Ecs.h"
#include "renderer/vulkan/pass/heightmap_terrain/VulkanHeightmapTerrainPass.h"

namespace game {

AzgaarStreamingSystem azgaarStreamingSystem;

AzgaarStreamingSystem::AzgaarStreamingSystem() : engine::System("azgaarStreaming") {}

static const AzgaarWorld* streamWorld;

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
}}  // namespace game
