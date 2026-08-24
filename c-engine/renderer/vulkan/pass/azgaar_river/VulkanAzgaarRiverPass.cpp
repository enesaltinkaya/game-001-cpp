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

namespace engine {

VulkanAzgaarRiverPass vulkanAzgaarRiverPass;

VulkanAzgaarRiverPass::VulkanAzgaarRiverPass() : System("azgaar_river") {}

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
static utils::Thread uploadLock = {.mutex = PTHREAD_MUTEX_INITIALIZER};
static std::vector<SceneVertex> pendingVertexData;
static std::vector<u32> pendingIndexData;
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
    utils::threadLock(&uploadLock);
    pendingVertexData.clear();
    pendingIndexData.clear();
    pendingVertexCount = pendingIndexCount = 0;
    pendingClear = false;

    pendingVertexCount = vCount;
    pendingIndexCount = iCount;
    pendingVertexData.assign(static_cast<const SceneVertex*>(vertices), static_cast<const SceneVertex*>(vertices) + vCount);
    pendingIndexData.assign(static_cast<const u32*>(indices), static_cast<const u32*>(indices) + iCount);
    utils::threadUnlock(&uploadLock);
}

void vulkanAzgaarRiverClear(void) {
    utils::threadLock(&uploadLock);
    pendingClear = true;
    pendingVertexData.clear();
    pendingIndexData.clear();
    pendingVertexCount = pendingIndexCount = 0;
    utils::threadUnlock(&uploadLock);
}

bool vulkanAzgaarRiverGetGpuMesh(VulkanBuffer** outVertexBuffer,
                                  VulkanBuffer** outIndexBuffer,
                                  u32* outVertexCount,
                                  u32* outIndexCount) {
    utils::threadLock(&uploadLock);
    bool ok = uploaded && vertexBuffer.buf && indexBuffer.buf;
    if (ok) {
        if (outVertexBuffer) *outVertexBuffer = &vertexBuffer;
        if (outIndexBuffer)  *outIndexBuffer  = &indexBuffer;
        if (outVertexCount)  *outVertexCount  = vertexCount;
        if (outIndexCount)   *outIndexCount   = indexCount;
    }
    utils::threadUnlock(&uploadLock);
    return ok;
}

static void uploadPending(void) {
    utils::threadLock(&uploadLock);
    bool hasClear = pendingClear;
    bool hasData = !pendingVertexData.empty() && !pendingIndexData.empty() && pendingVertexCount > 0 && pendingIndexCount > 0;
    if (!hasClear && !hasData) { utils::threadUnlock(&uploadLock); return; }

    const SceneVertex* verts = nullptr;
    const u32* idxs = nullptr;
    u32 vCount = 0, iCount = 0;
    if (hasData) {
        verts = pendingVertexData.data();
        idxs = pendingIndexData.data();
        vCount = pendingVertexCount;
        iCount = pendingIndexCount;
        pendingVertexData.clear();
        pendingIndexData.clear();
        pendingVertexCount = pendingIndexCount = 0;
    }
    pendingClear = false;
    utils::threadUnlock(&uploadLock);

    if (hasClear) {
        utils::threadLock(&uploadLock);
        if (vertexBuffer.buf) vulkanDestroyBuffer(&vertexBuffer, VK_NULL_HANDLE);
        if (indexBuffer.buf) vulkanDestroyBuffer(&indexBuffer, VK_NULL_HANDLE);
        vertexCount = indexCount = 0;
        uploaded = false;
        utils::threadUnlock(&uploadLock);
        return;
    }

    if (!verts || !idxs) {
        return;
    }

    VulkanBuffer vbuf = vulkanCreateGpuBuffer(utils::strtmp("AzgaarRiverVBO"),
                                               vCount * sizeof(SceneVertex),
                                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VulkanBuffer ibuf = vulkanCreateGpuBuffer(utils::strtmp("AzgaarRiverIBO"),
                                               iCount * sizeof(u32),
                                               VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanCopy(.cmd = cmd, .source.data = (void*)verts, .target.buf = &vbuf,
               .size = static_cast<u32>(vCount * sizeof(SceneVertex)));
    vulkanCopy(.cmd = cmd, .source.data = (void*)idxs, .target.buf = &ibuf,
               .size = static_cast<u32>(iCount * sizeof(u32)));
    vulkanTransientEnd(cmd, 1);

    utils::threadLock(&uploadLock);
    if (vertexBuffer.buf) vulkanDestroyBuffer(&vertexBuffer, VK_NULL_HANDLE);
    if (indexBuffer.buf) vulkanDestroyBuffer(&indexBuffer, VK_NULL_HANDLE);
    vertexBuffer = vbuf;
    indexBuffer = ibuf;
    vertexCount = vCount;
    indexCount = iCount;
    uploaded = true;
    utils::threadUnlock(&uploadLock);
}

void VulkanAzgaarRiverPass::added() {
    utils::signalSubscribe("swapchainCreated", swapchainCreated);
    recreatePipelines();
}

void VulkanAzgaarRiverPass::preUpdate() {
    uploadPending();
}

void VulkanAzgaarRiverPass::update() {
    if (vulkan.skipFrame) { return; }

    utils::threadLock(&uploadLock);
    bool isUploaded = uploaded;
    VulkanBuffer vbufCopy = vertexBuffer;
    VulkanBuffer ibufCopy = indexBuffer;
    u32 idxCount = indexCount;
    utils::threadUnlock(&uploadLock);
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

void VulkanAzgaarRiverPass::removed() {
    vulkanAzgaarRiverClear();
    utils::threadLock(&uploadLock);
    if (vertexBuffer.buf) vulkanDestroyBuffer(&vertexBuffer, VK_NULL_HANDLE);
    if (indexBuffer.buf) vulkanDestroyBuffer(&indexBuffer, VK_NULL_HANDLE);
    vertexCount = indexCount = 0;
    uploaded = false;
    utils::threadUnlock(&uploadLock);
    vulkanDestroyPipe(&pipe);
}
}  // namespace engine
