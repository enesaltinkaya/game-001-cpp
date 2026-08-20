#include "VulkanDecalPass.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "renderer/decal/Decal.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanBuffer.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"

#define MAX_GPU_DECALS 8192u

typedef struct DecalGpu {
    mat4 model;
    mat4 invModel;
    vec4 color;
    vec4 params0; // opacity, normalThreshold, edgeFeather, time
    vec4 params1; // uvScale.xy, unused, unused
    u32  params2[4]; // flags, textureId, unused, unused
} DecalGpu;

typedef struct DecalPushConstants {
    u64 decalsAddress;
    u32 decalCount;
    u32 depthIndex;
    u32 normalsIndex;
    u32 width;
    u32 height;
} DecalPushConstants;

typedef struct RoadCompositePushConstants {
    u32 roadLayerIndex;
    u32 sceneColorIndex;
    u32 width;
    u32 height;
} RoadCompositePushConstants;

static void added(void);
static void preUpdate(void);
static void update(void);
static void postUpdate(void);
static void removed(void);

System vulkanDecalPass = {
    .name       = "decal",
    .added      = added,
    .preUpdate  = preUpdate,
    .update     = update,
    .postUpdate = postUpdate,
    .removed    = removed,
};

static VulkanPipe   pipeline;
static VulkanPipe   roadPipeline;
static VulkanPipe   roadCompositePipe;
static VulkanBuffer decalBuffer[FRAMES_IN_FLIGHT];
static double       elapsedCPU;
static double       elapsedGPU;
static u32          bufferIndex;

static void added(void) {
    pipeline = vulkanCreatePipe(.name = "decal",
                                .vs = "shaders/pass/decal/spv/decal.vert.spv",
                                .fs = "shaders/pass/decal/spv/decal.frag.spv",
                                .colorFormat1 = VK_FORMAT_R16G16B16A16_SFLOAT,
                                .clearColor1 = {0, 0, 0, 0},
                                .clearColor1Enabled = 0,
                                .blend = 1,
                                .blendPreserveAlpha = 1,
                                .noCull = 1);
    // Road decals render into a dedicated RGBA16F layer with a "union" blend
    // (replace RGB / MAX alpha) so overlapping junction rectangles don't
    // accumulate opacity. Reuses the same decal shaders; only the blend and
    // the (cleared) color attachment differ.
    roadPipeline = vulkanCreatePipe(.name = "decal_road",
                                    .vs = "shaders/pass/decal/spv/decal.vert.spv",
                                    .fs = "shaders/pass/decal/spv/decal.frag.spv",
                                    .colorFormat1 = VK_FORMAT_R16G16B16A16_SFLOAT,
                                    .clearColor1 = {0, 0, 0, 0},
                                    .clearColor1Enabled = 1,
                                    .blendRoad = 1,
                                    .noCull = 1);
    // Folds the road layer onto the scene color via a storage image
    // (sampling the live render target inside the raster pass is a feedback
    // loop), so it must run as a separate compute dispatch.
    roadCompositePipe = vulkanCreatePipe(.name = "decal_road_composite",
                                         .comp = "shaders/pass/decal/spv/road_composite.comp.spv");
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        decalBuffer[i] = vulkanCreateCpuToGpuBuffer("decal instances", sizeof(DecalGpu) * MAX_GPU_DECALS, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    }
}

static void preUpdate(void) {
    vulkanResetProfile(vulkan.currentCmd, &pipeline.profile, 0);
}

static bool decalVisibleToCamera(const DecalInstance* d, const Camera* camera, bool persistent) {
    if (!camera) return true;

    float radius = sqrtf(d->halfExtents[0] * d->halfExtents[0] +
                         d->halfExtents[1] * d->halfExtents[1] +
                         d->halfExtents[2] * d->halfExtents[2]);

    vec3 center = {d->position[0], d->position[1], d->position[2]};
    for (u32 p = 0; p < 6u; ++p) {
        const vec4* plane = &camera->cameraUbo.frustumPlanes[p];
        float dist = (*plane)[0] * center[0] + (*plane)[1] * center[1] + (*plane)[2] * center[2] + (*plane)[3];
        if (dist < -radius) return false;
    }

    // Persistent Azgaar road/trail decals are numerous. Until the decal pass is
    // changed to rasterize decal volumes/tile lists, keep only nearby road
    // projectors in the per-pixel compute loop.
    bool looksLikeRoad = (d->flags & DECAL_FLAG_ROAD) != 0;
    if (persistent && looksLikeRoad) {
        float dx = center[0] - camera->cameraUbo.renderLocation[0];
        float dz = center[2] - camera->cameraUbo.renderLocation[2];
        float maxDistance = 800.0f + radius;
        if (dx * dx + dz * dz > maxDistance * maxDistance) return false;
    }

    return true;
}

static void appendDecals(DecalGpu* dst, u32* count, const DecalInstance* src, size_t srcCount, const Camera* camera, bool persistent, bool roadsOnly) {
    for (size_t i = 0; i < srcCount && *count < MAX_GPU_DECALS; ++i) {
        const DecalInstance* d = &src[i];
        if (!decalVisibleToCamera(d, camera, persistent)) continue;
        bool isRoad = (d->flags & DECAL_FLAG_ROAD) != 0;
        if (roadsOnly != isRoad) continue;
        mat4 t, r, s, m;
        vec3 pos = {d->position[0], d->position[1], d->position[2]};
        versor rot = {d->rotation[0], d->rotation[1], d->rotation[2], d->rotation[3]};
        glm_translate_make(t, pos);
        glm_quat_mat4(rot, r);
        glm_scale_make(s, (vec3){d->halfExtents[0], d->halfExtents[1], d->halfExtents[2]});
        glm_mat4_mul(t, r, m);
        glm_mat4_mul(m, s, m);
        glm_mat4_copy(m, dst[*count].model);
        glm_mat4_inv(m, dst[*count].invModel);
        glm_vec4_copy((vec4){d->color[0], d->color[1], d->color[2], d->color[3]}, dst[*count].color);
        dst[*count].params0[0] = d->opacity;
        dst[*count].params0[1] = d->normalThreshold;
        dst[*count].params0[2] = d->edgeFeather;
        dst[*count].params0[3] = d->time;
        dst[*count].params1[0] = d->uvScale[0] == 0.0f ? 1.0f : d->uvScale[0];
        dst[*count].params1[1] = d->uvScale[1] == 0.0f ? 1.0f : d->uvScale[1];
        dst[*count].params1[2] = 0.0f;
        dst[*count].params1[3] = 0.0f;
        dst[*count].params2[0] = d->flags;
        dst[*count].params2[1] = d->textureId;
        dst[*count].params2[2] = 0u;
        dst[*count].params2[3] = 0u;
        ++*count;
    }
}

static void update(void) {
    elapsedCPU = nanos();
    elapsedGPU = 0;
    if (vulkan.skipFrame) { elapsedCPU = nanos() - elapsedCPU; return; }

    VulkanCommand* cmd = vulkan.currentCmd;
    VulkanImage* sceneColor = vulkanFrameResourcesGetSceneColor();
    VulkanImage* depth = vulkanFrameResourcesGetDepth();
    VulkanImage* normals = vulkanFrameResourcesGetNormals();
    if (!cmd || !sceneColor || !depth || !normals) { elapsedCPU = nanos() - elapsedCPU; return; }

    Entity* camEntity = cameraGetEntity();
    Camera* camera = camEntity ? getComponent(camEntity->scene, Camera, camEntity->id) : NULL;

    u32 fi = bufferIndex++ % FRAMES_IN_FLIGHT;
    DecalGpu* gpu  = static_cast<DecalGpu*>(decalBuffer[fi].vmaInfo.pMappedData);

    // Road decals are packed at the front of the instance buffer so they can be
    // drawn separately into a "union" layer, where overlapping junction
    // rectangles take MAX coverage instead of accumulating alpha (which would
    // darken forks/junctions). Non-road decals follow and are blended normally.
    size_t np = 0, nt = 0;
    const DecalInstance* persistent = decalGetPersistent(&np);
    const DecalInstance* transient = decalGetTransient(&nt);
    u32 count = 0;
    appendDecals(gpu, &count, persistent, np, camera, true, true);
    appendDecals(gpu, &count, transient, nt, camera, false, true);
    u32 roadCount = count;
    appendDecals(gpu, &count, persistent, np, camera, true, false);
    appendDecals(gpu, &count, transient, nt, camera, false, false);
    u32 otherCount = count - roadCount;
    decalClearTransient();
    if (!count) { elapsedCPU = nanos() - elapsedCPU; return; }

    vulkanTransition(cmd, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransition(cmd, normals, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    DecalPushConstants pc = {
        .decalsAddress = decalBuffer[fi].address,
        .decalCount = count,
        .depthIndex = (u32)depth->sampledPoolIndex,
        .normalsIndex = (u32)normals->sampledPoolIndex,
        .width = sceneColor->extent.width,
        .height = sceneColor->extent.height,
    };

    // --- Roads: render into the union layer, then composite onto sceneColor ---
    if (roadCount > 0) {
        VulkanImage* roadLayer = vulkanFrameResourcesGetRoadLayer();
        if (roadLayer) {
            vulkanTransition(cmd, roadLayer, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, 0, 1);
            vulkanBeginRender(.cmd = cmd, .pipe = &roadPipeline, .color1 = roadLayer);
            vulkanViewport(cmd, 0, roadLayer->extent.height, roadLayer->extent.width, -((i32)roadLayer->extent.height));
            vulkanScissor(cmd, 0, 0, roadLayer->extent.width, roadLayer->extent.height);
            vulkanBindPipe(cmd, &roadPipeline);
            vulkanPush(cmd, &roadPipeline, sizeof(pc), &pc);
            // Road instances occupy [0, roadCount) at the front of the buffer.
            vkCmdDraw(cmd->cmd, 36, roadCount, 0, 0);
            vulkanEndRender(cmd);

            // Composite the road layer over the scene. Roads sit underneath
            // blood/scorch and other transient decals, which are drawn next.
            vulkanTransition(cmd, roadLayer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
            vulkanTransition(cmd, sceneColor, VK_IMAGE_LAYOUT_GENERAL, 0, 1);
            vulkanBindPipe(cmd, &roadCompositePipe);
            RoadCompositePushConstants rpc = {
                .roadLayerIndex = (u32)roadLayer->sampledPoolIndex,
                .sceneColorIndex = (u32)sceneColor->storagePoolIndex,
                .width = sceneColor->extent.width,
                .height = sceneColor->extent.height,
            };
            vulkanPush(cmd, &roadCompositePipe, sizeof(rpc), &rpc);
            u32 groupsX = (sceneColor->extent.width + 7) / 8;
            u32 groupsY = (sceneColor->extent.height + 7) / 8;
            vulkanDispatch(cmd, &roadCompositePipe, groupsX, groupsY, 1);
        }
    }

    // --- Non-road decals: normal alpha blend over the road-composited scene ---
    if (otherCount > 0) {
        vulkanTransition(cmd, sceneColor, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, 0, 1);
        vulkanBeginProfile(cmd, &pipeline.profile, 0);
        vulkanBeginRender(.cmd = cmd, .pipe = &pipeline, .color1 = sceneColor);
        vulkanViewport(cmd, 0, sceneColor->extent.height, sceneColor->extent.width, -((i32)sceneColor->extent.height));
        vulkanScissor(cmd, 0, 0, sceneColor->extent.width, sceneColor->extent.height);
        vulkanBindPipe(cmd, &pipeline);
        vulkanPush(cmd, &pipeline, sizeof(pc), &pc);
        // Non-road instances start right after the road instances.
        vkCmdDraw(cmd->cmd, 36, otherCount, 0, roadCount);
        vulkanEndRender(cmd);
        vulkanEndProfile(cmd, &pipeline.profile, 0);
        elapsedGPU = pipeline.profile.elapsed;
    }

    vulkanTransition(cmd, sceneColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    elapsedCPU = nanos() - elapsedCPU;
}

static void postUpdate(void) {
    vulkanDecalPass.cpuElapsed = elapsedCPU;
    vulkanDecalPass.gpuElapsed = elapsedGPU;
}

static void removed(void) {
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) vulkanDestroyBuffer(&decalBuffer[i], NULL);
    vulkanDestroyPipe(&pipeline);
    vulkanDestroyPipe(&roadPipeline);
    vulkanDestroyPipe(&roadCompositePipe);
}
