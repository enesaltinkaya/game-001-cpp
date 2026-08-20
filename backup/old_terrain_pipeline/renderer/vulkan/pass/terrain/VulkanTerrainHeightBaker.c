#include "VulkanTerrainHeightBaker.h"
#include "renderer/vulkan/scene/VulkanTerrain.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/command/VulkanCommand.h"
#include "renderer/vulkan/pipeline/VulkanPipe.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "renderer/vulkan/resources/VulkanImage.h"
#include "renderer/vulkan/resources/VulkanBuffer.h"
#include "renderer/vulkan/scene/VulkanScene.h"
#include "Utils.h"
#include <math.h>
#include <float.h>

// ── GPU pipeline for the heightfield texture (used by probe bake compute) ──

static VulkanPipe bakePipe;
static bool pipeCreated;

static VkVertexInputBindingDescription vertexBinding = {
    .binding   = 0,
    .stride    = sizeof(SceneVertex),
    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
};

static VkVertexInputAttributeDescription vertexAttrs[] = {
    {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0},
    {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 12},
    {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 24},
    {.location = 3, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,     .offset = 40},
};

typedef struct HeightmapPC {
    mat4 orthoProj;
    vec4 boundsMin;
    vec4 boundsMax;
    vec4 cameraPos;
} HeightmapPC;

// ── CPU heightfield rasterizer (high-res, per-cell-center sampling) ────────

// Rasterize a single triangle into the height grid, keeping the maximum Y.
static void rasterizeTriangle(float* grid, u32 gridW, u32 gridH,
                               float minX, float minZ, float spacing,
                               float x0, float y0, float z0,
                               float x1, float y1, float z1,
                               float x2, float y2, float z2) {
    // Bounding box in grid coordinates
    float fMinGx = fminf(fminf(x0, x1), x2);
    float fMaxGx = fmaxf(fmaxf(x0, x1), x2);
    float fMinGz = fminf(fminf(z0, z1), z2);
    float fMaxGz = fmaxf(fmaxf(z0, z1), z2);

    i32 gxMin = (i32)floorf((fMinGx - minX) / spacing);
    i32 gxMax = (i32)floorf((fMaxGx - minX) / spacing);
    i32 gzMin = (i32)floorf((fMinGz - minZ) / spacing);
    i32 gzMax = (i32)floorf((fMaxGz - minZ) / spacing);

    if (gxMin < 0) gxMin = 0;
    if (gzMin < 0) gzMin = 0;
    if (gxMax >= (i32)gridW) gxMax = (i32)gridW - 1;
    if (gzMax >= (i32)gridH) gzMax = (i32)gridH - 1;

    // Edge function
    float e01x = x1 - x0, e01z = z1 - z0;
    float e12x = x2 - x1, e12z = z2 - z1;
    float e20x = x0 - x2, e20z = z0 - z2;
    float area = e01x * e20z - e01z * e20x; // 2x triangle area in XZ
    if (fabsf(area) < 1e-8f) return;         // degenerate triangle
    float invArea = 1.0f / area;

    for (i32 gz = gzMin; gz <= gzMax; gz++) {
        for (i32 gx = gxMin; gx <= gxMax; gx++) {
            // Sample point at grid cell center
            float sx = minX + (gx + 0.5f) * spacing;
            float sz = minZ + (gz + 0.5f) * spacing;

            // Barycentric coordinates (XZ only)
            float d0x = sx - x0, d0z = sz - z0;
            float w0 = (e12x * d0z - e12z * d0x) * invArea; // weight for v0
            float d1x = sx - x1, d1z = sz - z1;
            float w1 = (e20x * d1z - e20z * d1x) * invArea; // weight for v1
            float w2 = 1.0f - w0 - w1;                       // weight for v2

            // Inside triangle? (with generous tolerance for grid-aligned sampling)
            if (w0 < -0.01f || w1 < -0.01f || w2 < -0.01f) continue;

            // Interpolate Y at cell center
            float h = w0 * y0 + w1 * y1 + w2 * y2;

            // Keep maximum height (highest overlapping triangle at this cell)
            u32 idx = (u32)gz * gridW + (u32)gx;
            if (h > grid[idx]) grid[idx] = h;
        }
    }
}

static float* cpuBakeHeightfield(Terrain* terrain, u32 gridW, u32 gridH,
                                 float minX, float minZ, float spacing) {
    u32 totalCells = gridW * gridH;
    float* grid = memoryAlloc(totalCells * sizeof(float));

    // Initialize to min — we'll keep the maximum height per cell
    for (u32 i = 0; i < totalCells; i++) grid[i] = FLT_MIN;

    for (u32 ci = 0; ci < arraySize(terrain->chunks); ci++) {
        TerrainChunk* chunk = &terrain->chunks[ci];
        float* pos = chunk->positions;
        u32* idx   = chunk->indices;

        if (chunk->indexCount > 0) {
            // Indexed geometry
            for (u32 t = 0; t < chunk->indexCount; t += 3) {
                u32 i0 = idx[t + 0], i1 = idx[t + 1], i2 = idx[t + 2];
                rasterizeTriangle(grid, gridW, gridH,
                                  minX, minZ, spacing,
                                  pos[i0 * 3], pos[i0 * 3 + 1], pos[i0 * 3 + 2],
                                  pos[i1 * 3], pos[i1 * 3 + 1], pos[i1 * 3 + 2],
                                  pos[i2 * 3], pos[i2 * 3 + 1], pos[i2 * 3 + 2]);
            }
        } else {
            // Non-indexed (triangle list)
            for (u32 t = 0; t + 2 < chunk->vertexCount; t += 3) {
                rasterizeTriangle(grid, gridW, gridH,
                                  minX, minZ, spacing,
                                  pos[t * 3],     pos[t * 3 + 1],     pos[t * 3 + 2],
                                  pos[(t+1) * 3], pos[(t+1) * 3 + 1], pos[(t+1) * 3 + 2],
                                  pos[(t+2) * 3], pos[(t+2) * 3 + 1], pos[(t+2) * 3 + 2]);
            }
        }
    }

    // Replace FLT_MIN with 0 for cells that weren't covered
    for (u32 i = 0; i < totalCells; i++) {
        if (grid[i] == FLT_MIN) grid[i] = 0.0f;
    }

    // Fill gaps: uncovered cells get average of covered neighbors
    float* filled = memoryAlloc(totalCells * sizeof(float));
    memcpy(filled, grid, totalCells * sizeof(float));
    for (u32 z = 1; z < gridH - 1; z++) {
        for (u32 x = 1; x < gridW - 1; x++) {
            if (grid[z * gridW + x] == 0.0f) {
                float sum = 0.0f;
                int count = 0;
                for (int dz = -1; dz <= 1; dz++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        float h = grid[(z + dz) * gridW + (x + dx)];
                        if (h > 0.0f) { sum += h; count++; }
                    }
                }
                if (count > 0) filled[z * gridW + x] = sum / count;
            }
        }
    }
    memoryFree(grid);
    grid = filled;

    return grid;
}

// ── GPU heightfield texture bake (for probe grid compute shader) ────────────

static void gpuBakeHeightfieldTexture(Terrain* terrain, VulkanTerrain* vt,
                                       TerrainHeightfield* hf) {
    float centerX = (terrain->boundsMin[0] + terrain->boundsMax[0]) * 0.5f;
    float centerZ = (terrain->boundsMin[2] + terrain->boundsMax[2]) * 0.5f;
    float minY    = terrain->boundsMin[1];
    float maxY    = terrain->boundsMax[1];

    if (!pipeCreated) {
        bakePipe = vulkanCreatePipe(
            .name                 = "heightfieldBake",
            .vs                   = "shaders/pass/heightmap/spv/heightmap.vert.spv",
            .fs                   = "shaders/pass/heightmap/spv/heightmap.frag.spv",
            .colorFormat1         = VK_FORMAT_R32_SFLOAT,
            .depthFormat          = VK_FORMAT_D32_SFLOAT,
            .clearColor1          = {0, 0, 0, 0},
            .clearColor1Enabled   = 1,
            .clearDepth           = {0.0f, 0.0f},
            .clearDepthEnabled    = 1,
            .noCull               = 1,
            .vertexAttributes     = vertexAttrs,
            .vertexAttributeCount = 4,
            .vertexBindings       = &vertexBinding,
            .vertexBindingCount   = 1,
        );
        pipeCreated = true;
    }

    VulkanImage target = vulkanCreateImage(
        .name   = "HeightfieldBake",
        .format = VK_FORMAT_R32_SFLOAT,
        .usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                | VK_IMAGE_USAGE_SAMPLED_BIT,
        .width  = (int)hf->gridW,
        .height = (int)hf->gridH,
    );

    VulkanImage depthImg = vulkanCreateImage(
        .name   = "HeightfieldBakeDepth",
        .format = VK_FORMAT_D32_SFLOAT,
        .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
        .usage  = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .width  = (int)hf->gridW,
        .height = (int)hf->gridH,
    );

    mat4 viewProj;
    {
        mat4 view;
        vec3 eye    = {centerX, maxY + 100.0f, centerZ};
        vec3 center = {centerX, minY, centerZ};
        vec3 up     = {0.0f, 0.0f, 1.0f};
        glm_lookat(eye, center, up, view);

        mat4 proj;
        glm_ortho(terrain->boundsMin[0], terrain->boundsMax[0],
                  terrain->boundsMin[2], terrain->boundsMax[2],
                  minY, maxY, proj);

        glm_mat4_mul(proj, view, viewProj);
    }

    HeightmapPC pc;
    glm_mat4_copy(viewProj, pc.orthoProj);
    glm_vec4_copy3(terrain->boundsMin, pc.boundsMin);
    glm_vec4_copy3(terrain->boundsMax, pc.boundsMax);
    pc.cameraPos[0] = centerX;
    pc.cameraPos[1] = maxY + 100.0f;
    pc.cameraPos[2] = centerZ;
    pc.cameraPos[3] = 0.0f;

    VulkanCommand* cmd = vulkanTransientBegin();

    vulkanTransition(cmd, &target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
    vulkanTransition(cmd, &depthImg, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, 1);

    vulkanBeginRender(.cmd = cmd, .pipe = &bakePipe,
                      .color1 = &target, .depth = &depthImg);
    vulkanViewport(cmd, 0, (int)hf->gridH, (int)hf->gridW, -((i32)hf->gridH));
    vulkanScissor(cmd, 0, 0, (int)hf->gridW, (int)hf->gridH);
    vulkanBindPipe(cmd, &bakePipe);
    vulkanPush(cmd, &bakePipe, sizeof(pc), &pc);

    vulkanBindVertex(cmd, &vt->vertexBuffer, 0, NULL, 0, NULL, 0);
    vulkanBindIndex(cmd, &vt->indexBuffer, 0, VK_INDEX_TYPE_UINT32);

    for (u32 ci = 0; ci < vt->chunkCount; ci++) {
        TerrainChunkDraw* chunk = &vt->chunks[ci];
        vkCmdDrawIndexed(cmd->cmd, chunk->indexCount, 1,
                         chunk->firstIndex, chunk->vertexOffset, 0);
    }
    vulkanEndRender(cmd);

    vulkanTransition(cmd, &target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1);

    vulkanTransientEnd(cmd, 1);

    addImageGarbage(&depthImg, cmd->fence, &cmd->submitted);

    vt->heightfieldTexture = target;
}

// ── Public API ──────────────────────────────────────────────────────────────

void vulkanTerrainHeightBakerBake(Terrain* terrain, float spacing) {
    VulkanTerrain* vt = (VulkanTerrain*)terrain->backendData;
    assert(vt && vt->uploaded && "terrain must be uploaded before heightfield bake");

    TerrainHeightfield* hf = &terrain->heightfield;
    hf->minX    = terrain->boundsMin[0];
    hf->minZ    = terrain->boundsMin[2];
    hf->sizeX   = terrain->boundsMax[0] - hf->minX;
    hf->sizeZ   = terrain->boundsMax[2] - hf->minZ;
    hf->spacing = spacing;
    hf->gridW   = (u32)ceilf(hf->sizeX / spacing);
    hf->gridH   = (u32)ceilf(hf->sizeZ / spacing);

    if (hf->gridW < 2 || hf->gridH < 2) {
        warn("terrainHeightBaker: grid too small (%ux%u), skipping", hf->gridW, hf->gridH);
        return;
    }

    // Free old CPU heightfield
    if (hf->heights) { memoryFree(hf->heights); hf->heights = NULL; }

    // CPU rasterize — barycentric interpolation at cell centers (accurate surface height)
    float* grid = cpuBakeHeightfield(terrain, hf->gridW, hf->gridH,
                                      hf->minX, hf->minZ, hf->spacing);
    hf->heights = grid;

    // GPU bake — renders highest-Y texture for probe grid compute shader
    gpuBakeHeightfieldTexture(terrain, vt, hf);

    // Log stats
    float hMin = hf->heights[0], hMax = hf->heights[0];
    for (u32 i = 1; i < hf->gridW * hf->gridH; i++) {
        if (hf->heights[i] < hMin) hMin = hf->heights[i];
        if (hf->heights[i] > hMax) hMax = hf->heights[i];
    }

    info("terrainHeightBaker: baked %ux%u heightfield (%.1f MB), "
         "heights[min]=%.1f heights[max]=%.1f",
         hf->gridW, hf->gridH, (double)(hf->gridW * hf->gridH * sizeof(float) / (1024.0f * 1024.0f)),
         hMin, hMax);
}

void vulkanTerrainHeightBakerDestroy(void) {
    if (pipeCreated) {
        vulkanDestroyPipe(&bakePipe);
        pipeCreated = false;
    }
}
