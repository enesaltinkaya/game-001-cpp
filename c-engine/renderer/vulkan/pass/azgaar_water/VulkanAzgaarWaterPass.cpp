#include "renderer/vulkan/pass/azgaar_water/VulkanAzgaarWaterPass.h"
#include "renderer/Renderer.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/resources/VulkanBuffer.h"
#include "renderer/vulkan/scene/VulkanScene.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "thread/Thread.h"

static void added(void);
static void preUpdate(void);
static void update(void);
static void removed(void);

struct System vulkanAzgaarWaterPass = {
    .name      = "azgaar_water",
    .added     = added,
    .preUpdate = preUpdate,
    .update    = update,
    .removed   = removed,
};

// Must match the GLSL push-constant block in azgaar_water.frag.
typedef struct WaterPushConstants {
    u32 depthIndex;  // index of the scene depth image in the global sampled pool
    u32 width;
    u32 height;
    float nearZ;
    float farZ;
    float projM00;
    float projM11;
} WaterPushConstants;

static VulkanPipe pipe;
static VulkanBuffer vertexBuffer;
static VulkanBuffer indexBuffer;
static u32 vertexCount = 0;
static u32 indexCount = 0;
static bool uploaded = false;

// Pending upload (set on game thread, consumed on render thread)
static Thread uploadLock = {.mutex = PTHREAD_MUTEX_INITIALIZER};
static SceneVertex* pendingVertexData = NULL;
static u32* pendingIndexData = NULL;
static u32 pendingVertexCount = 0;
static u32 pendingIndexCount = 0;
static bool pendingClear = false;

static VkVertexInputBindingDescription vertexBinding = {
    .binding   = 0,
    .stride    = sizeof(SceneVertex),
    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
};

static VkVertexInputAttributeDescription vertexAttrs[] = {
    {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0},
    {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 12},
    {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 24},
    {.location = 3, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 40},
};

static void recreatePipelines(void) {
    if (pipe.pipe) vulkanDestroyPipe(&pipe);

    // The water pass is a pure color pass: no depth attachment.  The
    // scene depth buffer is sampled as a texture in the fragment shader to
    // recover the terrain height below the water (used for the real water
    // depth and the shore foam band), so the waterline is no longer a hard
    // depth-test cutoff — it becomes a soft, noise-modulated foam edge.
    pipe = vulkanCreatePipe(
        .name                 = "azgaar_water",
        .vs                   = "shaders/pass/azgaar_water/spv/azgaar_water.vert.spv",
        .fs                   = "shaders/pass/azgaar_water/spv/azgaar_water.frag.spv",
        .colorFormat1         = VK_FORMAT_R16G16B16A16_SFLOAT,
        .noCull               = 1,
        .blend                = 1,
        .clearColor1          = {0, 0, 0, 0}, .clearColor1Enabled = 0,
        .clearDepth           = {0, 0}, .clearDepthEnabled = 0,
        .vertexAttributes     = vertexAttrs,
        .vertexAttributeCount = 4,
        .vertexBindings       = &vertexBinding,
        .vertexBindingCount   = 1);
}

static void swapchainCreated(void*) {
    recreatePipelines();
}

void vulkanAzgaarWaterSetMesh(const void* vertices, u32 vCount,
                               const void* indices, u32 iCount) {
    threadLock(&uploadLock);
    // Free any existing pending data
    if (pendingVertexData) { memoryFree(pendingVertexData); pendingVertexData = NULL; }
    if (pendingIndexData) { memoryFree(pendingIndexData); pendingIndexData = NULL; }
    pendingVertexCount = pendingIndexCount = 0;
    pendingClear = false;

    pendingVertexCount = vCount;
    pendingIndexCount = iCount;
    pendingVertexData  = static_cast<SceneVertex*>(memoryAlloc(sizeof(SceneVertex) * vCount));
    pendingIndexData  = static_cast<u32*>(memoryAlloc(sizeof(u32) * iCount));
    memcpy(pendingVertexData, vertices, sizeof(SceneVertex) * vCount);
    memcpy(pendingIndexData, indices, sizeof(u32) * iCount);
    threadUnlock(&uploadLock);
}

void vulkanAzgaarWaterClear(void) {
    threadLock(&uploadLock);
    pendingClear = true;
    if (pendingVertexData) { memoryFree(pendingVertexData); pendingVertexData = NULL; }
    if (pendingIndexData) { memoryFree(pendingIndexData); pendingIndexData = NULL; }
    pendingVertexCount = pendingIndexCount = 0;
    threadUnlock(&uploadLock);
}

bool vulkanAzgaarWaterGetGpuMesh(VulkanBuffer** outVertexBuffer,
                                  VulkanBuffer** outIndexBuffer,
                                  u32* outVertexCount,
                                  u32* outIndexCount) {
    threadLock(&uploadLock);
    bool ok = uploaded && vertexBuffer.buf && indexBuffer.buf;
    if (ok) {
        if (outVertexBuffer) *outVertexBuffer = &vertexBuffer;
        if (outIndexBuffer)  *outIndexBuffer  = &indexBuffer;
        if (outVertexCount)  *outVertexCount  = vertexCount;
        if (outIndexCount)   *outIndexCount   = indexCount;
    }
    threadUnlock(&uploadLock);
    return ok;
}

bool vulkanAzgaarWaterIsVisible(void) {
    VulkanWaterData water = vulkanResourceGetWaterData();
    if (water.enabled < 0.5f) return false;

    Entity* camEntity = cameraGetEntity();
    Camera* cam = camEntity ? getComponent(camEntity->scene, Camera, camEntity->id) : NULL;
    if (!cam) return false;

    // Camera underwater: the water grid would render between the camera
    // and the scene (e.g. the player), compositing a translucent film that
    // ghosts/tints the character.  No water should be drawn at all.
    float seaLevel = water.surfaceY[0];
    if (cam->cameraUbo.renderLocation[1] < seaLevel) return false;

    // The water is effectively an infinite horizontal plane at sea level
    // (the camera-following grid reaches the far plane).  The frustum is a
    // convex volume, so the plane is entirely outside it iff every frustum
    // corner lies strictly above the surface — e.g. flying high over inland
    // terrain: no water can be on screen, so the draws can be skipped.
    // Wave displacement can push the surface above sea level by up to the
    // summed wave amplitudes, so keep that as a margin to avoid culling a
    // visible crest at the screen edge.
    float waveMargin = 1.0f;
    for (int i = 0; i < 4; i++) waveMargin += water.waveDirAmp[i][2];

    static const vec4 ndcCorners[8] = {
        {-1, -1, 0, 1}, {1, -1, 0, 1}, {1, 1, 0, 1}, {-1, 1, 0, 1},
        {-1, -1, 1, 1}, {1, -1, 1, 1}, {1, 1, 1, 1}, {-1, 1, 1, 1},
    };
    for (int i = 0; i < 8; i++) {
        vec4 world;
        glm_mat4_mulv((vec4*)cam->cameraUbo.invViewProjection,
                      (float*)&ndcCorners[i], world);
        float y = world[1] / world[3];
        if (y < seaLevel + waveMargin) return true;
    }
    return false;
}

static void uploadPending(void) {
    threadLock(&uploadLock);
    bool hasClear = pendingClear;
    bool hasData = pendingVertexData && pendingIndexData && pendingVertexCount > 0 && pendingIndexCount > 0;
    if (!hasClear && !hasData) { threadUnlock(&uploadLock); return; }

    SceneVertex* verts = NULL;
    u32* idxs = NULL;
    u32 vCount = 0, iCount = 0;
    if (hasData) {
        verts = pendingVertexData;
        idxs = pendingIndexData;
        vCount = pendingVertexCount;
        iCount = pendingIndexCount;
        pendingVertexData = NULL;
        pendingIndexData = NULL;
        pendingVertexCount = pendingIndexCount = 0;
    }
    pendingClear = false;
    threadUnlock(&uploadLock);

    if (hasClear) {
        threadLock(&uploadLock);
        if (vertexBuffer.buf) vulkanDestroyBuffer(&vertexBuffer, VK_NULL_HANDLE);
        if (indexBuffer.buf) vulkanDestroyBuffer(&indexBuffer, VK_NULL_HANDLE);
        vertexCount = indexCount = 0;
        uploaded = false;
        threadUnlock(&uploadLock);
        return;
    }

    if (!verts || !idxs) {
        memoryFree(verts);
        memoryFree(idxs);
        return;
    }

    // Upload via transient command buffer (synchronous)
    VulkanBuffer vbuf = vulkanCreateGpuBuffer(strtmp("AzgaarWaterVBO"),
                                               vCount * sizeof(SceneVertex),
                                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VulkanBuffer ibuf = vulkanCreateGpuBuffer(strtmp("AzgaarWaterIBO"),
                                               iCount * sizeof(u32),
                                               VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanCopy(.cmd = cmd, .source.data = verts, .target.buf = &vbuf,
               .size = static_cast<u32>(vCount * sizeof(SceneVertex)));
    vulkanCopy(.cmd = cmd, .source.data = idxs, .target.buf = &ibuf,
               .size = static_cast<u32>(iCount * sizeof(u32)));
    vulkanTransientEnd(cmd, 1);

    threadLock(&uploadLock);
    if (vertexBuffer.buf) vulkanDestroyBuffer(&vertexBuffer, VK_NULL_HANDLE);
    if (indexBuffer.buf) vulkanDestroyBuffer(&indexBuffer, VK_NULL_HANDLE);
    vertexBuffer = vbuf;
    indexBuffer = ibuf;
    vertexCount = vCount;
    indexCount = iCount;
    uploaded = true;
    threadUnlock(&uploadLock);

    memoryFree(verts);
    memoryFree(idxs);
}

static void added(void) {
    signalSubscribe("swapchainCreated", swapchainCreated);
    recreatePipelines();
}

static void preUpdate(void) {
    uploadPending();
}

static void update(void) {
    if (vulkan.skipFrame) { return; }

    threadLock(&uploadLock);
    bool isUploaded = uploaded;
    VulkanBuffer vbufCopy = vertexBuffer;
    VulkanBuffer ibufCopy = indexBuffer;
    u32 idxCount = indexCount;
    threadUnlock(&uploadLock);
    if (!isUploaded || !vbufCopy.buf || !ibufCopy.buf || idxCount == 0) return;

    VulkanCommand* cmd = vulkan.currentCmd;
    if (!cmd) return;

    VulkanImage* sceneColor  = vulkanFrameResourcesGetSceneColor();
    VulkanImage* depthImage  = vulkanFrameResourcesGetDepth();
    if (!sceneColor || !depthImage) return;

    // Skip the whole pass when no water can be on screen: water disabled,
    // camera underwater, or the entire frustum above the sea plane.
    if (!vulkanAzgaarWaterIsVisible()) return;

    // The fragment shader linearizes the scene depth with the camera's
    // near/far and projection values.
    Entity* camEntity = cameraGetEntity();
    Camera* cam       = camEntity ? getComponent(camEntity->scene, Camera, camEntity->id) : NULL;
    if (!cam) return;

    // The depth buffer is no longer a render attachment here, so it can be
    // sampled as a texture.  Ensure it is in SHADER_READ_ONLY_OPTIMAL.
    vulkanTransition(cmd, depthImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    vulkanBeginRender(.cmd      = cmd,
                      .pipe      = &pipe,
                      .color1    = sceneColor);

    vulkanViewport(cmd, 0, sceneColor->extent.height, sceneColor->extent.width,
                   -((i32)sceneColor->extent.height));
    vulkanScissor(cmd, 0, 0, sceneColor->extent.width, sceneColor->extent.height);

    vulkanBindPipe(cmd, &pipe);
    vulkanBindVertex(cmd, &vbufCopy, 0, NULL, 0, NULL, 0);
    vulkanBindIndex(cmd, &ibufCopy, 0, VK_INDEX_TYPE_UINT32);

    WaterPushConstants pc = {
        .depthIndex = (u32)depthImage->sampledPoolIndex,
        .width      = depthImage->extent.width,
        .height     = depthImage->extent.height,
        .nearZ      = cam->znear,
        .farZ       = cam->zfar,
        .projM00    = cam->cameraUbo.projection[0][0],
        .projM11    = cam->cameraUbo.projection[1][1],
    };
    vulkanPush(cmd, &pipe, sizeof(pc), &pc);

    vkCmdDrawIndexed(cmd->cmd, idxCount, 1, 0, 0, 0);
    renderer.drawCalls++;
    renderer.instanceCount++;
    renderer.triangleCount += idxCount / 3;

    vulkanEndRender(cmd);
}

static void removed(void) {
    vulkanAzgaarWaterClear();
    threadLock(&uploadLock);
    if (vertexBuffer.buf) vulkanDestroyBuffer(&vertexBuffer, VK_NULL_HANDLE);
    if (indexBuffer.buf) vulkanDestroyBuffer(&indexBuffer, VK_NULL_HANDLE);
    vertexCount = indexCount = 0;
    uploaded = false;
    threadUnlock(&uploadLock);
    vulkanDestroyPipe(&pipe);
}
