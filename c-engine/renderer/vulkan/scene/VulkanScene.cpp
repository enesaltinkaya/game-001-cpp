#include "VulkanScene.h"
#include "ecs/Ecs.h"
#include "ecs/system/scene/Scene.h"
#include "ecs/system/mesh/MeshComponent.h"
#include "ecs/system/transform/TransformComponent.h"
#include "ecs/components/Skin.h"
#include "ecs/system/transform/TransformSystem.h"
#include "timer/Timer.h"
#include "renderer/material/Material.h"
#include "renderer/material/MaterialManager.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include <string.h>
#include <math.h>

#define ENTITY_SKIN_NOT_SKINNED 0xFFFFFFFF

namespace engine {
static void unpackNormal(const u8* src, float out[3]) {
    const int16_t* n = (const int16_t*)src;
    out[0]           = (float)n[0] / 32767.0f;
    out[1]           = (float)n[1] / 32767.0f;
    out[2]           = (float)n[2] / 32767.0f;
}

static void unpackTangent(const u8* src, float out[4]) {
    const int8_t* t = (const int8_t*)src;
    out[0]          = (float)t[0] / 127.0f;
    out[1]          = (float)t[1] / 127.0f;
    out[2]          = (float)t[2] / 127.0f;
    out[3]          = (float)t[3] / 127.0f;
}

static void unpackUV(const u8* src, float out[2]) {
    const uint16_t* uv = (const uint16_t*)src;
    out[0]             = (float)uv[0] / 65535.0f;
    out[1]             = (float)uv[1] / 65535.0f;
}

// Compute a tight bounding sphere for a set of positions
static void computeBoundingSphere(const float* positions, u32 vertexCount, float outSphere[4]) {
    if (vertexCount == 0) {
        outSphere[0] = outSphere[1] = outSphere[2] = outSphere[3] = 0.0f;
        return;
    }

    // Compute centroid
    double cx = 0.0, cy = 0.0, cz = 0.0;
    for (u32 i = 0; i < vertexCount; i++) {
        cx += positions[i * 3 + 0];
        cy += positions[i * 3 + 1];
        cz += positions[i * 3 + 2];
    }
    cx /= vertexCount;
    cy /= vertexCount;
    cz /= vertexCount;

    // Find maximum distance from centroid
    float maxDist2 = 0.0f;
    for (u32 i = 0; i < vertexCount; i++) {
        float dx    = positions[i * 3 + 0] - (float)cx;
        float dy    = positions[i * 3 + 1] - (float)cy;
        float dz    = positions[i * 3 + 2] - (float)cz;
        float dist2 = dx * dx + dy * dy + dz * dz;
        if (dist2 > maxDist2) maxDist2 = dist2;
    }

    outSphere[0] = (float)cx;
    outSphere[1] = (float)cy;
    outSphere[2] = (float)cz;
    outSphere[3] = sqrtf(maxDist2);
}

void vulkanSceneCreate(Scene* scene) {
    VulkanScene* vs = new VulkanScene{};
    scene->backendScene = vs;

    utils::SparseSet* meshes = getComponents(scene, Mesh);
    if (!meshes) return;

    // First pass: count totals
    u32 totalVertices = 0;
    u32 totalIndices  = 0;
    u32 totalDraws    = 0;

    for (u32 i = 0; i < meshes->size; i++) {
        Mesh* mesh             = static_cast<Mesh*>(utils::ssGetDataByIndex(meshes, i));
        u32 meshInstanceCount = static_cast<i32>(mesh->instances.size());
        for (u32 p = 0; p < mesh->primitives.size(); p++) {
            Primitive* prim = &mesh->primitives[p];
            totalVertices += prim->vertexCount;
            totalIndices += prim->indexCount;
            totalDraws += meshInstanceCount;
        }
    }

    if (totalVertices == 0) return;

    vs->totalVertices = totalVertices;
    vs->totalIndices  = totalIndices;
    vs->totalDraws    = totalDraws;

    // Allocate temp CPU buffers
    std::vector<SceneVertex> tempVertices(totalVertices);
    std::vector<u32> tempIndices(totalIndices);
    std::vector<GpuDrawInstance> tempDraws(totalDraws);
    std::vector<u32> tempDrawVertexCounts(totalDraws);

    SceneVertex* currVert = tempVertices.data();
    u32* currIdx          = tempIndices.data();
    u32 vertexOffset      = 0;
    u32 indexOffset       = 0;
    u32 drawIndex         = 0;

    // Second pass: pack vertices, indices, and build draw instance list
    for (u32 i = 0; i < meshes->size; i++) {
        Mesh* mesh             = static_cast<Mesh*>(utils::ssGetDataByIndex(meshes, i));
        u32 meshInstanceCount = static_cast<i32>(mesh->instances.size());

        for (u32 p = 0; p < mesh->primitives.size(); p++) {
            Primitive* prim = &mesh->primitives[p];

            // Pack vertices
            for (u32 v = 0; v < prim->vertexCount; v++) {
                SceneVertex sv = {};
                sv.position[0] = prim->positions[v * 3 + 0];
                sv.position[1] = prim->positions[v * 3 + 1];
                sv.position[2] = prim->positions[v * 3 + 2];

                if (prim->attributeMask & (1 << cgltf_attribute_type_normal)) {
                    unpackNormal(prim->attributes[cgltf_attribute_type_normal].data() + v * 8, sv.normal);
                }
                if (prim->attributeMask & (1 << cgltf_attribute_type_tangent)) {
                    unpackTangent(prim->attributes[cgltf_attribute_type_tangent].data() + v * 4,
                                  sv.tangent);
                }
                if (prim->attributeMask & (1 << cgltf_attribute_type_texcoord)) {
                    unpackUV(prim->attributes[cgltf_attribute_type_texcoord].data() + v * 4, sv.uv);
                }
                if (prim->attributeMask & (1 << cgltf_attribute_type_joints)) {
                    memcpy(&sv.joints, prim->attributes[cgltf_attribute_type_joints].data() + v * 4, 4);
                }
                if (prim->attributeMask & (1 << cgltf_attribute_type_weights)) {
                    memcpy(&sv.weights, prim->attributes[cgltf_attribute_type_weights].data() + v * 4, 4);
                }

                *currVert++ = sv;
            }

            // Copy indices (no global offset needed — we use vertexOffset per draw)
            memcpy(currIdx, prim->indices.data(), prim->indexCount * sizeof(u32));
            currIdx += prim->indexCount;

            // Compute bounding sphere for this primitive
            float boundingSphere[4];
            computeBoundingSphere(prim->positions.data(), prim->vertexCount, boundingSphere);

            // Check material for double-sided / transparent
            bool isSkinned = (prim->attributeMask & (1 << cgltf_attribute_type_joints)) != 0;
            u32 flags     = 0;
            Material* mat = getMaterialById(prim->materialId);
            if (mat) {
                if (mat->featureMask & (1u << MAT_IS_DOUBLE_SIDED)) flags |= DRAW_FLAG_DOUBLE_SIDED;
                if (mat->featureMask & (1u << MAT_ALPHA_BLEND)) flags |= DRAW_FLAG_TRANSPARENT;
            }
            if (isSkinned) flags |= DRAW_FLAG_SKINNED;

            // Create one draw per entity instance
            for (u32 inst = 0; inst < meshInstanceCount; inst++) {
                GpuDrawInstance* draw = &tempDraws[drawIndex];
                draw->firstIndex      = indexOffset;
                draw->indexCount      = prim->indexCount;
                draw->vertexOffset    = (i32)vertexOffset;
                draw->entity          = mesh->instances[inst].entity;
                draw->materialId      = prim->materialId;
                draw->flags           = flags;
                draw->_pad0           = 0;
                draw->_pad1           = 0;
                memcpy(draw->boundingSphere, boundingSphere, sizeof(float) * 4);
                tempDrawVertexCounts[drawIndex] = prim->vertexCount;
                drawIndex++;
            }

            vertexOffset += prim->vertexCount;
            indexOffset += prim->indexCount;
        }
    }

    // Create GPU buffers
    // STORAGE usage: the brixelizer voxelizer binds vertex/index data as shader
    // storage buffers (plan pitfall #13) — no VRAM cost, one flag.
    vs->vertexBuffer =
        vulkanCreateGpuBuffer(utils::strtmp("SceneVBO %s", scene->name.data),
                              totalVertices * sizeof(SceneVertex),
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    vs->indexBuffer =
        vulkanCreateGpuBuffer(utils::strtmp("SceneIBO %s", scene->name.data),
                              totalIndices * sizeof(u32),
                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    vs->drawInstanceBuffer = vulkanCreateGpuBuffer(
        utils::strtmp("SceneDrawInst %s", scene->name.data),
        totalDraws * sizeof(GpuDrawInstance),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    // CPU-side copy of the draw list (brixelizer SDF registration, Step 3)
    vs->cpuDraws.resize(totalDraws);
    for (u32 i = 0; i < totalDraws; i++) {
        vs->cpuDraws[i].draw       = tempDraws[i];
        vs->cpuDraws[i].vertexCount = tempDrawVertexCounts[i];
    }

    // Upload geometry + draw instances
    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanCopy(.cmd         = cmd,
               .source.data = tempVertices.data(),
               .target.buf  = &vs->vertexBuffer,
               .size        = static_cast<u32>(totalVertices * sizeof(SceneVertex)));
    vulkanCopy(.cmd         = cmd,
               .source.data = tempIndices.data(),
               .target.buf  = &vs->indexBuffer,
               .size        = static_cast<u32>(totalIndices * sizeof(u32)));
    vulkanCopy(.cmd         = cmd,
               .source.data = tempDraws.data(),
               .target.buf  = &vs->drawInstanceBuffer,
               .size        = static_cast<u32>(totalDraws * sizeof(GpuDrawInstance)));
    vulkanTransientEnd(cmd, 0);

    // Per-frame buffers
    for (int f = 0; f < FRAMES_IN_FLIGHT; f++) {
        // Transform buffers
        vs->transformBuffer[f] =
            vulkanCreateCpuBuffer(utils::strtmp("SceneTransforms %i %s", f, scene->name.data),
                                  sizeof(Transform) * MAX_ENTITIES,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        vs->prevTransformBuffer[f] =
            vulkanCreateCpuBuffer(utils::strtmp("ScenePrevTransforms %i %s", f, scene->name.data),
                                  sizeof(Transform) * MAX_ENTITIES,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        memset(vs->transformBuffer[f].vmaInfo.pMappedData, 0, sizeof(Transform) * MAX_ENTITIES);
        memset(vs->prevTransformBuffer[f].vmaInfo.pMappedData, 0, sizeof(Transform) * MAX_ENTITIES);
        vs->transformUploads[f].clear();

        // Opaque single-sided culling output
        vs->indirectBuffer[f] = vulkanCreateGpuBuffer(
            utils::strtmp("SceneIndirect %i %s", f, scene->name.data),
            totalDraws * sizeof(SceneDrawIndexedCommand),
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        vs->drawCountBuffer[f] = vulkanCreateGpuBuffer(
            utils::strtmp("SceneDrawCount %i %s", f, scene->name.data),
            sizeof(u32),
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        vs->culledBuffer[f] = vulkanCreateGpuBuffer(
            utils::strtmp("SceneCulled %i %s", f, scene->name.data),
            totalDraws * sizeof(u32),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

        // Double-sided culling output
        vs->dsIndirectBuffer[f] = vulkanCreateGpuBuffer(
            utils::strtmp("SceneDSIndirect %i %s", f, scene->name.data),
            totalDraws * sizeof(SceneDrawIndexedCommand),
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        vs->dsDrawCountBuffer[f] = vulkanCreateGpuBuffer(
            utils::strtmp("SceneDSDrawCount %i %s", f, scene->name.data),
            sizeof(u32),
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        vs->dsCulledBuffer[f] = vulkanCreateGpuBuffer(
            utils::strtmp("SceneDSCulled %i %s", f, scene->name.data),
            totalDraws * sizeof(u32),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

        // Transparent culling output
        vs->transIndirectBuffer[f] = vulkanCreateGpuBuffer(
            utils::strtmp("SceneTransIndirect %i %s", f, scene->name.data),
            totalDraws * sizeof(SceneDrawIndexedCommand),
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        vs->transDrawCountBuffer[f] = vulkanCreateGpuBuffer(
            utils::strtmp("SceneTransDrawCount %i %s", f, scene->name.data),
            sizeof(u32),
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        vs->transCulledBuffer[f] = vulkanCreateGpuBuffer(
            utils::strtmp("SceneTransCulled %i %s", f, scene->name.data),
            totalDraws * sizeof(u32),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

        // Visibility buffer (per-draw flags for two-phase occlusion)
        vs->visibilityBuffer[f] = vulkanCreateGpuBuffer(
            utils::strtmp("SceneVisibility %i %s", f, scene->name.data),
            totalDraws * sizeof(u32),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

        // Phase 2 occlusion output: opaque single-sided
        vs->phase2IndirectBuffer[f] = vulkanCreateGpuBuffer(
            utils::strtmp("SceneP2Indirect %i %s", f, scene->name.data),
            totalDraws * sizeof(SceneDrawIndexedCommand),
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        vs->phase2DrawCountBuffer[f] = vulkanCreateGpuBuffer(
            utils::strtmp("SceneP2DrawCount %i %s", f, scene->name.data),
            sizeof(u32),
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        vs->phase2CulledBuffer[f] = vulkanCreateGpuBuffer(
            utils::strtmp("SceneP2Culled %i %s", f, scene->name.data),
            totalDraws * sizeof(u32),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

        // Phase 2 occlusion output: opaque double-sided
        vs->phase2DsIndirectBuffer[f] = vulkanCreateGpuBuffer(
            utils::strtmp("SceneP2DSIndirect %i %s", f, scene->name.data),
            totalDraws * sizeof(SceneDrawIndexedCommand),
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        vs->phase2DsDrawCountBuffer[f] = vulkanCreateGpuBuffer(
            utils::strtmp("SceneP2DSDrawCount %i %s", f, scene->name.data),
            sizeof(u32),
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        vs->phase2DsCulledBuffer[f] = vulkanCreateGpuBuffer(
            utils::strtmp("SceneP2DSCulled %i %s", f, scene->name.data),
            totalDraws * sizeof(u32),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

        // Stats readback: 8 bytes (drawCalls + triangleCount), host-visible
        vs->statsReadbackBuffer[f] = vulkanCreateReadbackBuffer(
            utils::strtmp("SceneStatsReadback %i %s", f, scene->name.data),
            sizeof(u32) * 2,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }

    utils::info("vulkanScene: created %s — %u verts, %u indices, %u draws",
         scene->name.data,
         totalVertices,
         totalIndices,
         totalDraws);

    vulkanSceneInitSkinning(scene);
}

void vulkanSceneDestroy(Scene* scene) {
    if (!scene || !scene->backendScene) return;
    VulkanScene* vs  = static_cast<VulkanScene*>(scene->backendScene);

    vulkanDestroyBuffer(&vs->vertexBuffer, VK_NULL_HANDLE);
    vulkanDestroyBuffer(&vs->indexBuffer, VK_NULL_HANDLE);
    vulkanDestroyBuffer(&vs->drawInstanceBuffer, VK_NULL_HANDLE);

    for (int f = 0; f < FRAMES_IN_FLIGHT; f++) {
        vulkanDestroyBuffer(&vs->transformBuffer[f], VK_NULL_HANDLE);
        vulkanDestroyBuffer(&vs->prevTransformBuffer[f], VK_NULL_HANDLE);

        vulkanDestroyBuffer(&vs->indirectBuffer[f], VK_NULL_HANDLE);
        vulkanDestroyBuffer(&vs->drawCountBuffer[f], VK_NULL_HANDLE);
        vulkanDestroyBuffer(&vs->culledBuffer[f], VK_NULL_HANDLE);

        vulkanDestroyBuffer(&vs->dsIndirectBuffer[f], VK_NULL_HANDLE);
        vulkanDestroyBuffer(&vs->dsDrawCountBuffer[f], VK_NULL_HANDLE);
        vulkanDestroyBuffer(&vs->dsCulledBuffer[f], VK_NULL_HANDLE);

        vulkanDestroyBuffer(&vs->transIndirectBuffer[f], VK_NULL_HANDLE);
        vulkanDestroyBuffer(&vs->transDrawCountBuffer[f], VK_NULL_HANDLE);
        vulkanDestroyBuffer(&vs->transCulledBuffer[f], VK_NULL_HANDLE);

        vulkanDestroyBuffer(&vs->visibilityBuffer[f], VK_NULL_HANDLE);

        vulkanDestroyBuffer(&vs->phase2IndirectBuffer[f], VK_NULL_HANDLE);
        vulkanDestroyBuffer(&vs->phase2DrawCountBuffer[f], VK_NULL_HANDLE);
        vulkanDestroyBuffer(&vs->phase2CulledBuffer[f], VK_NULL_HANDLE);

        vulkanDestroyBuffer(&vs->phase2DsIndirectBuffer[f], VK_NULL_HANDLE);
        vulkanDestroyBuffer(&vs->phase2DsDrawCountBuffer[f], VK_NULL_HANDLE);
        vulkanDestroyBuffer(&vs->phase2DsCulledBuffer[f], VK_NULL_HANDLE);

        vulkanDestroyBuffer(&vs->statsReadbackBuffer[f], VK_NULL_HANDLE);

        vulkanDestroyBuffer(&vs->jointMatrixBuffer[f], VK_NULL_HANDLE);
        vulkanDestroyBuffer(&vs->prevJointMatrixBuffer[f], VK_NULL_HANDLE);
        vulkanDestroyBuffer(&vs->entitySkinMapBuffer[f], VK_NULL_HANDLE);
    }

    delete vs;
    scene->backendScene = nullptr;
}

void vulkanSceneUploadTransform(Scene* scene, u32 entity, Transform* transform) {
    if (!scene || !scene->backendScene) return;
    VulkanScene* vulkanScene  = static_cast<VulkanScene*>(scene->backendScene);
    for (i32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        TransformUpload upload = {*transform, entity};
        vulkanScene->transformUploads[i].push_back(upload);
    }
}

void vulkanSceneInitSkinning(Scene* scene) {
    if (!scene || !scene->backendScene) return;
    VulkanScene* vs  = static_cast<VulkanScene*>(scene->backendScene);

    utils::SparseSet* skinSet = getComponents(scene, Skin);
    if (!skinSet || skinSet->size == 0) return;

    u32 totalJointCount         = 0;
    u32 totalSkinnedEntityCount = 0;
    for (u32 i = 0; i < skinSet->size; i++) {
        Skin* skin  = static_cast<Skin*>(utils::ssGetDataByIndex(skinSet, i));
        if (!skin) continue;

        u32 jointCount = static_cast<i32>(skin->joints.size());
        if (jointCount == 0) continue;

        if (jointCount > 255) {
            utils::warn(
                "Scene '%s' skin %u has %u joints, but vertex joints are uint8 indices; "
                "truncation/OOB reads may occur",
                scene->name.data,
                i,
                jointCount);
        }

        totalJointCount += jointCount;
        totalSkinnedEntityCount++;
    }

    vs->totalSkinJointCount     = totalJointCount;
    vs->totalSkinnedEntityCount = totalSkinnedEntityCount;

    if (totalJointCount == 0) return;

    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        vs->jointMatrixBuffer[i] =
            vulkanCreateCpuBuffer(utils::strtmp("JointMatrices %i %s", i, scene->name.data),
                                  (u64)totalJointCount * sizeof(mat4),
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

        vs->prevJointMatrixBuffer[i] =
            vulkanCreateCpuBuffer(utils::strtmp("PrevJointMatrices %i %s", i, scene->name.data),
                                  (u64)totalJointCount * sizeof(mat4),
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

        vs->entitySkinMapBuffer[i] =
            vulkanCreateCpuBuffer(utils::strtmp("EntitySkinMap %i %s", i, scene->name.data),
                                  (u64)MAX_ENTITIES * sizeof(u32),
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

        memset(vs->jointMatrixBuffer[i].vmaInfo.pMappedData,
               0,
               (u64)totalJointCount * sizeof(mat4));
        memset(vs->prevJointMatrixBuffer[i].vmaInfo.pMappedData,
               0,
               (u64)totalJointCount * sizeof(mat4));

        u32* skinMap  = static_cast<u32*>(vs->entitySkinMapBuffer[i].vmaInfo.pMappedData);
        for (u32 j = 0; j < MAX_ENTITIES; j++) {
            skinMap[j] = ENTITY_SKIN_NOT_SKINNED;
        }
    }
}

void vulkanSceneFlushJoints(Scene* scene) {
    if (!scene || !scene->backendScene) return;
    VulkanScene* vs  = static_cast<VulkanScene*>(scene->backendScene);

    int fi = renderer.flightIndex;

    if (vs->jointMatrixBuffer[fi].buf == VK_NULL_HANDLE ||
        vs->prevJointMatrixBuffer[fi].buf == VK_NULL_HANDLE ||
        vs->entitySkinMapBuffer[fi].buf == VK_NULL_HANDLE) {
        return;
    }

    utils::SparseSet* skinSet = getComponents(scene, Skin);
    if (!skinSet || skinSet->size == 0) return;

    mat4* jointMatrices      = static_cast<mat4*>(vs->jointMatrixBuffer[fi].vmaInfo.pMappedData);
    mat4* prevJointMatrices  = static_cast<mat4*>(vs->prevJointMatrixBuffer[fi].vmaInfo.pMappedData);
    u32* skinMap             = static_cast<u32*>(vs->entitySkinMapBuffer[fi].vmaInfo.pMappedData);

    // Previous-joint history from the PREVIOUS RENDER FRAME
    int previousFi = fi == 0 ? (FRAMES_IN_FLIGHT - 1) : (fi - 1);
    mat4* previousFrameJointMatrices  = static_cast<mat4*>(vs->jointMatrixBuffer[previousFi].vmaInfo.pMappedData);
    memcpy(prevJointMatrices,
           previousFrameJointMatrices,
           (u64)vs->totalSkinJointCount * sizeof(mat4));

    for (u32 i = 0; i < MAX_ENTITIES; i++) {
        skinMap[i] = ENTITY_SKIN_NOT_SKINNED;
    }

    u32 jointCursor = 0;
    for (u32 i = 0; i < skinSet->size; i++) {
        Skin* skin  = static_cast<Skin*>(utils::ssGetDataByIndex(skinSet, i));
        u32 entity = utils::ssGetValueByIndex(skinSet, i);
        if (!skin) continue;
        if (entity >= MAX_ENTITIES) {
            utils::warn("Scene '%s' skin entity %u exceeds MAX_ENTITIES", scene->name.data, entity);
            continue;
        }

        u32 jointCount = static_cast<i32>(skin->joints.size());
        u32 ibmCount   = static_cast<i32>(skin->inverseBindMatrices.size()) / 16;
        if (jointCount == 0) continue;

        if (ibmCount < jointCount) {
            utils::warn(
                "Scene '%s' skin entity %u has %u joints but only %u inverse bind matrices; "
                "clamping",
                scene->name.data,
                entity,
                jointCount,
                ibmCount);
            jointCount = ibmCount;
            if (jointCount == 0) continue;
        }

        if (jointCursor + jointCount > vs->totalSkinJointCount) {
            utils::warn("Scene '%s' skin entity %u overflows per-scene joint buffer (%u + %u > %u)",
                 scene->name.data,
                 entity,
                 jointCursor,
                 jointCount,
                 vs->totalSkinJointCount);
            jointCount =
                vs->totalSkinJointCount > jointCursor ? (vs->totalSkinJointCount - jointCursor) : 0;
            if (jointCount == 0) break;
        }

        skin->jointBufferCursor = jointCursor;
        skinMap[entity]         = jointCursor;

        for (u32 j = 0; j < jointCount; j++) {
            u32 jointEntity    = skin->joints[j];
            WorldTransform* wt = transformGetWorld(scene, jointEntity);
            LastTransform* lt  = getComponent(scene, LastTransform, jointEntity);
            mat4 worldMat;
            if (wt && lt) {
                Transform lerp = {};
                quatSlerpShortest(lt->rot, wt->rot, utils::timer.alpha, lerp.rot);
                glm_vec3_lerp(lt->pos, wt->pos, utils::timer.alpha, lerp.pos);
                lerp.pos[3] = glm_lerp(lt->pos[3], wt->pos[3], utils::timer.alpha);
                transformToMat4(&lerp, worldMat);
            } else if (wt) {
                transformToMat4((Transform*)wt, worldMat);
            } else {
                glm_mat4_identity(worldMat);
            }

            glm_mat4_mul(worldMat, reinterpret_cast<vec4*>(&skin->inverseBindMatrices[j * 16]), jointMatrices[jointCursor + j]);
        }

        jointCursor += jointCount;
    }
}

void vulkanSceneFlushTransforms(VulkanScene* vulkanScene) {
    int flight                        = renderer.flightIndex;
    VulkanBuffer* transformBuffer     = &vulkanScene->transformBuffer[flight];
    VulkanBuffer* prevTransformBuffer = &vulkanScene->prevTransformBuffer[flight];
    std::vector<TransformUpload> uploads    = vulkanScene->transformUploads[flight];

    if (transformBuffer->buf == VK_NULL_HANDLE || prevTransformBuffer->buf == VK_NULL_HANDLE) {
        vulkanScene->transformUploads[flight].clear();
        return;
    }

    Transform* current   = static_cast<Transform*>(transformBuffer->vmaInfo.pMappedData);
    Transform* previous  = static_cast<Transform*>(prevTransformBuffer->vmaInfo.pMappedData);

    for (i32 j = 0, sj = static_cast<i32>(uploads.size()); j < sj; j++) {
        TransformUpload* upload = &uploads[j];
        u32 entityId            = upload->entity;
        previous[entityId]      = current[entityId];
        current[entityId]       = upload->transform;
    }

    vulkanScene->transformUploads[flight].clear();
}
}  // namespace engine
