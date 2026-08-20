#include "VulkanDebugNavMeshPass.h"
#include "VulkanDebugNavMeshPass.h"
#include "ecs/Ecs.h"
#include "ecs/system/System.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "renderer/Renderer.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanBuffer.h"
#include "renderer/vulkan/resources/VulkanFrameResources.h"
#include "renderer/vulkan/swapchain/VulkanSwapchain.h"
#include <stdlib.h>

// Forward declarations from recast C API
typedef struct RcNavMesh RcNavMesh;

namespace engine {
typedef struct RcDebugTriangle {
    float v0[3];
    float v1[3];
    float v2[3];
    uint32_t color;
} RcDebugTriangle;

#ifdef __cplusplus
extern "C"
#endif
unsigned int rcNavMeshGetDebugTriangles(const RcNavMesh* mesh,
                                        RcDebugTriangle* outTris,
                                        unsigned int maxTriangles);


VulkanDebugNavMeshPass vulkanDebugNavMeshPass;

VulkanDebugNavMeshPass::VulkanDebugNavMeshPass() : System("debug_navmesh") {}

static VulkanPipe fillPipeline;
static VulkanBuffer vertexBuffer;
static char enabled;
static float yOffset = 0.25f;
static RcNavMesh* registeredMesh;

// Three vertices per triangle: pos (xyz+pad) + color (rgba)
typedef struct {
    float pos[4];
    float color[4];
} DebugVertex;

#define MAX_DEBUG_TRIANGLES 200000
#define MAX_DEBUG_VERTICES (MAX_DEBUG_TRIANGLES * 3)

void vulkanDebugNavMeshSetEnabled(char e) {
    enabled = e;
}

char vulkanDebugNavMeshIsEnabled(void) {
    return enabled;
}

void vulkanDebugNavMeshSetMesh(RcNavMesh* mesh) {
    registeredMesh = mesh;
}

static void recreatePipelines(void) {
    if (fillPipeline.pipe) vulkanDestroyPipe(&fillPipeline);

    // Pass 1: Semi-transparent filled triangles — writes depth
    fillPipeline = vulkanCreatePipe(.name = "debug_navmesh_fill",
                                    .vs   = "shaders/pass/debug_navmesh/spv/debug_navmesh.vert.spv",
                                    .fs   = "shaders/pass/debug_navmesh/spv/debug_navmesh.frag.spv",
                                    .colorFormat1        = VK_FORMAT_R16G16B16A16_SFLOAT,
                                    .depthFormat         = VK_FORMAT_D32_SFLOAT,
                                    .blend               = 1,
                                    .noCull              = 1,
                                    .in1attr =
                                        VkVertexInputAttributeDescription{
                                            .location = 0,
                                            .binding  = 0,
                                            .format   = VK_FORMAT_R32G32B32A32_SFLOAT,
                                            .offset   = 0,
                                        },
                                    .in1bind =
                                        VkVertexInputBindingDescription{
                                            .binding   = 0,
                                            .stride    = sizeof(DebugVertex),
                                            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
                                        },
                                    .in2attr = VkVertexInputAttributeDescription{
                                        .location = 1,
                                        .binding  = 0,
                                        .format   = VK_FORMAT_R32G32B32A32_SFLOAT,
                                        .offset   = 16,
                                    }, );

    utils::info("debug_navmesh: fill + wire pipelines recreated");
}

static void swapchainCreated(void*) {
    recreatePipelines();
}

void VulkanDebugNavMeshPass::added() {
    if (getenv("ENGINE_DEBUG_NAVMESH")) {
        enabled = 1;
        utils::info("debug_navmesh: enabled via ENGINE_DEBUG_NAVMESH env var");
    }
    const char* offsetEnv = getenv("ENGINE_DEBUG_NAVMESH_Y_OFFSET");
    if (offsetEnv) yOffset = (float)atof(offsetEnv);
    utils::info("debug_navmesh: y offset %.3f", yOffset);

    utils::signalSubscribe("swapchainCreated", swapchainCreated);
    recreatePipelines();

    vertexBuffer =
        vulkanCreateGpuBuffer("debug_navmesh_vertices",
                              MAX_DEBUG_VERTICES * sizeof(DebugVertex),
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    utils::info("debug_navmesh: vertex buffer created");
}

void VulkanDebugNavMeshPass::update() {
    if (vulkan.skipFrame || !enabled || !registeredMesh) return;
    if (!fillPipeline.pipe) return;

    VulkanCommand* cmd = vulkan.currentCmd;
    if (!cmd) return;

    VulkanImage* sceneColor  = vulkanFrameResourcesGetSceneColor();
    VulkanImage* depthImage  = vulkanFrameResourcesGetDepth();
    if (!sceneColor || !depthImage) return;

    Entity* camEntity = cameraGetEntity();
    if (!camEntity) return;
    Camera* camera = getComponent(camEntity->scene, Camera, camEntity->id);
    if (!camera) return;

    static RcDebugTriangle cpuTris[MAX_DEBUG_TRIANGLES];
    uint32_t triCount = rcNavMeshGetDebugTriangles(registeredMesh, cpuTris, MAX_DEBUG_TRIANGLES);
    {
        static bool _l = false;
        if (!_l) {
            utils::info("debug_navmesh: %u triangles (cap=%u)", triCount, MAX_DEBUG_TRIANGLES);
            _l = true;
        }
    }
    if (triCount == 0) return;
    if (triCount >= MAX_DEBUG_TRIANGLES) {
        utils::warn("debug_navmesh: hit triangle cap (%u), increase MAX_DEBUG_TRIANGLES", triCount);
    }
    static DebugVertex cpuVerts[MAX_DEBUG_VERTICES];
    uint32_t vertIndex = 0;

    // static float oo = 0;
    // oo += timer.dt;
    for (uint32_t i = 0; i < triCount && vertIndex + 3 <= MAX_DEBUG_VERTICES; i++) {
        const RcDebugTriangle* t = &cpuTris[i];
        uint32_t c               = t->color;

        for (int v = 0; v < 3; v++) {
            memcpy(cpuVerts[vertIndex].pos,
                   v == 0   ? t->v0
                   : v == 1 ? t->v1
                            : t->v2,
                   sizeof(float) * 3);

            cpuVerts[vertIndex].pos[1] += yOffset;
            cpuVerts[vertIndex].pos[3]   = 1.0f;
            cpuVerts[vertIndex].color[0] = (float)((c >> 24) & 0xFF) / 255.0f;
            cpuVerts[vertIndex].color[1] = (float)((c >> 16) & 0xFF) / 255.0f;
            cpuVerts[vertIndex].color[2] = (float)((c >> 8) & 0xFF) / 255.0f;
            cpuVerts[vertIndex].color[3] = 1.0f;

            vertIndex++;
        }
    }

    // Copy vertex data to GPU buffer
    vulkanCopy(.cmd         = cmd,
               .target.buf  = &vertexBuffer,
               .source.data = cpuVerts,
               .size        = static_cast<u32>(vertIndex * sizeof(DebugVertex)));

    // Push constants
    typedef struct {
        mat4 viewProjection;
        u32 vertexCount;
        u32 pad[3];
    } PushConstants;

    PushConstants pc;
    glm_mat4_copy(camera->cameraUbo.viewProjection, pc.viewProjection);
    pc.vertexCount = vertIndex;

    // Pass 1: Filled semi-transparent triangles (writes to sceneColor + depth)
    vulkanBeginRender(.cmd      = cmd,
                      .pipe      = &fillPipeline,
                      .color1    = sceneColor,
                      .depth     = depthImage);
    vulkanViewport(cmd,
                   0,
                   sceneColor->extent.height,
                   sceneColor->extent.width,
                   -((i32)sceneColor->extent.height));
    vulkanScissor(cmd, 0, 0, sceneColor->extent.width, sceneColor->extent.height);
    vulkanBindPipe(cmd, &fillPipeline);
    vulkanPush(cmd, &fillPipeline, sizeof(pc), &pc);
    vulkanBindVertex(cmd, &vertexBuffer, 0, NULL, 0, NULL, 0);
    vulkanDraw(cmd, (int)vertIndex, 1);
    vulkanEndRender(cmd);
}

void VulkanDebugNavMeshPass::removed() {
    vulkanDestroyPipe(&fillPipeline);
    vulkanDestroyBuffer(&vertexBuffer, VK_NULL_HANDLE);
}
}  // namespace engine
