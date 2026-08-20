#include "ecs/system/terrain/TerrainSystem.h"
#include "ecs/system/terrain/Terrain.h"
#include "ecs/Ecs.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"

static bool aabbOutsideFrustum(const vec3 min, const vec3 max, vec4* planes) {
    for (i32 i = 0; i < 6; i++) {
        vec4 p = {planes[i][0], planes[i][1], planes[i][2], planes[i][3]};
        vec3 positive = {
            p[0] >= 0.0f ? max[0] : min[0],
            p[1] >= 0.0f ? max[1] : min[1],
            p[2] >= 0.0f ? max[2] : min[2],
        };
        if ((p[0] * positive[0]) + (p[1] * positive[1]) + (p[2] * positive[2]) + p[3] < 0.0f) {
            return true;
        }
    }
    return false;
}

static void terrainSystemAdded(void) {}

static void terrainSystemRemoved(void) {}

static void terrainSystemPostUpdate(void) {
    Entity* cameraEntity = cameraGetEntity();
    Camera* camera = cameraEntity ? getComponent(cameraEntity->scene, Camera, cameraEntity->id)
                                  : NULL;
    vec4* frustumPlanes = camera ? (vec4*)camera->cameraUbo.frustumPlanes : NULL;
    vec3 origin = {0, 0, 0};

    foreach (Terrain* terrain, ecs.terrains) {
        if (!terrain) continue;

        bool terrainVisible = true;
        if (frustumPlanes) {
            vec3 cmin, cmax;
            glm_vec3_sub(terrain->boundsMin, origin, cmin);
            glm_vec3_sub(terrain->boundsMax, origin, cmax);
            terrainVisible = !aabbOutsideFrustum(cmin, cmax, frustumPlanes);
        }
        terrain->visible = terrainVisible;

        if (!terrainVisible) {
            foreachptr (TerrainChunk* chunk, terrain->chunks) {
                chunk->visible = false;
            }
            continue;
        }

        foreachptr (TerrainChunk* chunk, terrain->chunks) {
            vec3 cmin, cmax;
            glm_vec3_sub(chunk->boundsMin, origin, cmin);
            glm_vec3_sub(chunk->boundsMax, origin, cmax);
            chunk->visible = !aabbOutsideFrustum(cmin, cmax, frustumPlanes);
        }
    }
}

struct System terrainSystem = {
    .name       = "terrainSystem",
    .added      = terrainSystemAdded,
    .removed    = terrainSystemRemoved,
    .postUpdate = terrainSystemPostUpdate,
};
