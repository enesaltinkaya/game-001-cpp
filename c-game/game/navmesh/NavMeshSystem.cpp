#include "NavMeshSystem.h"
#include "navmesh/NavMeshSystem.h"
#include "recast_c_api.h"

#include "ecs/Ecs.h"
#include "ecs/system/scene/SceneSystem.h"
#include "datamanager/DataManager.h"
#include "renderer/vulkan/pass/debug_navmesh/VulkanDebugNavMeshPass.h"

#include <string.h>

// ── Navmesh storage ──────────────────────────────────────────────────────

#include <zstd.h>
namespace game {
static NavMeshData navMesh;

// ── File loading ─────────────────────────────────────────────────────────


static std::vector<u8> decompressZstd(utils::String* compressed, u32* outSize) {
    std::vector<u8> buf;
    u64 rSize = ZSTD_getFrameContentSize(compressed->data, compressed->size);
    if (rSize == ZSTD_CONTENTSIZE_ERROR || rSize == ZSTD_CONTENTSIZE_UNKNOWN) {
        return buf;
    }
    buf.resize(static_cast<size_t>(rSize));
    u64 dSize = ZSTD_decompress(buf.data(), rSize, compressed->data, compressed->size);
    if (ZSTD_isError(dSize) != 0U) {
        return {};
    }
    *outSize = static_cast<u32>(rSize);
    return buf;
}

static bool loadNavMeshFromFile(const char* path) {
    if (!utils::dataManagerFileExists(path)) return false;

    utils::info("navMesh: loading %s", path);
    utils::String fileData = utils::dataManagerRead(path);

    u32 rawSize;
    std::vector<u8> raw = decompressZstd(&fileData, &rawSize);
    utils::stringDestroy(&fileData);
    if (raw.empty()) {
        utils::warn("navMesh: failed to decompress %s", path);
        return false;
    }

    const u8* p    = raw.data();
    const u8* end = p + rawSize;
    if (rawSize < 32 || memcmp(p, "NAVM", 4) != 0) {
        utils::warn("navMesh: invalid header");
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
            utils::warn("navMesh: no tiles in %s", path);
            return false;
        }

        std::vector<const void*> tilePtrs(tileCount);
        std::vector<u32>         tileSizes(tileCount);

        for (u32 i = 0; i < tileCount; i++) {
            u32 tileSize;
            memcpy(&tileSize, p, 4); p += 4;
            if (p + tileSize > end) {
                utils::warn("navMesh: truncated tile %u data", i);
                return false;
            }
            tilePtrs[i]  = p;
            tileSizes[i] = tileSize;
            p += tileSize;
        }

        navMesh.navMesh = rcNavMeshLoadTiled(tilePtrs.data(), tileSizes.data(), tileCount,
                                               bmin, bmax, tileWorldSize, tileWorldSize);
    } else {
        // v1 single tile
        u32 tileSize;
        memcpy(&tileSize, p, 4); p += 4;

        if (p + tileSize > end) {
            utils::warn("navMesh: truncated tile data");
            return false;
        }

        navMesh.navMesh = rcNavMeshLoad(p, tileSize);
    }

    if (!navMesh.navMesh) {
        utils::warn("navMesh: failed to load navmesh");
        return false;
    }

    navMesh.query = rcQueryCreate(navMesh.navMesh);
    if (!navMesh.query) {
        utils::warn("navMesh: failed to create query");
        rcNavMeshDestroy(navMesh.navMesh);
        navMesh.navMesh = nullptr;
        return false;
    }

    utils::info("navMesh: loaded %s (v%u)", path, version);
    engine::vulkanDebugNavMeshSetMesh(navMesh.navMesh);
    return true;
}

// ── System lifecycle ─────────────────────────────────────────────────────

void NavMeshSystem::added() {
    loadNavMeshFromFile("models/combined.nav.dat");
}

void NavMeshSystem::removed() {
    engine::vulkanDebugNavMeshSetMesh(nullptr);
    if (navMesh.query)   rcQueryDestroy(navMesh.query);
    if (navMesh.navMesh) rcNavMeshDestroy(navMesh.navMesh);
    navMesh.navMesh = nullptr;
    navMesh.query   = nullptr;
}

void NavMeshSystem::update() {
    static_cast<void>(0);
}

NavMeshSystem navMeshSystem;

NavMeshSystem::NavMeshSystem() : engine::System("navMesh") {}

// ── Public API ───────────────────────────────────────────────────────────

uint32_t navMeshFindPath(engine::Scene* scene,
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

int navMeshClosestPoint(engine::Scene* scene, const float* pos, float* outPoint) {
    static_cast<void>(scene);
    if (!navMesh.query) return 0;
    return rcQueryClosestPoint(navMesh.query, pos, outPoint);
}

RcNavMesh* navMeshGetMesh(void) {
    return navMesh.navMesh;
}
}  // namespace game
