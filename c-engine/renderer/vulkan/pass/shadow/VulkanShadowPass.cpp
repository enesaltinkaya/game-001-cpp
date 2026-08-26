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
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/scene/VulkanScene.h"
#include "renderer/vulkan/scene/VulkanVisibleScenes.h"
#include "renderer/vulkan/pass/azgaar_props/VulkanAzgaarPropsPass.h"
#include <float.h>
#include <math.h>

/* ── Constants ────────────────────────────────────────────────────────── */

#define SHADOW_CASCADE_BLEND_FRACTION 0.3f

/* ── Forward declarations ─────────────────────────────────────────────── */

namespace engine {

/* Per-quality map size (texels), active cascade count and shadow cast range
 * (metres).  The raster CSM redraws the visible casters once per cascade, so
 * the cost of a level scales with its cascade count as much as its map size. */
typedef struct ShadowQualityParams {
    int   mapSize;
    int   cascadeCount;
    float maxDistance;
} ShadowQualityParams;

static const ShadowQualityParams kShadowQualityTable[SHADOW_QUALITY_COUNT - 1] = {
    /* LOW    */ {1024, 1, 40.0f},
    /* MEDIUM */ {2048, 2, 80.0f},
    /* HIGH   */ {4096, 3, 160.0f},
};

static ShadowQualityParams shadowQualityParams(ShadowQuality quality) {
    if (quality == SHADOW_QUALITY_OFF) {
        ShadowQualityParams zero = {0, 0, 0.0f};
        return zero;
    }
    return kShadowQualityTable[quality - 1];
}

static const char* shadowQualityName(ShadowQuality quality) {
    switch (quality) {
        case SHADOW_QUALITY_OFF:    return "off";
        case SHADOW_QUALITY_LOW:    return "low";
        case SHADOW_QUALITY_MEDIUM: return "medium";
        case SHADOW_QUALITY_HIGH:   return "high";
        case SHADOW_QUALITY_COUNT:
        default:                    return "?";
    }
}

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
static int shadowMapSize; /* live image size in texels, 0 when destroyed */
static ShadowQuality shadowQuality = SHADOW_QUALITY_MEDIUM;
static float focusDistance = 0.0f; /* player-character distance for cascade bias */

/* ── Cascade state ────────────────────────────────────────────────────── */

static mat4 cascadeViewProj[SHADOW_CASCADE_COUNT];
static float cascadeSplits[SHADOW_CASCADE_COUNT]; /* view-space far plane */
static vec3 cascadeLightDir; /* toward the scene (-sun.direction) */

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

    float texelSize = (extent * 2.0f) / (float)shadowMapSize;
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

/* A retired shadow map whose actual destruction is deferred: the views and
 * image stay alive until the flight command buffer that recorded the last
 * frame referencing them has completed (see retiredShadowMaps). */
struct RetiredShadowMap {
    VulkanImage image;
    VulkanImage layers[SHADOW_CASCADE_COUNT];
    int flight; /* flight item that recorded the last referencing frame */
};
static std::vector<RetiredShadowMap> retiredShadowMaps;

/* Destroy a retired shadow map.  Only call this when no in-flight command
 * buffer can reference the image anymore: either the flight item that
 * recorded the last referencing frame has completed (its fence was waited on
 * by vulkanSwapchainBegin before this frame's passes ran), or the device was
 * drained at teardown. */
static void destroyRetiredShadowMap(RetiredShadowMap& retired) {
    for (int i = 0; i < SHADOW_CASCADE_COUNT; i++) {
        if (retired.layers[i].img) {
            /* Don't vkDestroyImage here — the underlying VkImage belongs to
             * retired.image (destroyed below).  Only destroy the per-layer view. */
            vkDestroyImageView(vulkan.device, retired.layers[i].view, NULL);
        }
    }
    if (retired.image.img) {
        vulkanDestroyImage(&retired.image, NULL);
    }
}

/* Retire the live shadow map: re-point its bindless pool entries at the
 * engine dummy and defer the actual view/image destruction until the flight
 * command buffer that recorded the last referencing frame (the current
 * frame) has completed.  In-flight frames (submitted before the current
 * frame) may still be reading the shadow map, so drain the device first —
 * same pattern as the DOF / swapchain mid-session recreate.  At teardown the
 * renderer has already drained, so waitIdleFirst is false there. */
static void retireShadowMap(char waitIdleFirst) {
    if (!shadowMapReady) return;

    if (waitIdleFirst) vulkanWaitIdle("shadow map destroy");

    RetiredShadowMap retired = {};
    retired.image            = shadowMapImage;
    for (int i = 0; i < SHADOW_CASCADE_COUNT; i++) {
        retired.layers[i] = shadowMapLayerImages[i];
    }
    /* The current frame's flight cmd was already recorded (the shadow pass
     * rendered into the map, the lighting pass bound the global set with the
     * per-layer views) and is only submitted at the end of this frame —
     * AFTER the waitIdle above — so it is the last command buffer that
     * directly references the image.  It completes when this flight item is
     * reused, i.e. when renderer.flightIndex wraps back to this value. */
    retired.flight           = renderer.flightIndex;
    shadowMapImage           = VulkanImage{};
    for (int i = 0; i < SHADOW_CASCADE_COUNT; i++) {
        shadowMapLayerImages[i] = VulkanImage{};
    }
    shadowMapReady           = 0;
    shadowMapSize            = 0;

    for (int i = 0; i < SHADOW_CASCADE_COUNT; i++) {
        if (retired.layers[i].img) {
            /* Every frame binds the global set, whose sampled-image array
             * still points at these views.  Free the slot and re-point the
             * entries at the dummy (alive for the whole renderer lifetime) so
             * the current + future command buffers never reference a view we
             * are about to destroy. */
            vulkanRetireSampledPoolEntry(retired.layers[i].sampledPoolIndex,
                                         &vulkanResources.dummyImage);
            retired.layers[i].inPool = 0;
        }
    }

    retiredShadowMaps.push_back(retired);
}

/* Destroy the live shadow map after a mid-session change (quality off or map
 * size) retired it; the actual destruction is deferred until the last
 * referencing flight command buffer has completed. */
static void destroyShadowMap(void) {
    retireShadowMap(1);
}

/* Create (or recreate, when the quality level's map size changed) the shadow
 * map.  The layer count is always SHADOW_CASCADE_COUNT so the per-layer views,
 * UBO slots and layout transitions stay stable across quality changes. */
static void ensureShadowMap(int size) {
    if (shadowMapReady && shadowMapSize == size) return;
    destroyShadowMap();

    shadowMapImage = vulkanCreateImage(.name   = "shadow_csm",
                                       .format = VK_FORMAT_D32_SFLOAT,
                                       .usage  = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                                 VK_IMAGE_USAGE_SAMPLED_BIT |
                                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                       .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
                                       .width  = size,
                                       .height = size,
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
    shadowMapSize  = size;
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
    if (env && *env && atoi(env)) vulkanShadowPassSetQuality(SHADOW_QUALITY_OFF);
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

    vulkanViewport(cmd, 0, shadowMapSize, shadowMapSize, -shadowMapSize);
    vulkanScissor(cmd, 0, 0, shadowMapSize, shadowMapSize);

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

    /* Flush deferred shadow map destructions.  vulkanSwapchainBegin has
     * already waited on the current flight item's fence (and skipped frames
     * return above, before any fence wait), so a retired map whose last
     * referencing frame used this flight item is now safe to destroy. */
    for (size_t i = 0; i < retiredShadowMaps.size(); ) {
        if (retiredShadowMaps[i].flight == renderer.flightIndex) {
            destroyRetiredShadowMap(retiredShadowMaps[i]);
            retiredShadowMaps.erase(retiredShadowMaps.begin() + i);
        } else {
            i++;
        }
    }

    if (shadowQuality == SHADOW_QUALITY_OFF) {
        uploadEmptyShadow();
        return;
    }

    const ShadowQualityParams qs = shadowQualityParams(shadowQuality);

    Entity* camEntity = cameraGetEntity();
    if (!camEntity) return;
    Camera* camera = getComponent(camEntity->scene, Camera, camEntity->id);
    if (!camera) return;

    /* Fixed scene sun (points TOWARD the sun).
     * We need the direction FROM the sun (toward the scene). */
    RendererSunLight sun = rendererGetSun();
    vec3 lightDir;
    glm_vec3_negate_to(sun.direction, lightDir);
    glm_vec3_normalize(lightDir);

    /* If no meaningful light direction, skip shadow rendering */
    if (glm_vec3_norm(lightDir) < 0.001f) return;

    glm_vec3_copy(lightDir, cascadeLightDir);

    ensureShadowMap(qs.mapSize);

    VulkanCommand* cmd = vulkan.currentCmd;
    u32 fi             = renderer.flightIndex;

    vulkanBeginProfile(cmd, &shadowPipe.profile, 0);

    /* Compute cascade splits */
    float zNear = camera->cameraUbo.znear;
    float zFar  = fminf(camera->cameraUbo.zfar, qs.maxDistance);
    if (zFar <= zNear) zFar = zNear + 1.0f;
    const int activeCascadeCount = qs.cascadeCount;
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

    /* Render each active cascade.  The UBO and the 4-layer image always
     * reserve the full cascade count; only the active prefix is rendered. */
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
    shadow.shadowParams[2] = (float)shadowMapSize;
    shadow.shadowParams[3] = 1.0f / (float)shadowMapSize;
    shadow.lightSize       = 0.0f;
    shadow.temporalActive  = rendererIsUpscalerEnabled() ? 1u : 0u;

    vulkanResourceUploadShadow(&shadow);

    vulkanEndProfile(cmd, &shadowPipe.profile, 0);
    vulkanShadowPass.gpuElapsed = shadowPipe.profile.elapsed;
}

void VulkanShadowPass::removed() {
    /* Teardown: the device was drained by vulkanDestroyDelayed before the
     * passes are removed, so retire (without waiting) and flush everything
     * immediately. */
    retireShadowMap(0);
    for (RetiredShadowMap& retired : retiredShadowMaps) {
        destroyRetiredShadowMap(retired);
    }
    retiredShadowMaps.clear();
    if (shadowPipe.pipe) {
        vulkanDestroyPipe(&shadowPipe);
        vulkanDestroyPipe(&shadowPipeDoubleSided);
    }
}

/* ── Public API ───────────────────────────────────────────────────────── */

void vulkanShadowPassSetQuality(ShadowQuality quality) {
    if (quality < SHADOW_QUALITY_OFF || quality >= SHADOW_QUALITY_COUNT) return;
    if (quality == shadowQuality) return;

    ShadowQuality previous = shadowQuality;
    shadowQuality = quality;

    /* Free the shadow map while shadows are off; switching between active
     * levels recreates the image lazily in update() at the new size. */
    if (quality == SHADOW_QUALITY_OFF) {
        destroyShadowMap();
    }
    utils::info("vulkanShadowPass: shadow quality %s -> %s",
                shadowQualityName(previous), shadowQualityName(quality));
}

ShadowQuality vulkanShadowPassGetQuality(void) {
    return shadowQuality;
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
