#include "Utils.h"
#include <algorithm>
#include <vector>
#include "ecs/system/scene/SceneSystem.h"
#include "ecs/system/System.h"
#include "ecs/system/transform/TransformSystem.h"
#include "ecs/system/animation/AnimationSystem.h"
#include "events/Events.h"
#include "futuretask/FutureTask.h"
#include "ecs/Ecs.h"
#include "ecs/system/lua/LuaSystem.h"
#include "ecs/system/sound/SoundSystem.h"
#include "ecs/system/window/WindowSystem.h"
#include "ecs/system/physics/PhysicsSystem.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/light/LightSystem.h"
#include "ecs/system/mesh/MeshSystem.h"
#include "ecs/system/heightmap/HeightmapTerrain.h"
#include "renderer/Renderer.h"
#include "timer/Timer.h"

namespace engine {
struct Ecs ecs;

void ecsInit(System* gameSystem) {
    utils::info("ecs: initializing");
    static Scene scene = {};
    ecs.defaultScene   = &scene;
    utils::stringPrintf(&ecs.defaultScene->name, "defaultScene");

    /*
     * System order matters.
     *
     * Update phase:
     * - window/lua/sound handle platform and auxiliary work
     * - game/physics/animation/transform update world state
     * - camera/light finalize render-facing state from the updated world
     *
     * Post-update phase:
     * - sceneSystem performs coarse culling for the current frame
     * - renderSystem runs last and performs the actual backend render
     *
     * Scene/terrain culling must not happen before transforms and camera are final,
     * and rendering must remain the last step of the ECS frame.
     */
    systemAddNow(1, &windowSystem);
    systemAddNow(2, &luaSystem);
    systemAddNow(3, &soundSystem);
    systemAddNow(1000, gameSystem);
    systemAddNow(2000, &pyhsicsSystem);
    systemAddNow(3000, &animationSystem);
    systemAddNow(4000, &transformSystem);
    systemAddNow(5000, &cameraSystem);
    systemAddNow(6000, &sceneSystem);
    systemAddNow(6500, &heightmapTerrainSystem);
    systemAddNow(8000, &lightSystem);
    systemAddNow(9000, &meshSystem);
    systemAddNow(10000, &renderSystem);

    utils::signalEmit("ecsInitialized", nullptr);
}

void ecsDestroy(void) {
    // Destroy systems in reverse order so the renderer shuts down before
    // the window system (SDL_Quit closes the X11 display, which the AMD
    // Vulkan driver needs for swapchain destruction).
    for (i32 i = (i32)static_cast<i32>(ecs.systems.size()) - 1; i >= 0; i--) {
        System* system = ecs.systems[i];
        utils::warn("remove system: %s", system->name);
        system->removed();
    }

    // Free CPU-side scene data after all systems have cleaned up.
    // GPU scene resources are already freed by the renderer's removed().
    for (i32 i = (i32)static_cast<i32>(ecs.scenes.size()) - 1; i >= 0; i--) {
        sceneDestroy(ecs.scenes[i]);
    }
}

void ecsPreUpdate(void) {  // runs every frame
    double now              = utils::nanos();
    ecs.totalCpuElapsed     = ecs.totalCpuElapsedTemp;
    ecs.totalCpuElapsedTemp = now;
    for (System* system : ecs.systems) {
        systemPreUpdate(system);
    }
}

static void ecsUpdateForTimer(void) {
    for (System* system : ecs.systems) {
        systemUpdate(system);
    }
}

void ecsUpdate(void) {  // might not run every frame, might run multiple times per frame
    utils::timerUpdate(ecsUpdateForTimer);
}

void ecsPostUpdate(void) {  // runs every frame
    for (System* system : ecs.systems) {
        systemPostUpdate(system);
    }
    ecs.totalCpuElapsedTemp = utils::nanos() - ecs.totalCpuElapsedTemp;
}

/*
------------------------------------------
SYSTEM
------------------------------------------
*/

static void systemRemoveLater(void* pSystem) {
    System* system = static_cast<System*>(pSystem);
    for (i32 i = 0, si = static_cast<i32>(ecs.systems.size()); i < si; i++) {
        if (ecs.systems[i] == system) {
            ecs.systems.erase(ecs.systems.begin() + i);
            system->removed();
            break;
        }
    }
}

void systemRemove(System* system) {
    utils::futureTaskAdd(0, systemRemoveLater, system);
}

static bool systemIsRegistered(System* system) {
    for (i32 i = 0, si = static_cast<i32>(ecs.systems.size()); i < si; i++) {
        if (ecs.systems[i] == system) return true;
    }
    return false;
}

static void systemAddDelayed(void* pSystem) {
    System* system = static_cast<System*>(pSystem);
    // Idempotent add: re-entering a state that adds the same systems would
    // otherwise duplicate the entry (double added()/update()/removed()
    // lifetimes — a classic crash source).
    if (systemIsRegistered(system)) {
        utils::warn("ecs: system %s already registered, skipping duplicate add", system->name);
        return;
    }
    utils::info("ecs: adding system %s", system->name);
    ecs.systems.push_back(system);
    std::sort(ecs.systems.begin(), ecs.systems.end(), [](const System* a, const System* b) { return a->priority < b->priority; });
    system->added();
}

void systemAdd(int priority, System* system) {
    system->priority = priority;
    utils::futureTaskAdd(0, systemAddDelayed, system);
}

void systemAddNow(int priority, System* system) {
    system->priority = priority;
    // See systemAddDelayed: never register the same System twice.
    if (systemIsRegistered(system)) {
        utils::warn("ecs: system %s already registered, skipping duplicate add", system->name);
        return;
    }
    utils::info("ecs: adding system %s", system->name);
    ecs.systems.push_back(system);
    std::sort(ecs.systems.begin(), ecs.systems.end(), [](const System* a, const System* b) { return a->priority < b->priority; });
    system->added();
}
}  // namespace engine
