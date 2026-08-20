#include "ecs/system/System.h"
#include "ecs/system/window/WindowSystem.h"
#include "image/Image.h"
#include "renderer/Renderer.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"

#include "renderer/texture/TextureManager.h"
#include "renderer/vulkan/resources/VulkanDesc.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/swapchain/VulkanSwapchain.h"
#include "rmlui/wrapper/src/crmlui.h"
#include "container/Map.h"

#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/resources/VulkanBuffer.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"

static void added(void);
static void preUpdate(void);
static void postUpdate(void);
static void removed(void);

static double elapsedCPU;
static double elapsedGPU;

System vulkanRmluiPass = {
    .name       = "rmlui",
    .added      = added,
    .preUpdate  = preUpdate,
    .postUpdate = postUpdate,
    .removed    = removed,
};

////////////////////////////////////////////////////////////////////
#define RML_BUFFER_SIZE (1024L * 1024 * 5)
#define MAX_CACHED_RML_GEOMETRY 1000
Map(uintptr_t, void*) rmlManagedTextures;

typedef struct RmlInstanceData {
    mat4 transform;
    vec2 translation;
    int textureId;
    int padding;
} RmlInstanceData;

typedef struct RmlGeometry {
    VulkanVirtualBuf vertexVirtualBuf;
    VulkanVirtualBuf indexVirtualBuf;
    RmlInstanceData instanceData;
    u32 numVertices;
    u32 numIndices;
    int entityId;
    uintptr_t texture;
    char inUse;
} RmlGeometry;

static struct {
    uintptr_t vertexBuffer;
    uintptr_t instanceBuffer;
} pushConstant;

static void createBuffers(void);
static void createPipeline(void);
static void freeRemovedGeometries(void);

static Array(uintptr_t) geometriesToRemove;
static VulkanPipe pipeline;

static VulkanBuffer vertexBuffer;
static VulkanBuffer indexBuffer;
static VulkanBuffer instanceBuffer;
static VulkanBuffer indirectDrawScissor[FRAMES_IN_FLIGHT];
static VulkanBuffer indirectDrawNoScissor[FRAMES_IN_FLIGHT];

static Array(VkDrawIndexedIndirectCommand) drawCommandsScissor;
static Array(VkDrawIndexedIndirectCommand) drawCommandsNoScissor;

static int viewportW, viewportH;
static int scissorEnabled, scissorX, scissorY, scissorW, scissorH;
static u32 drawCalls, instanceCount, triangleCount;
static mat4 projection, model;

void added(void) {
    glm_mat4_identity(model);
    glm_ortho(0, window.width, 0, window.height, -10000, 10000, projection);

    createBuffers();
    createPipeline();

    guiManagerRmlUi.added();
}

void createBuffers(void) {
    vertexBuffer =
        vulkanCreateCpuBuffer("rmlVertex", RML_BUFFER_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    indexBuffer =
        vulkanCreateCpuBuffer("rmlIndex", RML_BUFFER_SIZE, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    instanceBuffer = vulkanCreateCpuBuffer("rmlInstance",
                                           sizeof(RmlInstanceData) * MAX_CACHED_RML_GEOMETRY,
                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    pushConstant.vertexBuffer   = vertexBuffer.address;
    pushConstant.instanceBuffer = instanceBuffer.address;

    for (i32 i = 0, si = FRAMES_IN_FLIGHT; i < si; i++) {
        indirectDrawScissor[i]   = vulkanCreateCpuBuffer("rmlIndirectScissor",
                                                         sizeof(VkDrawIndexedIndirectCommand) * 500,
                                                         VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
        indirectDrawNoScissor[i] = vulkanCreateCpuBuffer("rmlIndirectNoScissor",
                                                         sizeof(VkDrawIndexedIndirectCommand) * 500,
                                                         VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    }
}

void createPipeline(void) {
    pipeline = vulkanCreatePipe(.name         = "rml",
                                .vs           = "shaders/pass/rmlui/spv/vertex.vert.spv",
                                .fs           = "shaders/pass/rmlui/spv/fragment.frag.spv",
                                .colorFormat1 = VK_FORMAT_B8G8R8A8_SRGB,
                                // .clearColor1  = {0, 0, 0, 1},
                                .blend = 1);
}

/////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////
/////////// RMLUI RELATED STUFF
/////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////
void rmlBeginFrame(void) {}

void rmlEndFrame(void) {
    if (vulkan.skipFrame) {
        return;
    }
    assert(arraySize(drawCommandsNoScissor) > 500 || "syke!");
    assert(arraySize(drawCommandsScissor) > 500 || "syke!");

    VulkanCommand* cmd = vulkan.currentCmd;
    vulkanBeginProfile(cmd, &pipeline.profile, 0);
    vulkanBindPipe(cmd, &pipeline);
    vulkanViewport(cmd, 0, viewportH, viewportW, -viewportH);
    vulkanBindIndex(cmd, &indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    vulkanPush(cmd, &pipeline, sizeof(pushConstant), &pushConstant);

    vulkanBeginRender(.cmd    = cmd,  //
                      .pipe   = &pipeline,
                      .color1 = vulkanSwapchain.currentSwapchainImage);

    if (arraySize(drawCommandsNoScissor) > 0) {
        vulkanScissor(cmd, 0, 0, window.width, window.height);
        vulkanCopy(.target.buf  = &indirectDrawNoScissor[renderer.flightIndex],
                   .source.data = drawCommandsNoScissor,
                   .size = static_cast<u32>(sizeof(VkDrawIndexedIndirectCommand) * arraySize(drawCommandsNoScissor)));
        vulkanDrawIndexedIndirect(cmd,
                                  &indirectDrawNoScissor[renderer.flightIndex],
                                  arraySize(drawCommandsNoScissor),
                                  sizeof(VkDrawIndexedIndirectCommand));
        drawCalls += arraySize(drawCommandsNoScissor);
        instanceCount += arraySize(drawCommandsNoScissor);
    }

    if (arraySize(drawCommandsScissor) > 0) {
        vulkanScissor(cmd, scissorX, scissorY, scissorW, scissorH);

        vulkanCopy(.target.buf  = &indirectDrawScissor[renderer.flightIndex],
                   .source.data = drawCommandsScissor,
                   .size = static_cast<u32>(sizeof(VkDrawIndexedIndirectCommand) * arraySize(drawCommandsScissor)));

        vulkanDrawIndexedIndirect(cmd,
                                  &indirectDrawScissor[renderer.flightIndex],
                                  arraySize(drawCommandsScissor),
                                  sizeof(VkDrawIndexedIndirectCommand));
        drawCalls += arraySize(drawCommandsScissor);
        instanceCount += arraySize(drawCommandsScissor);
    }
    vulkanEndRender(cmd);

    vulkanEndProfile(vulkan.currentCmd, &pipeline.profile, 0);
    elapsedGPU = pipeline.profile.elapsed;

    arrayClear(drawCommandsNoScissor);
    arrayClear(drawCommandsScissor);
}

uintptr_t rmlLoadTexture(int* outX, int* outY, const char* path) {
    if (strStartsWith(path, "fb-")) {
        Texture* texture = getTextureByName(path);
        if (!texture) {
            return 0;
        }
        if (outX) {
            *outX = texture->image.width;
        }
        if (outY) {
            *outY = texture->image.height;
        }
        if (!texture) {
            warn("could not find %s returning dummy image", path);
            return (uintptr_t)&vulkanResources.dummyImage;
        }
        return (uintptr_t)texture->backendImg;
    }

    Image image = imageLoad(path);

    VulkanImage* img = static_cast<VulkanImage*>(memoryAlloc(sizeof(VulkanImage)));
    *img = vulkanCreateImage(.name   = path,  //
                             .width  = image.width,
                             .height = image.height,
                             .format = static_cast<VkFormat>(image.vkFormat ? image.vkFormat : VK_FORMAT_R8G8B8A8_UNORM));
    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanTransition(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);
    vulkanCopy(.cmd = cmd, .target.img = img, .source.data = (void*)image.data, .size = static_cast<u32>(image.size));
    vulkanTransition(cmd, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransientEnd(cmd, 0);

    uintptr_t texKey = (uintptr_t)img->img;
    mapPut(rmlManagedTextures, texKey, 0);

    if (outX) {
        *outX = image.width;
    }
    if (outY) {
        *outY = image.height;
    }

    imageDestory(&image);
    return (uintptr_t)img;
}

uintptr_t rmlGenerateTexture(const unsigned char* data, size_t size, int x, int y) {
    static int counter;
    const char* name   = strtmp("rml generated: %d", counter++);
    VulkanImage* img = static_cast<VulkanImage*>(memoryAlloc(sizeof(VulkanImage)));
    *img               = vulkanCreateImage(.name = name, .width = x, .height = y);
    VulkanCommand* cmd = vulkanTransientBegin();
    vulkanTransition(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1);
    vulkanCopy(.cmd = cmd, .target.img = img, .source.data = (void*)data, .size = static_cast<u32>(size));
    vulkanTransition(cmd, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);
    vulkanTransientEnd(cmd, 0);

    uintptr_t texKey = (uintptr_t)img->img;
    mapPut(rmlManagedTextures, texKey, 0);
    return (uintptr_t)img;
}

void rmlReleaseTexture(uintptr_t textureHandleOut) {
    VulkanImage* img = (VulkanImage*)textureHandleOut;
    uintptr_t texKey = (uintptr_t)img->img;
    if (mapContainsKey(rmlManagedTextures, texKey)) {
        vulkanDestroyImage(img, NULL);
        mapRemove(rmlManagedTextures, texKey);
        memoryFree(img);
    }
}

static RmlGeometry geometryPool[MAX_CACHED_RML_GEOMETRY];
static int geometryPoolCursor;

static RmlGeometry* getGeometryFromPool(void) {
    RmlGeometry* item;
    while (1) {
        item           = &geometryPool[geometryPoolCursor];
        item->entityId = geometryPoolCursor;
        geometryPoolCursor++;
        if (geometryPoolCursor > MAX_CACHED_RML_GEOMETRY - 1) {
            geometryPoolCursor = 0;
        }
        if (!item->inUse) {
            break;
        }
    }
    item->inUse = 1;
    return item;
}

uintptr_t rmlCompileGeometry(RmlVertex* vertices,
                             int vertexCount,
                             const int* indices,
                             int indexCount) {
    RmlGeometry* rmlGeometry = getGeometryFromPool();

    rmlGeometry->numIndices  = indexCount;
    rmlGeometry->numVertices = vertexCount;

    rmlGeometry->vertexVirtualBuf =
        vulkanBufferAllocateVirtual(&vertexBuffer, sizeof(RmlVertex) * vertexCount, 4);
    vulkanCopy(.target.buf          = &vertexBuffer,
               .target.bufferOffset = static_cast<u32>(rmlGeometry->vertexVirtualBuf.offset),
               .source.data         = vertices,
               .size                = static_cast<u32>(sizeof(RmlVertex) * vertexCount));

    rmlGeometry->indexVirtualBuf =
        vulkanBufferAllocateVirtual(&indexBuffer, sizeof(int) * indexCount, 4);
    vulkanCopy(.target.buf          = &indexBuffer,  //
               .target.bufferOffset = static_cast<u32>(rmlGeometry->indexVirtualBuf.offset),
               .source.data         = (void*)indices,
               .size                = static_cast<u32>(sizeof(u32) * indexCount));

    return (uintptr_t)rmlGeometry;
}

void rmlReleaseGeometry(uintptr_t geometryHandle) {
    arrayPut(geometriesToRemove, geometryHandle);
}

void rmlRenderGeometry(uintptr_t rmlGeometryHandle,
                       float translationX,
                       float translationY,
                       uintptr_t texture) {
    if (vulkan.skipFrame) {
        return;
    }

    RmlGeometry* rmlGeometry = (RmlGeometry*)rmlGeometryHandle;
    rmlGeometry->texture     = texture;
    glm_mat4_mul(projection, model, rmlGeometry->instanceData.transform);
    rmlGeometry->instanceData.translation[0] = translationX;
    rmlGeometry->instanceData.translation[1] = translationY;

    if (rmlGeometry->texture) {
        VulkanImage* img                    = (VulkanImage*)rmlGeometry->texture;
        rmlGeometry->instanceData.textureId = img->sampledPoolIndex;
    } else {
        rmlGeometry->instanceData.textureId = 0;
    }

    vulkanCopy(.target.buf          = &instanceBuffer,
               .target.bufferOffset = static_cast<u32>(sizeof(RmlInstanceData) * rmlGeometry->entityId),
               .source.data         = &rmlGeometry->instanceData,
               .size                = sizeof(RmlInstanceData));

    VkDrawIndexedIndirectCommand command = {
        .indexCount    = rmlGeometry->numIndices,
        .instanceCount = 1,
        .firstIndex    = static_cast<uint32_t>(rmlGeometry->indexVirtualBuf.offset / sizeof(int)),
        .vertexOffset  = static_cast<int32_t>(rmlGeometry->vertexVirtualBuf.offset / sizeof(RmlVertex)),
        .firstInstance = static_cast<uint32_t>(rmlGeometry->entityId),
    };
    triangleCount += rmlGeometry->numIndices / 3;

    if (scissorEnabled) {
        arrayPut(drawCommandsScissor, command);
    } else {
        arrayPut(drawCommandsNoScissor, command);
    }
}

void rmlEnableScissorRegion(char enable) {
    scissorEnabled = (int)enable;
}

void rmlSetScissorRegion(int x, int y, int width, int height) {
    scissorX = x;
    scissorY = y;
    scissorW = width;
    scissorH = height;
}

void rmlSetTransform(void* transform) {
    if (transform) {
        memcpy(model, transform, sizeof(mat4));
    } else {
        glm_mat4_identity(model);
    }
}

void rmlSetViewport(int width, int height) {
    viewportW = width;
    viewportH = height;
}

void preUpdate(void) {
    vulkanResetProfile(vulkan.currentCmd, &pipeline.profile, 0);
}

static void postUpdate(void) {
    freeRemovedGeometries();
    glm_ortho(0, window.width, window.height, 0, -10000, 10000, projection);

    vulkanRmluiPass.cpuElapsed = elapsedCPU;
    vulkanRmluiPass.gpuElapsed = elapsedGPU;
    elapsedCPU                 = nanos();
    guiManagerRmlUi.postUpdate();

    renderer.drawCalls     += drawCalls;
    renderer.instanceCount += instanceCount;
    renderer.triangleCount += triangleCount;

    drawCalls     = 0;
    instanceCount = 0;
    triangleCount = 0;

    elapsedCPU = nanos() - elapsedCPU;
}

void removed(void) {
    guiManagerRmlUi.removed();

    freeRemovedGeometries();

    vulkanDestroyBuffer(&vertexBuffer, NULL);
    vulkanDestroyBuffer(&indexBuffer, NULL);
    vulkanDestroyBuffer(&instanceBuffer, NULL);
    for (i32 i = 0, si = FRAMES_IN_FLIGHT; i < si; i++) {
        vulkanDestroyBuffer(&indirectDrawScissor[i], NULL);
        vulkanDestroyBuffer(&indirectDrawNoScissor[i], NULL);
    }

    vulkanDestroyPipe(&pipeline);

    arrayFree(drawCommandsNoScissor);
    arrayFree(drawCommandsScissor);
    arrayFree(geometriesToRemove);
    mapFree(rmlManagedTextures);
}

void freeRemovedGeometries(void) {
    for (int i = 0, si = arraySize(geometriesToRemove); i < si; i++) {
        RmlGeometry* rmlGeometry = (RmlGeometry*)geometriesToRemove[i];
        vulkanBufferDestroyVirtual(&rmlGeometry->indexVirtualBuf);
        vulkanBufferDestroyVirtual(&rmlGeometry->vertexVirtualBuf);
        *rmlGeometry = (RmlGeometry){0};
    }
    arrayClear(geometriesToRemove);
}
