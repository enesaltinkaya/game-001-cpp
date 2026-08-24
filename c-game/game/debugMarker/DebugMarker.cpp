#include "debugMarker/DebugMarker.h"

#include "ecs/Ecs.h"
#include "ecs/system/mesh/MeshComponent.h"
#include "ecs/system/scene/SceneSystem.h"
#include "ecs/system/transform/TransformComponent.h"
#include "ecs/system/transform/TransformSystem.h"
#include "renderer/Renderer.h"
#include "renderer/material/Material.h"
#include "renderer/material/MaterialManager.h"
#include "string/String.h"

// ── TEMPORARY debug marker ─────────────────────────────────────────────────
// White 50 cm cube at the requested world position. Delete this file (and the
// GameState registration) when the debugging session is over.

namespace game {

using namespace engine;

static Scene* markerScene;

static const vec3 markerPos  = {144.25f, 132.14f, -6246.13f};  // +0.25: unit cube origin is its center
static const float markerSize = 0.5f;                             // 50 cm

DebugMarkerSystem debugMarkerSystem;

DebugMarkerSystem::DebugMarkerSystem() : System("debugMarker") {}

// Materials are process-lifetime (createMaterial() terminates on a name
// collision and destroyMaterial() never unregisters), so build it once and
// reuse it across gameplay enter/exit cycles.
static Material* markerMaterial() {
    Material* material = getMaterialByName("debugMarker");
    if (material) return material;

    material = createMaterial("debugMarker");
    material->baseColor[0] = material->baseColor[1] = material->baseColor[2] = 1.0f;
    material->baseColor[3]                                                  = 1.0f;
    material->rmas[0] = 0.8f;  // roughness
    material->rmas[1] = 0.0f;  // metallic
    material->rmas[2] = 1.0f;
    material->rmas[3] = 1.0f;
    // No emissive: a self-lit marker adds a flat +1.0 white in scene.frag
    // (color += emissive) which dominates and hides the ambient light — this
    // cube exists to READ indirect-light bleed, so it must be pure albedo.
    material->featureMask |= (1u << MAT_ALPHA_OPAQUE);
    material->refCount++;

    rendererUploadMaterial(material);
    return material;
}

// Unit cube (±0.5) with flat per-face normals, CCW front faces
// (the pipelines use VK_FRONT_FACE_COUNTER_CLOCKWISE + back-face culling).
static const float cubePositions[24][3] = {
    // +X
    { 0.5f, -0.5f, -0.5f}, { 0.5f,  0.5f, -0.5f}, { 0.5f,  0.5f,  0.5f}, { 0.5f, -0.5f,  0.5f},
    // -X
    {-0.5f, -0.5f,  0.5f}, {-0.5f,  0.5f,  0.5f}, {-0.5f,  0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f},
    // +Y
    {-0.5f,  0.5f, -0.5f}, {-0.5f,  0.5f,  0.5f}, { 0.5f,  0.5f,  0.5f}, { 0.5f,  0.5f, -0.5f},
    // -Y
    {-0.5f, -0.5f,  0.5f}, {-0.5f, -0.5f, -0.5f}, { 0.5f, -0.5f, -0.5f}, { 0.5f, -0.5f,  0.5f},
    // +Z
    {-0.5f, -0.5f,  0.5f}, { 0.5f, -0.5f,  0.5f}, { 0.5f,  0.5f,  0.5f}, {-0.5f,  0.5f,  0.5f},
    // -Z
    { 0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}, {-0.5f,  0.5f, -0.5f}, { 0.5f,  0.5f, -0.5f},
};

static const float cubeNormals[6][3] = {
    { 1.0f,  0.0f,  0.0f}, {-1.0f,  0.0f,  0.0f}, { 0.0f,  1.0f,  0.0f},
    { 0.0f, -1.0f,  0.0f}, { 0.0f,  0.0f,  1.0f}, { 0.0f,  0.0f, -1.0f},
};

void DebugMarkerSystem::added() {
    markerScene           = new Scene{};
    markerScene->alwaysVisible = true;
    utils::stringPrintf(&markerScene->name, "debugMarker");
    ecs.scenes.push_back(markerScene);

    Entity* entity = createEntity(markerScene, "debug_marker_cube");
    Transform* t   = createComponent(markerScene, Transform, entity->id);
    glm_quat_identity(t->rot);
    t->pos[0] = markerPos[0];
    t->pos[1] = markerPos[1];
    t->pos[2] = markerPos[2];
    t->pos[3] = markerSize;  // unit cube → 50 cm

    Mesh* mesh = createComponentT(markerScene, Mesh, entity->id);
    mesh->instances.push_back(InstanceData{.entity = entity->id});
    mesh->aabbLocal[0][0] = mesh->aabbLocal[0][1] = mesh->aabbLocal[0][2] = -0.5f;
    mesh->aabbLocal[1][0] = mesh->aabbLocal[1][1] = mesh->aabbLocal[1][2] = 0.5f;

    Primitive prim     = {};
    prim.vertexCount   = 24;
    prim.indexCount    = 36;
    prim.materialId    = markerMaterial()->id;
    prim.attributeMask = (1u << cgltf_attribute_type_position) | (1u << cgltf_attribute_type_normal);

    prim.positions.reserve(prim.vertexCount * 3);
    for (u32 v = 0; v < prim.vertexCount; v++) {
        prim.positions.push_back(cubePositions[v][0]);
        prim.positions.push_back(cubePositions[v][1]);
        prim.positions.push_back(cubePositions[v][2]);
    }

    for (u32 face = 0; face < 6; face++) {
        u32 base = face * 4;
        u32 faceIndices[6] = {base + 0, base + 1, base + 2, base + 0, base + 2, base + 3};
        for (u32 i = 0; i < 6; i++) prim.indices.push_back(faceIndices[i]);
    }

    // Normals are stored SNORM16x3 (+2 pad bytes) per vertex, exactly what
    // vulkanSceneCreate()'s unpackNormal() expects (stride 8).
    prim.attributes[cgltf_attribute_type_normal].resize(prim.vertexCount * 8);
    for (u32 v = 0; v < prim.vertexCount; v++) {
        const float* n        = cubeNormals[v / 4];
        i16* dst              = reinterpret_cast<i16*>(prim.attributes[cgltf_attribute_type_normal].data() + v * 8);
        dst[0]                = static_cast<i16>(n[0] * 32767.0f);
        dst[1]                = static_cast<i16>(n[1] * 32767.0f);
        dst[2]                = static_cast<i16>(n[2] * 32767.0f);
    }

    mesh->primitives.push_back(prim);

    transformActivateAndSaveLastSubtree(markerScene, entity->id);

    rendererSceneCreate(markerScene);
    markerScene->ready = true;

    utils::info("debugMarker: %.2fm white cube at (%.2f, %.2f, %.2f)",
                markerSize,
                markerPos[0],
                markerPos[1],
                markerPos[2]);
}

void DebugMarkerSystem::removed() {
    if (!markerScene) return;
    rendererSceneDestroy(markerScene);
    sceneDestroy(markerScene);
    markerScene = nullptr;
}
}  // namespace game
