#include "VulkanDebugPhysicsPass.h"
#include "VulkanDebugPhysicsPass.h"
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

namespace engine {

VulkanDebugPhysicsPass vulkanDebugPhysicsPass;

VulkanDebugPhysicsPass::VulkanDebugPhysicsPass() : System("debug_physics") {}

static VulkanPipe pipeline;
static VulkanBuffer vertexBuffer;
static char enabled;

#define MAX_DEBUG_CHARACTERS 64
static JoltCharacter* registeredCharacters[MAX_DEBUG_CHARACTERS];
static u32 registeredCharacterCount;

void vulkanDebugPhysicsRegisterCharacter(JoltCharacter* c) {
    if (!c || registeredCharacterCount >= MAX_DEBUG_CHARACTERS) return;
    for (u32 i = 0; i < registeredCharacterCount; i++)
        if (registeredCharacters[i] == c) return;
    registeredCharacters[registeredCharacterCount++] = c;
}

void vulkanDebugPhysicsUnregisterCharacter(JoltCharacter* c) {
    if (!c) return;
    for (u32 i = 0; i < registeredCharacterCount; i++) {
        if (registeredCharacters[i] == c) {
            registeredCharacters[i] = registeredCharacters[--registeredCharacterCount];
            return;
        }
    }
}

// Two vec4s per vertex: pos (xyz + pad) + color (rgba)
typedef struct {
    float pos[4];
    float color[4];
} DebugVertex;

#define MAX_DEBUG_LINES 8192
#define MAX_DEBUG_VERTICES (MAX_DEBUG_LINES * 2)

static inline char debugLineInFrustum(const float* posA, const float* posB, const vec4* planes) {
    for (u32 p = 0; p < 6; p++) {
        float dA =
            planes[p][0] * posA[0] + planes[p][1] * posA[1] + planes[p][2] * posA[2] + planes[p][3];
        float dB =
            planes[p][0] * posB[0] + planes[p][1] * posB[1] + planes[p][2] * posB[2] + planes[p][3];
        if (dA < -0.5f && dB < -0.5f) return 0;
    }
    return 1;
}

// Colors (0xRRGGBBAA → rgba floats)
#define COLOR_DYNAMIC 0.0f, 1.0f, 0.392f, 1.0f   // green
#define COLOR_STATIC 1.0f, 0.784f, 0.0f, 1.0f    // yellow
#define COLOR_SENSOR 0.392f, 0.392f, 1.0f, 1.0f  // blue
#define COLOR_CHARACTER 0.0f, 1.0f, 1.0f, 1.0f   // cyan

void vulkanDebugPhysicsSetEnabled(char e) {
    enabled = e;
}

char vulkanDebugPhysicsIsEnabled(void) {
    return enabled;
}

void VulkanDebugPhysicsPass::added() {
    if (getenv("ENGINE_DEBUG_PHYSICS")) {
        enabled = 1;
        utils::info("debug_physics: enabled via ENGINE_DEBUG_PHYSICS env var");
    }

    pipeline = vulkanCreatePipe(.name = "debug_physics",
                                .vs   = "shaders/pass/debug_physics/spv/debug_physics.vert.spv",
                                .fs   = "shaders/pass/debug_physics/spv/debug_physics.frag.spv",
                                .colorFormat1 = VK_FORMAT_B8G8R8A8_SRGB,
                                .lineList     = 1,
                                .blend        = 1,
                                .noCull       = 1,
                                .depthClamp   = 1,
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
                                    .offset   = 16,  // sizeof(float) * 4
                                }, );

    vertexBuffer =
        vulkanCreateGpuBuffer("debug_physics_vertices",
                              MAX_DEBUG_VERTICES * sizeof(DebugVertex),
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    utils::info("debug_physics: pipeline and vertex buffer created");
}

void VulkanDebugPhysicsPass::update() {
    if (vulkan.skipFrame || !enabled) return;

    VulkanCommand* cmd     = vulkan.currentCmd;
    VulkanImage* swapImage = vulkanSwapchain.currentSwapchainImage;
    if (!swapImage) return;

    Entity* camEntity = cameraGetEntity();
    if (!camEntity) return;
    Camera* camera = getComponent(camEntity->scene, Camera, camEntity->id);
    if (!camera) return;

    static DebugVertex cpuVerts[MAX_DEBUG_VERTICES];
    u32 vertIndex = 0;

    // 1) Registered character shapes (cyan, no frustum cull)
    for (u32 i = 0; i < registeredCharacterCount && vertIndex + 2 <= MAX_DEBUG_VERTICES; i++) {
        JoltDebugLine charLines[512];
        u32 n =
            joltGetDebugLinesForCharacter(registeredCharacters[i], 0x00FFFFFF, charLines, 512, 0);
        for (u32 j = 0; j < n && vertIndex + 2 <= MAX_DEBUG_VERTICES; j++) {
            // Vertex A
            glm_vec3_copy(charLines[j].posA, cpuVerts[vertIndex].pos);
            cpuVerts[vertIndex].pos[3]   = 1.0f;
            cpuVerts[vertIndex].color[0] = 0.0f;
            cpuVerts[vertIndex].color[1] = 1.0f;
            cpuVerts[vertIndex].color[2] = 1.0f;
            cpuVerts[vertIndex].color[3] = 1.0f;
            vertIndex++;
            // Vertex B
            glm_vec3_copy(charLines[j].posB, cpuVerts[vertIndex].pos);
            cpuVerts[vertIndex].pos[3]   = 1.0f;
            cpuVerts[vertIndex].color[0] = 0.0f;
            cpuVerts[vertIndex].color[1] = 1.0f;
            cpuVerts[vertIndex].color[2] = 1.0f;
            cpuVerts[vertIndex].color[3] = 1.0f;
            vertIndex++;
        }
    }

    // 2) Jolt body shapes (with frustum culling, after characters so they don't
    //    eat the entire vertex budget)
    JoltDebugLine lines[MAX_DEBUG_LINES];
    u32 bodyLines = joltGetDebugLines(lines, MAX_DEBUG_LINES);
    for (u32 i = 0; i < bodyLines && vertIndex + 2 <= MAX_DEBUG_VERTICES; i++) {
        if (!debugLineInFrustum(lines[i].posA, lines[i].posB, camera->cameraUbo.frustumPlanes)) continue;
        // Vertex A
        glm_vec3_copy(lines[i].posA, cpuVerts[vertIndex].pos);
        cpuVerts[vertIndex].pos[3]   = 1.0f;
        uint32_t ca                  = lines[i].colorA;
        cpuVerts[vertIndex].color[0] = (float)((ca >> 24) & 0xFF) / 255.0f;
        cpuVerts[vertIndex].color[1] = (float)((ca >> 16) & 0xFF) / 255.0f;
        cpuVerts[vertIndex].color[2] = (float)((ca >> 8) & 0xFF) / 255.0f;
        cpuVerts[vertIndex].color[3] = (float)(ca & 0xFF) / 255.0f;
        vertIndex++;
        // Vertex B
        glm_vec3_copy(lines[i].posB, cpuVerts[vertIndex].pos);
        cpuVerts[vertIndex].pos[3]   = 1.0f;
        uint32_t cb                  = lines[i].colorB;
        cpuVerts[vertIndex].color[0] = (float)((cb >> 24) & 0xFF) / 255.0f;
        cpuVerts[vertIndex].color[1] = (float)((cb >> 16) & 0xFF) / 255.0f;
        cpuVerts[vertIndex].color[2] = (float)((cb >> 8) & 0xFF) / 255.0f;
        cpuVerts[vertIndex].color[3] = (float)(cb & 0xFF) / 255.0f;
        vertIndex++;
    }

    if (vertIndex == 0) return;

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
    glm_mat4_copy(camera->cameraUbo.viewProjectionNoJitter, pc.viewProjection);
    pc.vertexCount = vertIndex;

    vulkanBeginRender(.cmd = cmd, .pipe = &pipeline, .color1 = swapImage);
    vulkanViewport(cmd,
                   0,
                   swapImage->extent.height,
                   swapImage->extent.width,
                   -((i32)swapImage->extent.height));
    vulkanScissor(cmd, 0, 0, swapImage->extent.width, swapImage->extent.height);
    vulkanBindPipe(cmd, &pipeline);
    vulkanPush(cmd, &pipeline, sizeof(pc), &pc);
    vulkanBindVertex(cmd, &vertexBuffer, 0, NULL, 0, NULL, 0);
    vulkanDraw(cmd, (int)vertIndex, 1);
    vulkanEndRender(cmd);
}

void VulkanDebugPhysicsPass::removed() {
    vulkanDestroyPipe(&pipeline);
    vulkanDestroyBuffer(&vertexBuffer, VK_NULL_HANDLE);
}
}  // namespace engine
