/*
 * vegetation-builder: Offline vegetation preprocessing tool.
 *
 * v1:
 *   - parse packed terrain GLB/GLTF
 *   - decompress meshopt buffers if needed
 *   - read vegetation group names (currently grass*) from JSON sidecar
 *   - load matching UDIM PNG tiles from sibling source directories
 *   - scatter deterministic vegetation instances from terrain triangles
 *   - bucket instances into the engine's 10x10 terrain tile grid
 *   - write a baked binary sidecar for future runtime integration
 *
 * Usage:
 *   vegetation-builder [--dry-run] <input.glb> <output.vegetation> <vegetationGroups.json> <sourceTerrainDir>
 */

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#include "meshoptimizer.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace fs = std::filesystem;

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;

static constexpr u8 MAGIC[4] = {'V', 'E', 'G', '1'};
static constexpr u32 VERSION = 1;
static constexpr int VEG_TILES_PER_AXIS = 10;
static constexpr int VEG_TILE_COUNT = VEG_TILES_PER_AXIS * VEG_TILES_PER_AXIS;
static constexpr float VEG_SLOPE_THRESHOLD = 0.7f;
static constexpr float VEG_BASE_DENSITY = 1.10f; /* tuned for visible grass coverage */
static constexpr u32 MAX_INSTANCES_PER_TRIANGLE = 128;
static constexpr u32 MAX_INSTANCES_PER_TILE = 300000;
static constexpr u64 MAX_TOTAL_INSTANCES = 2000000;
static constexpr float TAU = 6.28318530718f;

struct Vec2 {
    float x, y;
};

struct Vec3 {
    float x, y, z;
};

struct BakedInstance {
    float pos[3];
    float rotation;
    float scale;
    u8 groupIndex;
    u8 channel;
    u8 pad[2];
    float normal[3];
    float pad2;
};

static_assert(sizeof(BakedInstance) == 40, "BakedInstance size changed unexpectedly");

struct ImageRgba {
    int width = 0;
    int height = 0;
    std::vector<u8> pixels;
};

struct GroupMaps {
    std::string name;
    u32 groupIndex = 0;
    std::unordered_map<int, ImageRgba> udimImages;
};

struct BakeStats {
    u64 triCount = 0;
    u64 slopeRejected = 0;
    u64 mapRejected = 0;
    u64 trianglesWithGrass = 0;
    u64 requestedInstances = 0;
    u64 generatedInstances = 0;
    u64 cappedByTriangle = 0;
    u64 cappedByTile = 0;
    u64 cappedByTotal = 0;
    u64 sampleLogs = 0;
    u8 minR = 255, minG = 255, minB = 255, minA = 255;
    u8 maxR = 0,   maxG = 0,   maxB = 0,   maxA = 0;
};

static void write_u32(FILE* f, u32 v) { fwrite(&v, 4, 1, f); }
static void write_u64(FILE* f, u64 v) { fwrite(&v, 8, 1, f); }
static void write_f32(FILE* f, float v) { fwrite(&v, 4, 1, f); }

static Vec3 vec3_add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static Vec3 vec3_sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static Vec3 vec3_scale(Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
static float vec3_dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 vec3_cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static float vec3_len(Vec3 v) { return std::sqrt(vec3_dot(v, v)); }
static Vec3 vec3_norm(Vec3 v) {
    float len = vec3_len(v);
    if (len <= 1e-8f) return {0.0f, 1.0f, 0.0f};
    return vec3_scale(v, 1.0f / len);
}

static u32 hash_u32(u32 a, u32 b, u32 c) {
    u32 state = a * 747796405u + b * 2891336453u + c * 277803737u + 1u;
    u32 word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

static float hash_float01(u32 a, u32 b, u32 c) {
    return (float)hash_u32(a, b, c) / 4294967296.0f;
}

static int decompress_meshopt(cgltf_data* data) {
    for (cgltf_size i = 0; i < data->buffer_views_count; ++i) {
        if (!data->buffer_views[i].has_meshopt_compression) continue;
        cgltf_meshopt_compression* mc = &data->buffer_views[i].meshopt_compression;
        const unsigned char* source   = (const unsigned char*)mc->buffer->data;
        if (!source) return 0;
        source += mc->offset;

        void* decoded = std::malloc(mc->count * mc->stride);
        if (!decoded) return 0;

        int rc = -1;
        switch (mc->mode) {
        case cgltf_meshopt_compression_mode_attributes:
            rc = meshopt_decodeVertexBuffer(decoded, mc->count, mc->stride, source, mc->size);
            break;
        case cgltf_meshopt_compression_mode_triangles:
            rc = meshopt_decodeIndexBuffer(decoded, mc->count, mc->stride, source, mc->size);
            break;
        case cgltf_meshopt_compression_mode_indices:
            rc = meshopt_decodeIndexSequence(decoded, mc->count, mc->stride, source, mc->size);
            break;
        default: break;
        }
        if (rc != 0) {
            std::free(decoded);
            return 0;
        }

        switch (mc->filter) {
        case cgltf_meshopt_compression_filter_octahedral:
            meshopt_decodeFilterOct(decoded, mc->count, mc->stride);
            break;
        case cgltf_meshopt_compression_filter_quaternion:
            meshopt_decodeFilterQuat(decoded, mc->count, mc->stride);
            break;
        case cgltf_meshopt_compression_filter_exponential:
            meshopt_decodeFilterExp(decoded, mc->count, mc->stride);
            break;
        default: break;
        }

        data->buffer_views[i].data = decoded;
    }
    return 1;
}

static bool parseGltf(const char* inputPath, cgltf_data** outData) {
    cgltf_options options = {};
    cgltf_result result = cgltf_parse_file(&options, inputPath, outData);
    if (result != cgltf_result_success) {
        std::fprintf(stderr, "vegetation-builder: failed to parse %s (err %d)\n", inputPath, result);
        return false;
    }

    result = cgltf_load_buffers(&options, *outData, inputPath);
    if (result != cgltf_result_success) {
        std::fprintf(stderr, "vegetation-builder: failed to load buffers (err %d)\n", result);
        cgltf_free(*outData);
        *outData = nullptr;
        return false;
    }

    if (!decompress_meshopt(*outData)) {
        std::fprintf(stderr, "vegetation-builder: failed to decompress meshopt buffers\n");
        cgltf_free(*outData);
        *outData = nullptr;
        return false;
    }

    return true;
}

static std::vector<std::string> parseStringArrayJson(const fs::path& path) {
    std::vector<std::string> out;
    std::ifstream in(path);
    if (!in) return out;

    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    bool inString = false;
    bool escape = false;
    std::string cur;

    for (char ch : s) {
        if (!inString) {
            if (ch == '"') {
                inString = true;
                cur.clear();
            }
            continue;
        }

        if (escape) {
            cur.push_back(ch);
            escape = false;
            continue;
        }
        if (ch == '\\') {
            escape = true;
            continue;
        }
        if (ch == '"') {
            inString = false;
            out.push_back(cur);
            continue;
        }
        cur.push_back(ch);
    }

    return out;
}

static bool unpackWorldTransform(const cgltf_node* node, float out[16]) {
    cgltf_node_transform_world(node, out);
    return true;
}

static Vec3 transformPoint(const float m[16], Vec3 p) {
    return {
        m[0] * p.x + m[4] * p.y + m[8]  * p.z + m[12],
        m[1] * p.x + m[5] * p.y + m[9]  * p.z + m[13],
        m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14],
    };
}

static bool loadImageRgba(const fs::path& path, ImageRgba& out) {
    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = stbi_load(path.string().c_str(), &w, &h, &comp, 4);
    if (!pixels) return false;

    out.width = w;
    out.height = h;
    out.pixels.assign(pixels, pixels + (size_t)w * (size_t)h * 4u);
    stbi_image_free(pixels);
    return true;
}

static int parseUdimFromFilename(const fs::path& path) {
    std::string stem = path.stem().string();
    size_t dot = stem.rfind('.');
    std::string suffix = (dot == std::string::npos) ? stem : stem.substr(dot + 1);
    if (suffix.size() != 4) return -1;
    for (char c : suffix) if (!std::isdigit((unsigned char)c)) return -1;
    return std::atoi(suffix.c_str());
}

static std::vector<GroupMaps> loadVegetationMaps(const std::vector<std::string>& groupNames,
                                                 const fs::path& sourceTerrainDir) {
    std::vector<GroupMaps> groups;
    groups.reserve(groupNames.size());

    for (u32 gi = 0; gi < (u32)groupNames.size(); ++gi) {
        GroupMaps group;
        group.name = groupNames[gi];
        group.groupIndex = gi;

        fs::path dir = sourceTerrainDir / group.name;
        if (!fs::is_directory(dir)) {
            std::fprintf(stderr, "vegetation-builder: warning: missing vegetation dir %s\n", dir.string().c_str());
            groups.push_back(std::move(group));
            continue;
        }

        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (ext != ".png") continue;

            int udim = parseUdimFromFilename(entry.path());
            if (udim < 1001 || udim >= 1001 + VEG_TILE_COUNT) continue;

            ImageRgba image;
            if (!loadImageRgba(entry.path(), image)) {
                std::fprintf(stderr, "vegetation-builder: warning: failed to load %s\n", entry.path().string().c_str());
                continue;
            }
            group.udimImages[udim] = std::move(image);
        }

        groups.push_back(std::move(group));
    }

    return groups;
}

struct UdimSample {
    std::array<u8, 4> rgba = {0, 0, 0, 0};
    int tileU = -1;
    int tileV = -1;
    int udim = -1;
    bool hit = false;
};

static UdimSample sampleUdimRgba(const GroupMaps& group, Vec2 uv, bool flipV) {
    UdimSample sample;
    sample.tileU = (int)std::floor(uv.x);
    sample.tileV = (int)std::floor(uv.y);
    if (sample.tileU < 0 || sample.tileU >= VEG_TILES_PER_AXIS || sample.tileV < 0 || sample.tileV >= VEG_TILES_PER_AXIS) {
        return sample;
    }

    sample.udim = 1001 + sample.tileU + sample.tileV * VEG_TILES_PER_AXIS;
    auto it = group.udimImages.find(sample.udim);
    if (it == group.udimImages.end()) return sample;

    const ImageRgba& img = it->second;
    float localU = uv.x - std::floor(uv.x);
    float localV = uv.y - std::floor(uv.y);
    if (flipV) localV = 1.0f - localV;
    int x = std::clamp((int)(localU * (float)(img.width - 1) + 0.5f), 0, img.width - 1);
    int y = std::clamp((int)((1.0f - localV) * (float)(img.height - 1) + 0.5f), 0, img.height - 1);
    size_t idx = ((size_t)y * (size_t)img.width + (size_t)x) * 4u;
    sample.rgba = {img.pixels[idx + 0], img.pixels[idx + 1], img.pixels[idx + 2], img.pixels[idx + 3]};
    sample.hit = true;
    return sample;
}

static Vec2 worldPosToVegetationUv(Vec3 pos, Vec3 worldMin, Vec3 worldMax) {
    float sizeX = worldMax.x - worldMin.x;
    float sizeZ = worldMax.z - worldMin.z;
    if (sizeX <= 0.0f || sizeZ <= 0.0f) return {0.0f, 0.0f};

    float nx = std::clamp((pos.x - worldMin.x) / sizeX, 0.0f, 0.9999f);
    float nz = std::clamp((pos.z - worldMin.z) / sizeZ, 0.0f, 0.9999f);
    return {nx * (float)VEG_TILES_PER_AXIS, nz * (float)VEG_TILES_PER_AXIS};
}

static bool decodeGrassGreen(u8 green, float* densityOut) {
    if (green == 0) return false;
    float normalized = (float)green / 255.0f;
    if (normalized < 0.10f) return false;
    *densityOut = normalized;
    return *densityOut > 0.0f;
}

static int tileIndexForPosition(Vec3 pos, Vec3 worldMin, Vec3 worldMax) {
    float sizeX = worldMax.x - worldMin.x;
    float sizeZ = worldMax.z - worldMin.z;
    if (sizeX <= 0.0f || sizeZ <= 0.0f) return 0;

    float nx = std::clamp((pos.x - worldMin.x) / sizeX, 0.0f, 0.9999f);
    float nz = std::clamp((pos.z - worldMin.z) / sizeZ, 0.0f, 0.9999f);
    int tx = std::clamp((int)(nx * VEG_TILES_PER_AXIS), 0, VEG_TILES_PER_AXIS - 1);
    int tz = std::clamp((int)(nz * VEG_TILES_PER_AXIS), 0, VEG_TILES_PER_AXIS - 1);
    return tz * VEG_TILES_PER_AXIS + tx;
}

static bool writeBake(const char* outputPath,
                      Vec3 worldMin,
                      Vec3 worldMax,
                      const std::vector<std::vector<BakedInstance>>& tiles) {
    FILE* f = std::fopen(outputPath, "wb");
    if (!f) {
        std::fprintf(stderr, "vegetation-builder: cannot open %s for writing\n", outputPath);
        return false;
    }

    fwrite(MAGIC, 1, 4, f);
    write_u32(f, VERSION);
    write_f32(f, worldMin.x); write_f32(f, worldMin.y); write_f32(f, worldMin.z);
    write_f32(f, worldMax.x); write_f32(f, worldMax.y); write_f32(f, worldMax.z);
    write_u32(f, VEG_TILES_PER_AXIS);

    for (int i = 0; i < VEG_TILE_COUNT; ++i) {
        write_u32(f, (u32)tiles[i].size());
    }

    for (int i = 0; i < VEG_TILE_COUNT; ++i) {
        if (!tiles[i].empty()) {
            fwrite(tiles[i].data(), sizeof(BakedInstance), tiles[i].size(), f);
        }
    }

    std::fclose(f);
    return true;
}

int main(int argc, char** argv) {
    bool dryRun = false;
    int argi = 1;
    if (argi < argc && std::strcmp(argv[argi], "--dry-run") == 0) {
        dryRun = true;
        argi++;
    }

    if (argc - argi < 4) {
        std::fprintf(stderr,
                     "usage: vegetation-builder [--dry-run] <input.glb> <output.vegetation> <vegetationGroups.json> <sourceTerrainDir>\n");
        return 1;
    }

    const char* inputPath = argv[argi + 0];
    const char* outputPath = argv[argi + 1];
    fs::path vegetationGroupsPath = argv[argi + 2];
    fs::path sourceTerrainDir = argv[argi + 3];

    std::vector<std::string> groupNames = parseStringArrayJson(vegetationGroupsPath);
    if (groupNames.empty()) {
        std::printf("vegetation-builder: no vegetation groups, skipping\n");
        return 0;
    }

    cgltf_data* data = nullptr;
    if (!parseGltf(inputPath, &data)) return 1;

    std::vector<GroupMaps> groups = loadVegetationMaps(groupNames, sourceTerrainDir);
    std::vector<std::vector<BakedInstance>> tiles(VEG_TILE_COUNT);
    std::array<u32, VEG_TILE_COUNT> tileInstanceCounts = {};

    Vec3 worldMin = {FLT_MAX, FLT_MAX, FLT_MAX};
    Vec3 worldMax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

    // Pass 1: compute terrain world bounds from positions only.
    for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& node = data->nodes[ni];
        if (!node.mesh) continue;

        float world[16];
        unpackWorldTransform(&node, world);

        for (cgltf_size pi = 0; pi < node.mesh->primitives_count; ++pi) {
            const cgltf_primitive& prim = node.mesh->primitives[pi];
            const cgltf_accessor* posAccessor = nullptr;
            for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai) {
                const cgltf_attribute& attr = prim.attributes[ai];
                if (attr.type == cgltf_attribute_type_position) posAccessor = attr.data;
            }
            if (!posAccessor) continue;

            std::vector<float> positions(posAccessor->count * 3u);
            cgltf_accessor_unpack_floats(posAccessor, positions.data(), positions.size());

            for (cgltf_size vi = 0; vi < posAccessor->count; ++vi) {
                Vec3 p = transformPoint(world, {positions[vi * 3 + 0], positions[vi * 3 + 1], positions[vi * 3 + 2]});
                worldMin.x = std::min(worldMin.x, p.x);
                worldMin.y = std::min(worldMin.y, p.y);
                worldMin.z = std::min(worldMin.z, p.z);
                worldMax.x = std::max(worldMax.x, p.x);
                worldMax.y = std::max(worldMax.y, p.y);
                worldMax.z = std::max(worldMax.z, p.z);
            }
        }
    }

    BakeStats stats = {};

    for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& node = data->nodes[ni];
        if (!node.mesh) continue;

        float world[16];
        unpackWorldTransform(&node, world);

        for (cgltf_size pi = 0; pi < node.mesh->primitives_count; ++pi) {
            const cgltf_primitive& prim = node.mesh->primitives[pi];
            if (!prim.indices) continue;

            const cgltf_accessor* posAccessor = nullptr;
            for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai) {
                const cgltf_attribute& attr = prim.attributes[ai];
                if (attr.type == cgltf_attribute_type_position) posAccessor = attr.data;
            }
            if (!posAccessor) continue;

            std::vector<float> positions(posAccessor->count * 3u);
            cgltf_accessor_unpack_floats(posAccessor, positions.data(), positions.size());

            const u32 indexCount = (u32)prim.indices->count;
            for (u32 i = 0; i + 2 < indexCount; i += 3) {
                u32 i0 = (u32)cgltf_accessor_read_index(prim.indices, i + 0);
                u32 i1 = (u32)cgltf_accessor_read_index(prim.indices, i + 1);
                u32 i2 = (u32)cgltf_accessor_read_index(prim.indices, i + 2);

                Vec3 p0 = transformPoint(world, {positions[i0 * 3 + 0], positions[i0 * 3 + 1], positions[i0 * 3 + 2]});
                Vec3 p1 = transformPoint(world, {positions[i1 * 3 + 0], positions[i1 * 3 + 1], positions[i1 * 3 + 2]});
                Vec3 p2 = transformPoint(world, {positions[i2 * 3 + 0], positions[i2 * 3 + 1], positions[i2 * 3 + 2]});

                Vec3 posCentroid = {(p0.x + p1.x + p2.x) / 3.0f, (p0.y + p1.y + p2.y) / 3.0f, (p0.z + p1.z + p2.z) / 3.0f};
                Vec2 uv0 = worldPosToVegetationUv(p0, worldMin, worldMax);
                Vec2 uv1 = worldPosToVegetationUv(p1, worldMin, worldMax);
                Vec2 uv2 = worldPosToVegetationUv(p2, worldMin, worldMax);
                Vec2 uvCentroid = worldPosToVegetationUv(posCentroid, worldMin, worldMax);

                Vec3 e1 = vec3_sub(p1, p0);
                Vec3 e2 = vec3_sub(p2, p0);
                Vec3 cross = vec3_cross(e1, e2);
                float area = vec3_len(cross) * 0.5f;
                if (area <= 1e-6f) continue;

                stats.triCount++;

                Vec3 normal = vec3_norm(cross);
                if (normal.y < VEG_SLOPE_THRESHOLD) {
                    stats.slopeRejected++;
                    continue;
                }

                float slopeFactor = (normal.y - VEG_SLOPE_THRESHOLD) / (1.0f - VEG_SLOPE_THRESHOLD);
                slopeFactor = std::clamp(slopeFactor, 0.0f, 1.0f);
                slopeFactor *= slopeFactor;

                bool anyGrassThisTri = false;

                for (u32 gi = 0; gi < (u32)groups.size(); ++gi) {
                    UdimSample centerNoFlip = sampleUdimRgba(groups[gi], uvCentroid, false);
                    UdimSample centerFlip   = sampleUdimRgba(groups[gi], uvCentroid, true);
                    UdimSample v0NoFlip     = sampleUdimRgba(groups[gi], uv0, false);
                    UdimSample v1NoFlip     = sampleUdimRgba(groups[gi], uv1, false);
                    UdimSample v2NoFlip     = sampleUdimRgba(groups[gi], uv2, false);
                    UdimSample v0Flip       = sampleUdimRgba(groups[gi], uv0, true);
                    UdimSample v1Flip       = sampleUdimRgba(groups[gi], uv1, true);
                    UdimSample v2Flip       = sampleUdimRgba(groups[gi], uv2, true);

                    auto updateRanges = [&](const std::array<u8, 4>& rgba) {
                        stats.minR = std::min(stats.minR, rgba[0]); stats.maxR = std::max(stats.maxR, rgba[0]);
                        stats.minG = std::min(stats.minG, rgba[1]); stats.maxG = std::max(stats.maxG, rgba[1]);
                        stats.minB = std::min(stats.minB, rgba[2]); stats.maxB = std::max(stats.maxB, rgba[2]);
                        stats.minA = std::min(stats.minA, rgba[3]); stats.maxA = std::max(stats.maxA, rgba[3]);
                    };

                    if (centerNoFlip.hit) updateRanges(centerNoFlip.rgba);
                    if (centerFlip.hit) updateRanges(centerFlip.rgba);
                    if (v0NoFlip.hit) updateRanges(v0NoFlip.rgba);
                    if (v1NoFlip.hit) updateRanges(v1NoFlip.rgba);
                    if (v2NoFlip.hit) updateRanges(v2NoFlip.rgba);
                    if (v0Flip.hit) updateRanges(v0Flip.rgba);
                    if (v1Flip.hit) updateRanges(v1Flip.rgba);
                    if (v2Flip.hit) updateRanges(v2Flip.rgba);

                    if (dryRun && stats.sampleLogs < 24 && (centerNoFlip.hit || centerFlip.hit)) {
                        std::printf("vegetation-builder: sample group=%s uv=(%.3f,%.3f) noflip[udim=%d rgba=%u,%u,%u,%u hit=%d] flip[udim=%d rgba=%u,%u,%u,%u hit=%d]\n",
                                    groups[gi].name.c_str(),
                                    uvCentroid.x,
                                    uvCentroid.y,
                                    centerNoFlip.udim,
                                    (unsigned)centerNoFlip.rgba[0], (unsigned)centerNoFlip.rgba[1], (unsigned)centerNoFlip.rgba[2], (unsigned)centerNoFlip.rgba[3], centerNoFlip.hit ? 1 : 0,
                                    centerFlip.udim,
                                    (unsigned)centerFlip.rgba[0], (unsigned)centerFlip.rgba[1], (unsigned)centerFlip.rgba[2], (unsigned)centerFlip.rgba[3], centerFlip.hit ? 1 : 0);
                        stats.sampleLogs++;
                    }

                    u8 green = std::max({centerFlip.rgba[1], v0Flip.rgba[1], v1Flip.rgba[1], v2Flip.rgba[1]});

                    float densityMap = 0.0f;
                    if (!decodeGrassGreen(green, &densityMap)) {
                        stats.mapRejected++;
                        continue;
                    }

                    anyGrassThisTri = true;

                    float expected = area * VEG_BASE_DENSITY * densityMap * slopeFactor;
                    u32 count = (u32)expected;
                    float fraction = expected - (float)count;
                    if (hash_float01((u32)stats.triCount, gi, 100u) < fraction) count++;
                    stats.requestedInstances += count;
                    if (count > MAX_INSTANCES_PER_TRIANGLE) {
                        count = MAX_INSTANCES_PER_TRIANGLE;
                        stats.cappedByTriangle++;
                    }
                    if (count == 0) continue;

                    for (u32 inst = 0; inst < count; ++inst) {
                        float r1 = hash_float01((u32)stats.triCount, inst, gi * 17u + 1u);
                        float r2 = hash_float01((u32)stats.triCount, inst, gi * 19u + 2u);
                        float su = std::sqrt(r1);
                        float b0 = 1.0f - su;
                        float b1 = su * (1.0f - r2);
                        float b2 = su * r2;
                        Vec3 pos = vec3_add(vec3_add(vec3_scale(p0, b0), vec3_scale(p1, b1)), vec3_scale(p2, b2));

                        int tile = tileIndexForPosition(pos, worldMin, worldMax);
                        if (tileInstanceCounts[(size_t)tile] >= MAX_INSTANCES_PER_TILE) {
                            stats.cappedByTile++;
                            continue;
                        }
                        if (stats.generatedInstances >= MAX_TOTAL_INSTANCES) {
                            stats.cappedByTotal++;
                            continue;
                        }

                        BakedInstance out = {};
                        out.pos[0] = pos.x;
                        out.pos[1] = pos.y;
                        out.pos[2] = pos.z;
                        out.rotation = hash_float01((u32)stats.triCount, inst, gi * 23u + 3u) * TAU;
                        out.scale = 0.8f + hash_float01((u32)stats.triCount, inst, gi * 29u + 4u) * 0.4f;
                        out.groupIndex = (u8)gi;
                        out.channel = 1u; /* green only for grass */
                        out.normal[0] = normal.x;
                        out.normal[1] = normal.y;
                        out.normal[2] = normal.z;

                        if (!dryRun) {
                            tiles[tile].push_back(out);
                        }
                        tileInstanceCounts[(size_t)tile]++;
                        stats.generatedInstances++;
                    }
                }

                if (anyGrassThisTri) stats.trianglesWithGrass++;
            }
        }
    }

    bool ok = true;
    if (!dryRun) {
        ok = writeBake(outputPath, worldMin, worldMax, tiles);
    }

    if (ok) {
        std::printf("vegetation-builder: %s generated=%llu requested=%llu triangles=%llu grassTriangles=%llu output=%s\n",
                    dryRun ? "dry-run" : "bake",
                    (unsigned long long)stats.generatedInstances,
                    (unsigned long long)stats.requestedInstances,
                    (unsigned long long)stats.triCount,
                    (unsigned long long)stats.trianglesWithGrass,
                    outputPath);
        std::printf("vegetation-builder: ranges R=%u..%u G=%u..%u B=%u..%u A=%u..%u slopeRejected=%llu mapRejected=%llu\n",
                    (unsigned)stats.minR,
                    (unsigned)stats.maxR,
                    (unsigned)stats.minG,
                    (unsigned)stats.maxG,
                    (unsigned)stats.minB,
                    (unsigned)stats.maxB,
                    (unsigned)stats.minA,
                    (unsigned)stats.maxA,
                    (unsigned long long)stats.slopeRejected,
                    (unsigned long long)stats.mapRejected);
        std::printf("vegetation-builder: caps triangle=%llu tile=%llu total=%llu\n",
                    (unsigned long long)stats.cappedByTriangle,
                    (unsigned long long)stats.cappedByTile,
                    (unsigned long long)stats.cappedByTotal);
        for (u32 gi = 0; gi < (u32)groups.size(); ++gi) {
            std::printf("vegetation-builder: group '%s' loadedTiles=%u\n",
                        groups[gi].name.c_str(),
                        (unsigned)groups[gi].udimImages.size());
        }
        for (int i = 0; i < VEG_TILE_COUNT; ++i) {
            if (tileInstanceCounts[(size_t)i] > 0) {
                std::printf("vegetation-builder: tile %d instances=%u\n", i, tileInstanceCounts[(size_t)i]);
            }
        }
    }

    cgltf_free(data);
    return ok ? 0 : 1;
}
