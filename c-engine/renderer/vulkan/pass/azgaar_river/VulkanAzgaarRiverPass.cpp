#include "renderer/vulkan/pass/azgaar_river/VulkanAzgaarRiverPass.h"
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

struct System vulkanAzgaarRiverPass = {
    .name      = "azgaar_river",
    .added     = added,
    .preUpdate = preUpdate,
    .update    = update,
    .removed   = removed,
};

// Must match the GLSL push-constant block in azgaar_river.frag.
typedef struct RiverPushConstants {
    u32 depthIndex;
    u32 width;
    u32 height;
    float nearZ;
    float farZ;
    float projM00;
    float projM11;
} RiverPushConstants;

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

    // The ribbon is a thin ribbon of water drawn after the opaque scene pass.
    // It must be depth-tested (read-only) against the scene depth buffer so that
    // closer geometry (the player, props, terrain) correctly occludes it —
    // otherwise the ribbons render "always on top".  Mirrors the azgaar_props
    // pass: depth attachment (D32), test-only, GREATER_OR_EQUAL.
    pipe = vulkanCreatePipe(
        .name                 = "azgaar_river",
        .vs                   = "shaders/pass/azgaar_river/spv/azgaar_river.vert.spv",
        .fs                   = "shaders/pass/azgaar_river/spv/azgaar_river.frag.spv",
        .colorFormat1         = VK_FORMAT_R16G16B16A16_SFLOAT,
        .depthFormat          = VK_FORMAT_D32_SFLOAT,
        .depthTestOnly       = 1,
        .depthCompareOp      = VK_COMPARE_OP_GREATER_OR_EQUAL,
        .noCull              = 1,
        .blend               = 1,
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

void vulkanAzgaarRiverSetMesh(const void* vertices, u32 vCount,
                               const void* indices, u32 iCount) {
    threadLock(&uploadLock);
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

void vulkanAzgaarRiverClear(void) {
    threadLock(&uploadLock);
    pendingClear = true;
    if (pendingVertexData) { memoryFree(pendingVertexData); pendingVertexData = NULL; }
    if (pendingIndexData) { memoryFree(pendingIndexData); pendingIndexData = NULL; }
    pendingVertexCount = pendingIndexCount = 0;
    threadUnlock(&uploadLock);
}

bool vulkanAzgaarRiverGetGpuMesh(VulkanBuffer** outVertexBuffer,
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

    VulkanBuffer vbuf = vulkanCreateGpuBuffer(strtmp("AzgaarRiverVBO"),
                                               vCount * sizeof(SceneVertex),
                                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VulkanBuffer ibuf = vulkanCreateGpuBuffer(strtmp("AzgaarRiverIBO"),
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

    Entity* camEntity = cameraGetEntity();
    Camera* cam       = camEntity ? getComponent(camEntity->scene, Camera, camEntity->id) : NULL;
    if (!cam) return;

    // The scene pass already wrote the depth buffer; bind it as a read-only
    // depth-stencil attachment (test-only) so the ribbon is occluded by closer
    // geometry.  The depth is an attachment here, NOT a sampled texture, so no
    // SHADER_READ_ONLY transition is needed (and one would conflict).
    vulkanBeginRender(.cmd      = cmd,
                      .pipe      = &pipe,
                      .color1    = sceneColor,
                      .depth     = depthImage);

    vulkanViewport(cmd, 0, sceneColor->extent.height, sceneColor->extent.width,
                   -((i32)sceneColor->extent.height));
    vulkanScissor(cmd, 0, 0, sceneColor->extent.width, sceneColor->extent.height);

    vulkanBindPipe(cmd, &pipe);
    vulkanBindVertex(cmd, &vbufCopy, 0, NULL, 0, NULL, 0);
    vulkanBindIndex(cmd, &ibufCopy, 0, VK_INDEX_TYPE_UINT32);

    RiverPushConstants pc = {
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
    vulkanAzgaarRiverClear();
    threadLock(&uploadLock);
    if (vertexBuffer.buf) vulkanDestroyBuffer(&vertexBuffer, VK_NULL_HANDLE);
    if (indexBuffer.buf) vulkanDestroyBuffer(&indexBuffer, VK_NULL_HANDLE);
    vertexCount = indexCount = 0;
    uploaded = false;
    threadUnlock(&uploadLock);
    vulkanDestroyPipe(&pipe);
}
