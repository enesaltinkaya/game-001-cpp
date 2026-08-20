#include "VulkanShadowPass.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/light/LightComponent.h"
#include "ecs/system/scene/Scene.h"
#include "events/Events.h"
#include "renderer/Renderer.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanIbl.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/scene/VulkanScene.h"
#include "renderer/vulkan/scene/VulkanVisibleScenes.h"
#include "renderer/vulkan/pass/azgaar_props/VulkanAzgaarPropsPass.h"
#include <float.h>
#include <math.h>

/* ── Constants ────────────────────────────────────────────────────────── */

#define SHADOW_MAP_SIZE 2048
#define SHADOW_MAX_DISTANCE 80.0f
#define SHADOW_ACTIVE_CASCADE_COUNT 2
#define SHADOW_CASCADE_BLEND_FRACTION 0.3f

/* ── Forward declarations ─────────────────────────────────────────────── */

namespace engine {

VulkanShadowPass vulkanShadowPass;

VulkanShadowPass::VulkanShadowPass() : System("shadow") {}

/* ── Pipeline + push constants ────────────────────────────────────────── */

static VulkanPipe shadowPipe;
static VulkanPipe shadowPipeDoubleSided;

typedef struct ShadowPushConstants {
    u64 transformBufferAddress;
    u64 drawInstanceBufferAddress;
    u64 culledBufferAddress;
    u64 jointMatrixBufferAddress;
    u64 entitySkinMapBufferAddress;
    u64 prevJointMatrixBufferAddress;
    u32 cascadeIndex;
} ShadowPushConstants;

/* Same vertex layout as SceneVertex (48 bytes) */
static VkVertexInputBindingDescription shadowVertexBinding = {
    .binding   = 0,
    .stride    = sizeof(SceneVertex),
    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
};

static VkVertexInputAttributeDescription shadowVertexAttrs[] = {
    {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0}, /* position */
    {.location = 1,
     .binding  = 0,
     .format   = VK_FORMAT_R32G32B32_SFLOAT,
     .offset   = 12}, /* normal (unused but keeps layout) */
    {.location = 2,
     .binding  = 0,
     .format   = VK_FORMAT_R32G32B32A32_SFLOAT,
     .offset   = 24}, /* tangent (unused) */
    {.location = 3,
     .binding  = 0,
     .format   = VK_FORMAT_R32G32_SFLOAT,
     .offset   = 40}, /* uv (for alpha test) */
    {.location = 4, .binding = 0, .format = VK_FORMAT_R32_UINT, .offset = 48},  // joints
    {.location = 5, .binding = 0, .format = VK_FORMAT_R32_UINT, .offset = 52},  // weights
};

/* ── Shadow map image ─────────────────────────────────────────────────── */

static VulkanImage shadowMapImage; /* 2D array, SHADOW_CASCADE_COUNT layers */
static VulkanImage shadowMapLayerImages[SHADOW_CASCADE_COUNT]; /* sampled pool proxy per layer */
static char shadowMapReady;
static char shadowDisabled;
static float focusDistance = 0.0f; /* player-character distance for cascade bias */

/* ── Cascade state ────────────────────────────────────────────────────── */

static mat4 cascadeViewProj[SHADOW_CASCADE_COUNT];
static float cascadeSplits[SHADOW_CASCADE_COUNT]; /* view-space far plane */

/* ── Helpers ──────────────────────────────────────────────────────────── */

static void computeCascadeSplits(float zNear, float zFar, int cascadeCount) {
    if (zFar <= zNear) zFar = zNear + 1.0f;

    /* Keep cascade 0 focused around the player/camera target, then distribute
     * the remaining cascades non-linearly out to the shadow max distance. */
    float margin     = focusDistance * 0.5f + 2.0f;
    float firstSplit = focusDistance + margin;
    if (firstSplit < zNear + 1.0f) firstSplit = zNear + 1.0f;
    if (firstSplit > zFar) firstSplit = zFar;

    cascadeSplits[0] = firstSplit;

    for (int i = 1; i < cascadeCount; i++) {
        float t = (float)i / (float)(cascadeCount - 1);
        t       = powf(t, 1.7f); /* bias resolution toward nearer cascades */
        cascadeSplits[i] = firstSplit + (zFar - firstSplit) * t;
    }
    cascadeSplits[cascadeCount - 1] = zFar;
}

/* Build an orthographic light-space view-projection for one cascade.
 * The camera frustum corners for [splitNear, splitFar] are transformed
 * into light space, then snapped to texel boundaries for stability. */
static void buildCascadeMatrix(const Camera* cam,
                               const vec3 lightDir,
                               float splitNear,
                               float splitFar,
                               int cascadeIndex) {
    /* ── 1. NDC corners → world-space frustum slice ─────────────────── */
    /* 8 NDC corners of the unit cube */
    static const vec4 ndcCorners[8] = {
        {-1, -1, 0, 1},
        {1, -1, 0, 1},
        {1, 1, 0, 1},
        {-1, 1, 0, 1},
        {-1, -1, 1, 1},
        {1, -1, 1, 1},
        {1, 1, 1, 1},
        {-1, 1, 1, 1},
    };

    vec3 worldCorners[8];
    for (int i = 0; i < 8; i++) {
        vec4 wc;
        glm_mat4_mulv((vec4*)cam->cameraUbo.invViewProjectionNoJitter, (float*)&ndcCorners[i], wc);
        glm_vec3_divs(wc, wc[3], worldCorners[i]);
    }

    /* Interpolate between near and far planes in view-space depth.
     * Reverse-Z: NDC z=1 → near, z=0 → far.  But the corners above
     * already contain the full frustum; we reslice by view-space depth. */

    /* Convert split distances to [0,1] range within [zNear, zFar] */
    float zNear = cam->cameraUbo.znear;
    float zFar  = cam->cameraUbo.zfar;
    if (zFar <= zNear) zFar = zNear + 1.0f;

    float tNear = (splitNear - zNear) / (zFar - zNear);
    float tFar  = (splitFar - zNear) / (zFar - zNear);

    /* Lerp each near↔far edge to get the sub-frustum.
     * Reverse-Z: NDC z=0 → far, z=1 → near.
     * worldCorners[0..3] = far plane, worldCorners[4..7] = near plane.
     * Interpolate from near toward far using t in [0,1]. */
    vec3 frustum[8];
    for (int i = 0; i < 4; i++) {
        vec3 dir;
        glm_vec3_sub(worldCorners[i], worldCorners[i + 4], dir); /* far - near */
        /* near plane of slice */
        glm_vec3_scale(dir, tNear, frustum[i]);
        glm_vec3_add(worldCorners[i + 4], frustum[i], frustum[i]);
        /* far plane of slice */
        glm_vec3_scale(dir, tFar, frustum[i + 4]);
        glm_vec3_add(worldCorners[i + 4], frustum[i + 4], frustum[i + 4]);
    }

    /* ── 2. Frustum center ──────────────────────────────────────────── */
    vec3 center = {0, 0, 0};
    for (int i = 0; i < 8; i++) {
        glm_vec3_add(center, frustum[i], center);
    }
    glm_vec3_divs(center, 8.0f, center);

    /* ── 3. Light view matrix (look from far away along lightDir) ──── */
    vec3 lightDirN;
    glm_vec3_copy((float*)lightDir, lightDirN);
    glm_vec3_normalize(lightDirN);

    /* Sphere radius that encloses the frustum slice (for stable sizing) */
    float radius = 0.0f;
    for (int i = 0; i < 8; i++) {
        vec3 diff;
        glm_vec3_sub(frustum[i], center, diff);
        float dist = glm_vec3_norm(diff);
        if (dist > radius) radius = dist;
    }
    /* Quantize the enclosing sphere a bit more aggressively so tiny
     * camera-rotation changes do not continuously resize the cascade. */
    radius = ceilf(radius * 4.0f) / 4.0f;

    /* Build a stable light-view basis from the light direction instead of
     * using lookAt(eye, center). The look-at construction can drift a bit as
     * the cascade center changes, which slightly rotates the shadow projection
     * and causes shimmer during camera motion/rotation. */
    vec3 upRef = {0, 1, 0};
    if (fabsf(lightDirN[1]) > 0.99f) {
        upRef[0] = 0;
        upRef[1] = 0;
        upRef[2] = 1;
    }

    vec3 lightForward;
    glm_vec3_copy(lightDirN, lightForward);
    glm_vec3_normalize(lightForward);

    vec3 lightRight;
    glm_vec3_crossn(upRef, lightForward, lightRight);

    vec3 lightUp;
    glm_vec3_cross(lightForward, lightRight, lightUp);
    glm_vec3_normalize(lightUp);

    /* Build the light-view matrix WITHOUT baking in the frustum center.
     * Translation is left at zero so that the frustum center transforms to
     * a non-zero light-space position that can be meaningfully snapped to
     * the texel grid.  If we centered the view on `center`, the transform
     * would always yield (0,0,z) and the snap would be a no-op. */
    mat4 lightView  = GLM_MAT4_IDENTITY_INIT;
    lightView[0][0] = lightRight[0];
    lightView[1][0] = lightRight[1];
    lightView[2][0] = lightRight[2];
    lightView[3][0] = 0.0f;

    lightView[0][1] = lightUp[0];
    lightView[1][1] = lightUp[1];
    lightView[2][1] = lightUp[2];
    lightView[3][1] = 0.0f;

    lightView[0][2] = lightForward[0];
    lightView[1][2] = lightForward[1];
    lightView[2][2] = lightForward[2];
    lightView[3][2] = 0.0f;

    /* ── 4. Stable light-space bounds ──────────────────────────────── */
    /* Keep XY extents fixed per cascade slice (bounding sphere), then snap
     * the orthographic center in light space to the shadow-map texel grid.
     * This is the important part for eliminating shadow swimming: the old
     * code snapped the world origin in clip space, which still lets the
     * cascade window drift as the camera rotates/moves. */
    float padding = radius * 0.1f;
    float extent  = radius + padding;
    /* Also quantize the final ortho half-extent so the projection size
     * itself stays stable instead of breathing by tiny amounts. */
    extent = ceilf(extent * 64.0f) / 64.0f;

    vec4 center4 = {center[0], center[1], center[2], 1.0f};
    vec4 centerLS4;
    glm_mat4_mulv(lightView, center4, centerLS4);

    float texelSize = (extent * 2.0f) / (float)SHADOW_MAP_SIZE;
    if (texelSize > 0.0f) {
        centerLS4[0] = roundf(centerLS4[0] / texelSize) * texelSize;
        centerLS4[1] = roundf(centerLS4[1] / texelSize) * texelSize;
    }

    /* Fit Z to the actual slice bounds in light space so casters stay in
     * range without reintroducing XY instability. */
    float minZ = FLT_MAX;
    float maxZ = -FLT_MAX;
    for (int i = 0; i < 8; i++) {
        vec4 cornerLS = {frustum[i][0], frustum[i][1], frustum[i][2], 1.0f};
        glm_mat4_mulv(lightView, cornerLS, cornerLS);
        if (cornerLS[2] < minZ) minZ = cornerLS[2];
        if (cornerLS[2] > maxZ) maxZ = cornerLS[2];
    }
    minZ -= radius * 2.0f;
    maxZ += radius * 2.0f;

    /* ── 5. Light orthographic projection ───────────────────────────── */
    mat4 lightProj;
    glm_ortho(centerLS4[0] - extent,
              centerLS4[0] + extent,
              centerLS4[1] - extent,
              centerLS4[1] + extent,
              -minZ,
              -maxZ,
              lightProj);
    /* Vulkan clip space: [0,1] depth — cglm's glm_ortho already uses
     * GLM_CLIP_CONTROL_ZO when CGLM_FORCE_DEPTH_ZERO_TO_ONE is set. */

    mat4 lightViewProjAbs;
    glm_mat4_mul(lightProj, lightView, lightViewProjAbs);

    glm_mat4_copy(lightViewProjAbs, cascadeViewProj[cascadeIndex]);
}

/* ── Image management ─────────────────────────────────────────────────── */

static void destroyShadowMap(void) {
    if (!shadowMapReady) return;

    for (int i = 0; i < SHADOW_CASCADE_COUNT; i++) {
        if (shadowMapLayerImages[i].img) {
            vulkanRemoveImageFromPool(&shadowMapLayerImages[i]);
            /* Don't vkDestroyImage — the underlying VkImage belongs to
             * shadowMapImage.  Only destroy the per-layer view. */
            vkDestroyImageView(vulkan.device, shadowMapLayerImages[i].view, NULL);
            shadowMapLayerImages[i] = VulkanImage{};
        }
    }

    if (shadowMapImage.img) {
        vulkanDestroyImage(&shadowMapImage, NULL);
        shadowMapImage = VulkanImage{};
    }

    shadowMapReady = 0;
}

static void ensureShadowMap(void) {
    if (shadowMapReady) return;

    shadowMapImage = vulkanCreateImage(.name   = "shadow_csm",
                                       .format = VK_FORMAT_D32_SFLOAT,
                                       .usage  = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                                 VK_IMAGE_USAGE_SAMPLED_BIT,
                                       .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
                                       .width  = SHADOW_MAP_SIZE,
                                       .height = SHADOW_MAP_SIZE,
                                       .layers = SHADOW_CASCADE_COUNT,
                                       .noPool = 1);

    /* Create per-layer 2D views and add each to the sampled image pool
     * so the lighting shader can sample individual cascades. */
    for (int i = 0; i < SHADOW_CASCADE_COUNT; i++) {
        /* Build a "fake" VulkanImage that shares the same VkImage but
         * has its own VkImageView (single layer). */
        VulkanImage* layer = &shadowMapLayerImages[i];
        *layer             = VulkanImage{};
        layer->img         = shadowMapImage.img;
        layer->format      = VK_FORMAT_D32_SFLOAT;
        layer->aspect      = VK_IMAGE_ASPECT_DEPTH_BIT;
        layer->extent      = shadowMapImage.extent;
        layer->layers      = 1;
        layer->mipLevels   = 1;
        layer->usage       = VK_IMAGE_USAGE_SAMPLED_BIT;
        layer->viewType    = VK_IMAGE_VIEW_TYPE_2D;
        layer->layout      = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkImageViewCreateInfo viewInfo = {
            .sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .flags      = 0,
            .pNext      = nullptr,
            .image      = shadowMapImage.img,
            .viewType   = VK_IMAGE_VIEW_TYPE_2D,
            .format     = VK_FORMAT_D32_SFLOAT,
            .components = {VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY},
            .subresourceRange =
                {
                    .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
                    .baseMipLevel   = 0,
                    .levelCount     = 1,
                    .baseArrayLayer = (u32)i,
                    .layerCount     = 1,
                },
        };
        vkCreateImageView(vulkan.device, &viewInfo, nullptr, &layer->view);
        vulkanAddImageToPool(layer);
    }

    shadowMapReady = 1;
}

/* ── Pipeline management ──────────────────────────────────────────────── */

static void recreatePipelines(void) {
    if (shadowPipe.pipe) {
        vulkanDestroyPipe(&shadowPipe);
        vulkanDestroyPipe(&shadowPipeDoubleSided);
    }

    shadowPipe = vulkanCreatePipe(.name        = "shadow_csm",
                                  .vs          = "shaders/pass/shadow/spv/shadow_csm.vert.spv",
                                  .fs          = "shaders/pass/shadow/spv/shadow_csm.frag.spv",
                                  .depthFormat = VK_FORMAT_D32_SFLOAT,
                                  .clearDepth  = {1.0f, 0},
                                  .clearDepthEnabled       = 1,
                                  .depthClamp              = 1,
                                  .noCull                  = 1,
                                  .depthCompareOp          = VK_COMPARE_OP_LESS_OR_EQUAL,
                                  .depthBiasEnable         = 1,
                                  .depthBiasConstantFactor = 0.0f,
                                  .depthBiasSlopeFactor    = 0.8f,
                                  .depthBiasClamp          = 0.0f,
                                  .vertexAttributes        = shadowVertexAttrs,
                                  .vertexAttributeCount    = 6,
                                  .vertexBindings          = &shadowVertexBinding,
                                  .vertexBindingCount      = 1);

    shadowPipeDoubleSided = vulkanCreatePipe(.name = "shadow_csm_ds",
                                             .vs   = "shaders/pass/shadow/spv/shadow_csm.vert.spv",
                                             .fs   = "shaders/pass/shadow/spv/shadow_csm.frag.spv",
                                             .depthFormat             = VK_FORMAT_D32_SFLOAT,
                                             .depthClamp              = 1,
                                             .depthCompareOp          = VK_COMPARE_OP_LESS_OR_EQUAL,
                                             .depthBiasEnable         = 1,
                                             .depthBiasConstantFactor = 0.0f,
                                             .depthBiasSlopeFactor    = 0.8f,
                                             .depthBiasClamp          = 0.0f,
                                             .noCull                  = 1,
                                             .vertexAttributes        = shadowVertexAttrs,
                                             .vertexAttributeCount    = 6,
                                             .vertexBindings          = &shadowVertexBinding,
                                             .vertexBindingCount      = 1);
}

/* ── Pass lifecycle ───────────────────────────────────────────────────── */

static void swapchainCreated(void*) {
    recreatePipelines();
}

void VulkanShadowPass::added() {
    utils::signalSubscribe("swapchainCreated", swapchainCreated);
    recreatePipelines();

    const char* env = getenv("ENGINE_SHADOW_DISABLED");
    if (env && *env && atoi(env)) shadowDisabled = 1;
}

void VulkanShadowPass::preUpdate() {
    vulkanResetProfile(vulkan.currentCmd, &shadowPipe.profile, 0);
}

/* ── Render one cascade ───────────────────────────────────────────────── */

static void renderCascade(VulkanCommand* cmd, int cascade, u32 fi) {
    /* Begin rendering into the cascade's layer */
    vulkanBeginRender(.cmd        = cmd,
                      .pipe       = &shadowPipe,
                      .depth      = &shadowMapImage,
                      .depthLayer = cascade + 1 /* 1-indexed for per-layer view */);

    vulkanViewport(cmd, 0, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, -(int)SHADOW_MAP_SIZE);
    vulkanScissor(cmd, 0, 0, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);

    u32 visibleSceneCount = 0;
    const Scene** visibleScenes = vulkanGetVisibleScenes(&visibleSceneCount);
    for (u32 si = 0; si < visibleSceneCount; si++) {
        const Scene* scene = visibleScenes[si];
        if (!scene->backendScene) continue;
        VulkanScene* vs  = static_cast<VulkanScene*>(scene->backendScene);
        if (!vs->totalDraws) continue;

        vulkanBindVertex(cmd, &vs->vertexBuffer, 0, NULL, 0, NULL, 0);
        vulkanBindIndex(cmd, &vs->indexBuffer, 0, VK_INDEX_TYPE_UINT32);

        /* Single-sided draws */
        vulkanBindPipe(cmd, &shadowPipe);
        {
            ShadowPushConstants pc = {
                .transformBufferAddress       = vs->transformBuffer[fi].address,
                .drawInstanceBufferAddress    = vs->drawInstanceBuffer.address,
                .culledBufferAddress          = vs->culledBuffer[fi].address,
                .jointMatrixBufferAddress     = vs->jointMatrixBuffer[fi].address,
                .entitySkinMapBufferAddress   = vs->entitySkinMapBuffer[fi].address,
                .prevJointMatrixBufferAddress = vs->prevJointMatrixBuffer[fi].address,
                .cascadeIndex                 = (u32)cascade,
            };
            vulkanPush(cmd, &shadowPipe, sizeof(pc), &pc);

            vkCmdDrawIndexedIndirectCount(cmd->cmd,
                                          vs->indirectBuffer[fi].buf,
                                          0,
                                          vs->drawCountBuffer[fi].buf,
                                          0,
                                          vs->totalDraws,
                                          sizeof(SceneDrawIndexedCommand));
        }

        /* Double-sided draws */
        vulkanBindPipe(cmd, &shadowPipeDoubleSided);
        {
            ShadowPushConstants pc = {
                .transformBufferAddress       = vs->transformBuffer[fi].address,
                .drawInstanceBufferAddress    = vs->drawInstanceBuffer.address,
                .culledBufferAddress          = vs->dsCulledBuffer[fi].address,
                .jointMatrixBufferAddress     = vs->jointMatrixBuffer[fi].address,
                .entitySkinMapBufferAddress   = vs->entitySkinMapBuffer[fi].address,
                .prevJointMatrixBufferAddress = vs->prevJointMatrixBuffer[fi].address,
                .cascadeIndex                 = (u32)cascade,
            };
            vulkanPush(cmd, &shadowPipeDoubleSided, sizeof(pc), &pc);

            vkCmdDrawIndexedIndirectCount(cmd->cmd,
                                          vs->dsIndirectBuffer[fi].buf,
                                          0,
                                          vs->dsDrawCountBuffer[fi].buf,
                                          0,
                                          vs->totalDraws,
                                          sizeof(SceneDrawIndexedCommand));
        }
    }

    /* Azgaar props (vegetation, settlement buildings, landmarks) live outside
     * the VulkanScene draw list, so they are drawn into this cascade with
     * their own instanced pipe to cast shadows too. */
    vulkanAzgaarPropsDrawShadow(cmd, (u32)cascade);

    vulkanEndRender(cmd);
}

/* ── Frame update ─────────────────────────────────────────────────────── */

static void uploadEmptyShadow(void) {
    ShadowUbo shadow = {};
    vulkanResourceUploadShadow(&shadow);
    vulkanShadowPass.gpuElapsed = 0;
}

void VulkanShadowPass::update() {
    if (vulkan.skipFrame) return;

    if (shadowDisabled) {
        uploadEmptyShadow();
        return;
    }

    Entity* camEntity = cameraGetEntity();
    if (!camEntity) return;
    Camera* camera = getComponent(camEntity->scene, Camera, camEntity->id);
    if (!camera) return;

    /* Get sun direction from IBL-extracted sun (points TOWARD the sun).
     * We need the direction FROM the sun (toward the scene). */
    IblSunLight iblSun = vulkanIblGetExtractedSun();
    vec3 lightDir;
    glm_vec3_negate_to(iblSun.direction, lightDir);
    glm_vec3_normalize(lightDir);

    /* If no meaningful light direction, skip shadow rendering */
    if (glm_vec3_norm(lightDir) < 0.001f) return;

    ensureShadowMap();

    VulkanCommand* cmd = vulkan.currentCmd;
    u32 fi             = renderer.flightIndex;

    vulkanBeginProfile(cmd, &shadowPipe.profile, 0);

    /* Compute cascade splits */
    float zNear = camera->cameraUbo.znear;
    float zFar  = fminf(camera->cameraUbo.zfar, SHADOW_MAX_DISTANCE);
    if (zFar <= zNear) zFar = zNear + 1.0f;
    const int activeCascadeCount = SHADOW_ACTIVE_CASCADE_COUNT;
    computeCascadeSplits(zNear, zFar, activeCascadeCount);

    /* Build per-cascade view-projection matrices */
    for (int i = 0; i < activeCascadeCount; i++) {
        float splitNear = (i == 0) ? zNear : cascadeSplits[i - 1];
        /* Extend the near of each cascade backwards by the blend fraction
         * so that cascade blending has valid data in both cascades. */
        if (i > 0) {
            float range = cascadeSplits[i] - cascadeSplits[i - 1];
            splitNear -= range * SHADOW_CASCADE_BLEND_FRACTION;
            if (splitNear < zNear) splitNear = zNear;
        }
        float splitFar = cascadeSplits[i];
        buildCascadeMatrix(camera, lightDir, splitNear, splitFar, i);
    }

    /* Transition shadow map to depth attachment for rendering */
    vulkanTransition(cmd,
                     &shadowMapImage,
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                     0,
                     SHADOW_CASCADE_COUNT);

    /* Render each active cascade.  The UBO/layout still reserves four layers,
     * but three cascades is a much better cost/quality point for this scene. */
    for (int i = 0; i < activeCascadeCount; i++) {
        renderCascade(cmd, i, fi);
    }

    /* Transition shadow map to shader read for the lighting pass */
    vulkanTransition(cmd,
                     &shadowMapImage,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     0,
                     SHADOW_CASCADE_COUNT);

    /* Upload shadow UBO */
    ShadowUbo shadow    = {};
    shadow.cascadeCount = activeCascadeCount;
    for (int i = 0; i < activeCascadeCount; i++) {
        glm_mat4_copy(cascadeViewProj[i], shadow.shadowViewProjection[i]);
        shadow.cascadeSplits[i]  = cascadeSplits[i];
        shadow.shadowMapIndex[i] = (u32)shadowMapLayerImages[i].sampledPoolIndex;
    }
    shadow.shadowParams[0] = 0.00015f; /* receiver depth bias */
    shadow.shadowParams[1] = 0.001f; /* normal bias */
    shadow.shadowParams[2] = (float)SHADOW_MAP_SIZE;
    shadow.shadowParams[3] = 1.0f / (float)SHADOW_MAP_SIZE;
    shadow.lightSize       = 0.0f;
    shadow.temporalActive  = rendererIsUpscalerEnabled() ? 1u : 0u;

    vulkanResourceUploadShadow(&shadow);

    vulkanEndProfile(cmd, &shadowPipe.profile, 0);
    vulkanShadowPass.gpuElapsed = shadowPipe.profile.elapsed;
}

void VulkanShadowPass::removed() {
    destroyShadowMap();
    if (shadowPipe.pipe) {
        vulkanDestroyPipe(&shadowPipe);
        vulkanDestroyPipe(&shadowPipeDoubleSided);
    }
}

/* ── Public API ───────────────────────────────────────────────────────── */

void vulkanShadowPassSetDisabled(char disabled) {
    shadowDisabled = disabled;
}

char vulkanShadowPassIsDisabled(void) {
    return shadowDisabled;
}

void vulkanShadowPassSetPCSS(char enabled) {
    (void)enabled;
}

char vulkanShadowPassIsPCSS(void) {
    return 0;
}

void vulkanShadowPassSetFocusDistance(float distance) {
    focusDistance = distance;
}
}  // namespace engine
