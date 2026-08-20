#include "navmesh/NavMeshSystem.h"
#include "recast_c_api.h"

#include "ecs/Ecs.h"
#include "ecs/system/scene/SceneSystem.h"
#include "datamanager/DataManager.h"
#include "renderer/vulkan/pass/debug_navmesh/VulkanDebugNavMeshPass.h"

#include <string.h>

// ── Navmesh storage ──────────────────────────────────────────────────────

static NavMeshData navMesh;

// ── File loading ─────────────────────────────────────────────────────────

#include <zstd.h>

static void* decompressZstd(String* compressed, u32* outSize) {
    u64 rSize = ZSTD_getFrameContentSize(compressed->data, compressed->size);
    if (rSize == ZSTD_CONTENTSIZE_ERROR || rSize == ZSTD_CONTENTSIZE_UNKNOWN) {
        return nullptr;
    }
    void* buf = memoryAlloc(rSize);
    u64 dSize = ZSTD_decompress(buf, rSize, compressed->data, compressed->size);
    if (ZSTD_isError(dSize) != 0U) {
        memoryFree(buf);
        return nullptr;
    }
    *outSize = static_cast<u32>(rSize);
    return buf;
}

static bool loadNavMeshFromFile(const char* path) {
    if (!dataManagerFileExists(path)) return false;

    info("navMesh: loading %s", path);
    String fileData = dataManagerRead(path);

    u32 rawSize;
    void* raw = decompressZstd(&fileData, &rawSize);
    stringDestroy(&fileData);
    if (!raw) {
        warn("navMesh: failed to decompress %s", path);
        return false;
    }

    const u8* p    = static_cast<const u8*>(raw);
    const u8* end = p + rawSize;
    if (rawSize < 32 || memcmp(p, "NAVM", 4) != 0) {
        warn("navMesh: invalid header");
        memoryFree(raw);
        return false;
    }
    p += 4; // magic
    u32 version;
    memcpy(&version, p, 4); p += 4;

    float bmin[3], bmax[3];
    memcpy(bmin, p, sizeof(float) * 3); p += sizeof(float) * 3;
    memcpy(bmax, p, sizeof(float) * 3); p += sizeof(float) * 3;

    if (version == 2) {
        u32 tileCount;
        memcpy(&tileCount, p, 4); p += 4;

        float tileWorldSize;
        memcpy(&tileWorldSize, p, sizeof(float)); p += sizeof(float);

        if (tileCount == 0) {
            warn("navMesh: no tiles in %s", path);
            memoryFree(raw);
            return false;
        }

        const void** tilePtrs   = static_cast<const void**>(memoryAlloc(sizeof(void*) * tileCount));
        u32*         tileSizes = static_cast<u32*>(memoryAlloc(sizeof(u32) * tileCount));

        for (u32 i = 0; i < tileCount; i++) {
            u32 tileSize;
            memcpy(&tileSize, p, 4); p += 4;
            if (p + tileSize > end) {
                warn("navMesh: truncated tile %u data", i);
                memoryFree(tilePtrs);
                memoryFree(tileSizes);
                memoryFree(raw);
                return false;
            }
            tilePtrs[i]  = p;
            tileSizes[i] = tileSize;
            p += tileSize;
        }

        navMesh.navMesh = rcNavMeshLoadTiled(tilePtrs, tileSizes, tileCount,
                                               bmin, bmax, tileWorldSize, tileWorldSize);
        memoryFree(tilePtrs);
        memoryFree(tileSizes);
    } else {
        // v1 single tile
        u32 tileSize;
        memcpy(&tileSize, p, 4); p += 4;

        if (p + tileSize > end) {
            warn("navMesh: truncated tile data");
            memoryFree(raw);
            return false;
        }

        navMesh.navMesh = rcNavMeshLoad(p, tileSize);
    }

    if (!navMesh.navMesh) {
        warn("navMesh: failed to load navmesh");
        memoryFree(raw);
        return false;
    }

    navMesh.query = rcQueryCreate(navMesh.navMesh);
    if (!navMesh.query) {
        warn("navMesh: failed to create query");
        rcNavMeshDestroy(navMesh.navMesh);
        navMesh.navMesh = nullptr;
        memoryFree(raw);
        return false;
    }

    info("navMesh: loaded %s (v%u)", path, version);
    vulkanDebugNavMeshSetMesh(navMesh.navMesh);
    memoryFree(raw);
    return true;
}

// ── System lifecycle ─────────────────────────────────────────────────────

static void added(void) {
    loadNavMeshFromFile("models/combined.nav.dat");
}

static void removed(void) {
    vulkanDebugNavMeshSetMesh(nullptr);
    if (navMesh.query)   rcQueryDestroy(navMesh.query);
    if (navMesh.navMesh) rcNavMeshDestroy(navMesh.navMesh);
    navMesh.navMesh = nullptr;
    navMesh.query   = nullptr;
}

static void update(void) {
    static_cast<void>(0);
}

System navMeshSystem = {
    .name                = "navMesh",
    .added               = added,
    .removed             = removed,
    .preUpdate           = nullptr,
    .update              = update,
    .postUpdate          = nullptr,
    .cpuElapsedLastFrame = 0.0,
    .cpuElapsed          = 0.0,
    .gpuElapsed          = 0.0,
    .priority            = 1201,
};

// ── Public API ───────────────────────────────────────────────────────────

uint32_t navMeshFindPath(Scene* scene,
                          const float* startPos,
                          const float* endPos,
                          float* outPath,
                          uint32_t maxPath) {
    static_cast<void>(scene);
    if (!navMesh.query) return 0;
    uint32_t count = rcQueryFindPath(navMesh.query, startPos, endPos, outPath, maxPath);
    if (count > 0) {
        return count;
    }
    return 0;
}

int navMeshClosestPoint(Scene* scene, const float* pos, float* outPoint) {
    static_cast<void>(scene);
    if (!navMesh.query) return 0;
    return rcQueryClosestPoint(navMesh.query, pos, outPoint);
}

RcNavMesh* navMeshGetMesh(void) {
    return navMesh.navMesh;
}
