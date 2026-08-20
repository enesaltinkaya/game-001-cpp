/**
 * jolt-shape-builder: Standalone tool that reads a glTF/GLB file and
 * pre-bakes Jolt shape data for every physics-enabled node.
 *
 * A node qualifies if its glTF extras contain:
 *   - "notexture": true          (physics-only collision mesh, always static)
 *   - "rigidBodyShape": <shape>  (BOX, SPHERE, CAPSULE, CYLINDER, CONVEX_HULL, MESH)
 *
 * Usage: jolt-shape-builder <input.glb> <output.jolt>
 *
 * Output binary format (version 2):
 *   Header:
 *     u8[4]   magic = "JBVH"
 *     u32     version = 2
 *     u32     entry_count
 *   Per entry:
 *     u32     node_name_length
 *     char[]  node_name (NOT null-terminated)
 *     u8      shape_type  (0=NOTEXTURE, 1=BOX, 2=SPHERE, 3=CAPSULE,
 *                          4=CYLINDER, 5=CONVEX_HULL, 6=MESH)
 *     u8      motion_type (0=STATIC, 1=DYNAMIC)
 *     float   mass
 *     float   friction
 *     float   restitution
 *     u32     shape_blob_size
 *     u8[]    shape_blob (Jolt SaveBinaryState output)
 *
 * Version 1 format is no longer written; the engine sidecar loader
 * checks the version field and rejects v1 at load time.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <sstream>
#include <vector>
#include <string>

// ── cgltf ──────────────────────────────────────────────────────────────────
#define CGLTF_IMPLEMENTATION
extern "C" {
#include "cgltf.h"
}

// ── meshoptimizer (for decompressing gltfpack output) ──────────────────────
#include "meshoptimizer.h"

// ── Jolt Physics ───────────────────────────────────────────────────────────
#include "Jolt/Jolt.h"
#include "Jolt/RegisterTypes.h"
#include "Jolt/Core/Factory.h"
#include "Jolt/Core/StreamWrapper.h"
#include "Jolt/Physics/Collision/Shape/MeshShape.h"
#include "Jolt/Physics/Collision/Shape/BoxShape.h"
#include "Jolt/Physics/Collision/Shape/SphereShape.h"
#include "Jolt/Physics/Collision/Shape/CapsuleShape.h"
#include "Jolt/Physics/Collision/Shape/CylinderShape.h"
#include "Jolt/Physics/Collision/Shape/ConvexHullShape.h"
#include "Jolt/Physics/Collision/Shape/ScaledShape.h"

using namespace JPH;

// ── types ──────────────────────────────────────────────────────────────────
typedef uint32_t u32;
typedef uint8_t  u8;

static const u8  MAGIC[4] = {'J', 'B', 'V', 'H'};
static const u32 VERSION  = 2;

// Shape type tags written into the sidecar (must match engine-side enum).
enum ShapeTypeTag : u8 {
    SHAPE_TAG_NOTEXTURE  = 0,
    SHAPE_TAG_BOX        = 1,
    SHAPE_TAG_SPHERE     = 2,
    SHAPE_TAG_CAPSULE    = 3,
    SHAPE_TAG_CYLINDER   = 4,
    SHAPE_TAG_CONVEX_HULL = 5,
    SHAPE_TAG_MESH       = 6,
};

// Motion type tags.
enum MotionTypeTag : u8 {
    MOTION_TAG_STATIC  = 0,
    MOTION_TAG_DYNAMIC = 1,
};

// ── helpers ────────────────────────────────────────────────────────────────

static void write_u32(FILE* f, u32 v) { fwrite(&v, 4, 1, f); }
static void write_u8(FILE* f, u8 v)   { fwrite(&v, 1, 1, f); }
static void write_f32(FILE* f, float v) { fwrite(&v, 4, 1, f); }

static int decompress_meshopt(cgltf_data* data) {
    for (size_t i = 0; i < data->buffer_views_count; i++) {
        if (!data->buffer_views[i].has_meshopt_compression) continue;
        cgltf_meshopt_compression* mc = &data->buffer_views[i].meshopt_compression;
        const unsigned char* source   = (const unsigned char*)mc->buffer->data;
        if (!source) return -1;
        source += mc->offset;

        void* result = malloc(mc->count * mc->stride);
        if (!result) return -1;

        int rc = -1;
        switch (mc->mode) {
        case cgltf_meshopt_compression_mode_attributes:
            rc = meshopt_decodeVertexBuffer(result, mc->count, mc->stride, source, mc->size);
            break;
        case cgltf_meshopt_compression_mode_triangles:
            rc = meshopt_decodeIndexBuffer(result, mc->count, mc->stride, source, mc->size);
            break;
        case cgltf_meshopt_compression_mode_indices:
            rc = meshopt_decodeIndexSequence(result, mc->count, mc->stride, source, mc->size);
            break;
        default: break;
        }
        if (rc != 0) { free(result); return -1; }

        switch (mc->filter) {
        case cgltf_meshopt_compression_filter_octahedral:
            meshopt_decodeFilterOct(result, mc->count, mc->stride);
            break;
        case cgltf_meshopt_compression_filter_quaternion:
            meshopt_decodeFilterQuat(result, mc->count, mc->stride);
            break;
        case cgltf_meshopt_compression_filter_exponential:
            meshopt_decodeFilterExp(result, mc->count, mc->stride);
            break;
        default: break;
        }
        data->buffer_views[i].data = result;
    }
    return 0;
}

// ── JSON extra parsing helpers ────────────────────────────────────────────
// We do minimal string-based JSON parsing here — the engine uses a proper
// JSON parser but for this tool we just need to extract a few keys.

/// Check if a node's extras JSON contains "notexture": true
static bool has_notexture(cgltf_node* node) {
    if (!node->extras.data) return false;
    const char* p = strstr(node->extras.data, "\"notexture\"");
    if (!p) return false;
    p += strlen("\"notexture\"");
    return strstr(p, "true") != nullptr;
}

/// Extract the value of a string-valued key from extras JSON.
/// Returns "" if not found.  e.g. get_extra_string(node, "rigidBodyShape") → "BOX"
static std::string get_extra_string(cgltf_node* node, const char* key) {
    if (!node->extras.data) return {};
    std::string data(node->extras.data);
    std::string needle = std::string("\"") + key + "\"";
    size_t pos = data.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    // Skip whitespace and colon
    while (pos < data.size() && (data[pos] == ' ' || data[pos] == ':' || data[pos] == '\t')) pos++;
    if (pos >= data.size() || data[pos] != '"') return {};
    size_t end = data.find('"', pos + 1);
    if (end == std::string::npos) return {};
    return data.substr(pos + 1, end - pos - 1);
}

/// Extract a numeric value from extras JSON.  Returns def if not found.
static float get_extra_float(cgltf_node* node, const char* key, float def) {
    if (!node->extras.data) return def;
    std::string data(node->extras.data);
    std::string needle = std::string("\"") + key + "\"";
    size_t pos = data.find(needle);
    if (pos == std::string::npos) return def;
    pos += needle.size();
    while (pos < data.size() && (data[pos] == ' ' || data[pos] == ':' || data[pos] == '\t')) pos++;
    size_t end = pos;
    while (end < data.size() && (data[end] == '-' || data[end] == '.' ||
           (data[end] >= '0' && data[end] <= '9') || data[end] == 'e' || data[end] == 'E' || data[end] == '+')) end++;
    if (end == pos) return def;
    return (float)atof(data.substr(pos, end - pos).c_str());
}

// ── AABB / vertex extraction ──────────────────────────────────────────────

struct AABB { float minX, minY, minZ, maxX, maxY, maxZ; };

static AABB get_primitive_aabb(cgltf_primitive* prim) {
    AABB aabb = {0, 0, 0, 0, 0, 0};
    for (size_t ai = 0; ai < prim->attributes_count; ai++) {
        if (prim->attributes[ai].type == cgltf_attribute_type_position) {
            cgltf_accessor* pos_acc = prim->attributes[ai].data;
            if (pos_acc->has_min) {
                aabb.minX = pos_acc->min[0];
                aabb.minY = pos_acc->min[1];
                aabb.minZ = pos_acc->min[2];
            }
            if (pos_acc->has_max) {
                aabb.maxX = pos_acc->max[0];
                aabb.maxY = pos_acc->max[1];
                aabb.maxZ = pos_acc->max[2];
            }
            break;
        }
    }
    return aabb;
}

struct MyVec3 { float x, y, z; };

static std::vector<MyVec3> get_positions(cgltf_primitive* prim) {
    std::vector<MyVec3> out;
    for (size_t ai = 0; ai < prim->attributes_count; ai++) {
        if (prim->attributes[ai].type == cgltf_attribute_type_position) {
            cgltf_accessor* pos_acc = prim->attributes[ai].data;
            out.resize(pos_acc->count);
            cgltf_accessor_unpack_floats(pos_acc, &out[0].x, pos_acc->count * 3);
            return out;
        }
    }
    return out;
}

// ── Shape building + serialization ────────────────────────────────────────

/// Serialize a Jolt shape to a binary blob via SaveBinaryState.
static std::string serialize_shape(RefConst<Shape> shape) {
    std::ostringstream oss(std::ios::binary);
    StreamOutWrapper streamOut(oss);
    shape->SaveBinaryState(streamOut);
    return oss.str();
}

/// Build a Jolt MeshShape from a glTF primitive's vertices and indices.
static std::string build_mesh_shape(cgltf_primitive* prim) {
    cgltf_accessor* idx_acc = prim->indices;
    if (!idx_acc) return {};

    size_t index_count = idx_acc->count;
    u32* indices = (u32*)malloc(index_count * sizeof(u32));
    cgltf_accessor_unpack_indices(idx_acc, indices, sizeof(u32), index_count);

    std::vector<MyVec3> verts = get_positions(prim);
    if (verts.empty()) { free(indices); return {}; }

    MeshShapeSettings shapeSettings;
    shapeSettings.mTriangleVertices.resize(verts.size());
    memcpy(shapeSettings.mTriangleVertices.data(), verts.data(), verts.size() * 3 * sizeof(float));

    u32 inputTriangleCount = (u32)(index_count / 3);
    shapeSettings.mIndexedTriangles.resize(inputTriangleCount);

    IndexedTriangle* dstTriangles = shapeSettings.mIndexedTriangles.data();
    u32 validTriangleCount = 0;
    const float kEpsilonSq = 1.0e-12f;

    for (u32 i = 0; i < inputTriangleCount; ++i) {
        u32 i0 = indices[i * 3 + 0];
        u32 i1 = indices[i * 3 + 1];
        u32 i2 = indices[i * 3 + 2];

        if (i0 == i1 || i1 == i2 || i0 == i2) continue;

        const MyVec3& p0 = verts[i0];
        const MyVec3& p1 = verts[i1];
        const MyVec3& p2 = verts[i2];

        float e1x = p1.x - p0.x, e1y = p1.y - p0.y, e1z = p1.z - p0.z;
        float e2x = p2.x - p0.x, e2y = p2.y - p0.y, e2z = p2.z - p0.z;

        float cx = e1y * e2z - e1z * e2y;
        float cy = e1z * e2x - e1x * e2z;
        float cz = e1x * e2y - e1y * e2x;

        if (cx * cx + cy * cy + cz * cz < kEpsilonSq) continue;

        dstTriangles[validTriangleCount].mIdx[0]        = i0;
        dstTriangles[validTriangleCount].mIdx[1]        = i1;
        dstTriangles[validTriangleCount].mIdx[2]        = i2;
        dstTriangles[validTriangleCount].mMaterialIndex = 0;
        dstTriangles[validTriangleCount].mUserData      = 0;
        validTriangleCount++;
    }

    shapeSettings.mIndexedTriangles.resize(validTriangleCount);
    free(indices);

    ShapeSettings::ShapeResult shapeResult = shapeSettings.Create();
    if (shapeResult.HasError()) {
        fprintf(stderr, "jolt-shape-builder: mesh shape error: %s\n",
                shapeResult.GetError().c_str());
        return {};
    }

    return serialize_shape(shapeResult.Get());
}

/// Build a Jolt ConvexHullShape from a glTF primitive's vertices.
static std::string build_convex_hull_shape(cgltf_primitive* prim) {
    std::vector<MyVec3> verts = get_positions(prim);
    if (verts.empty()) return {};

    Array<Vec3> points;
    points.resize(verts.size());
    for (size_t i = 0; i < verts.size(); i++)
        points[i] = Vec3(verts[i].x, verts[i].y, verts[i].z);

    ConvexHullShapeSettings settings(points.data(), (int)verts.size());
    auto result = settings.Create();
    if (result.HasError()) {
        fprintf(stderr, "jolt-shape-builder: convex hull error: %s\n",
                result.GetError().c_str());
        return {};
    }
    return serialize_shape(result.Get());
}

/// Build a Jolt BoxShape from an AABB.
static std::string build_box_shape(const AABB& aabb) {
    float hx = (aabb.maxX - aabb.minX) * 0.5f;
    float hy = (aabb.maxY - aabb.minY) * 0.5f;
    float hz = (aabb.maxZ - aabb.minZ) * 0.5f;
    BoxShapeSettings settings(JPH::Vec3(hx, hy, hz));
    auto result = settings.Create();
    if (result.HasError()) {
        fprintf(stderr, "jolt-shape-builder: box shape error: %s\n",
                result.GetError().c_str());
        return {};
    }
    return serialize_shape(result.Get());
}

/// Build a Jolt SphereShape from an AABB (uses largest half-extent as radius).
static std::string build_sphere_shape(const AABB& aabb) {
    float hx = (aabb.maxX - aabb.minX) * 0.5f;
    float hy = (aabb.maxY - aabb.minY) * 0.5f;
    float hz = (aabb.maxZ - aabb.minZ) * 0.5f;
    float radius = hx > hy ? hx : hy;
    if (hz > radius) radius = hz;
    SphereShapeSettings settings(radius);
    auto result = settings.Create();
    if (result.HasError()) {
        fprintf(stderr, "jolt-shape-builder: sphere shape error: %s\n",
                result.GetError().c_str());
        return {};
    }
    return serialize_shape(result.Get());
}

/// Build a Jolt CapsuleShape from an AABB.
static std::string build_capsule_shape(const AABB& aabb) {
    float hx = (aabb.maxX - aabb.minX) * 0.5f;
    float hy = (aabb.maxY - aabb.minY) * 0.5f;
    float hz = (aabb.maxZ - aabb.minZ) * 0.5f;
    float radius = hx > hz ? hx : hz;
    float halfHeight = hy > radius ? hy - radius : 0.0f;
    CapsuleShapeSettings settings(halfHeight, radius);
    auto result = settings.Create();
    if (result.HasError()) {
        fprintf(stderr, "jolt-shape-builder: capsule shape error: %s\n",
                result.GetError().c_str());
        return {};
    }
    return serialize_shape(result.Get());
}

/// Build a Jolt CylinderShape from an AABB.
static std::string build_cylinder_shape(const AABB& aabb) {
    float hx = (aabb.maxX - aabb.minX) * 0.5f;
    float hy = (aabb.maxY - aabb.minY) * 0.5f;
    float hz = (aabb.maxZ - aabb.minZ) * 0.5f;
    float radius = hx > hz ? hx : hz;
    CylinderShapeSettings settings(hy, radius);
    auto result = settings.Create();
    if (result.HasError()) {
        fprintf(stderr, "jolt-shape-builder: cylinder shape error: %s\n",
                result.GetError().c_str());
        return {};
    }
    return serialize_shape(result.Get());
}

// ── Entry collection ──────────────────────────────────────────────────────

struct Entry {
    std::string nodeName;
    u8          shapeTag;     // ShapeTypeTag
    u8          motionTag;    // MotionTypeTag
    float       mass;
    float       friction;
    float       restitution;
    std::string shapeBlob;
};

static ShapeTypeTag rigid_body_shape_tag(const std::string& shape) {
    if (shape == "BOX")          return SHAPE_TAG_BOX;
    if (shape == "SPHERE")       return SHAPE_TAG_SPHERE;
    if (shape == "CAPSULE")      return SHAPE_TAG_CAPSULE;
    if (shape == "CYLINDER")     return SHAPE_TAG_CYLINDER;
    if (shape == "CONVEX_HULL")  return SHAPE_TAG_CONVEX_HULL;
    if (shape == "MESH")         return SHAPE_TAG_MESH;
    // Unknown shape — skip
    return (ShapeTypeTag)0xFF;
}

/// Recursively collect qualifying nodes
static void collect_nodes(cgltf_data* data, cgltf_node* node, std::vector<Entry>& entries) {
    const char* name = node->name ? node->name : "";

    // ── notexture: static mesh collision ──
    if (node->mesh && has_notexture(node) && node->mesh->primitives_count > 0) {
        std::string blob = build_mesh_shape(&node->mesh->primitives[0]);
        if (!blob.empty()) {
            entries.push_back({std::string(name),
                               SHAPE_TAG_NOTEXTURE,
                               MOTION_TAG_STATIC,
                               0.0f,   // mass (unused for static)
                               0.5f,   // friction (default)
                               0.0f,   // restitution (default)
                               std::move(blob)});
            printf("  node '%s': notexture mesh %zu bytes\n", name, entries.back().shapeBlob.size());
        }
    }
    // ── rigid body ──
    else if (node->extras.data) {
        std::string rbShape = get_extra_string(node, "rigidBodyShape");
        if (!rbShape.empty()) {
            ShapeTypeTag tag = rigid_body_shape_tag(rbShape);
            if (tag == 0xFF) {
                // Unsupported shape (e.g. CONE) — skip
                goto children;
            }

            std::string rbType = get_extra_string(node, "rigidBodyType");
            u8 motionTag = (rbType == "ACTIVE") ? MOTION_TAG_DYNAMIC : MOTION_TAG_STATIC;
            float mass        = get_extra_float(node, "rigidBodyMass", 1.0f);
            float friction    = get_extra_float(node, "rigidBodyFriction", 0.5f);
            float restitution = get_extra_float(node, "rigidBodyRestitution", 0.0f);

            std::string blob;

            if (!node->mesh || node->mesh->primitives_count == 0) goto children;

            AABB aabb = get_primitive_aabb(&node->mesh->primitives[0]);

            switch (tag) {
            case SHAPE_TAG_BOX:
                blob = build_box_shape(aabb);
                break;
            case SHAPE_TAG_SPHERE:
                blob = build_sphere_shape(aabb);
                break;
            case SHAPE_TAG_CAPSULE:
                blob = build_capsule_shape(aabb);
                break;
            case SHAPE_TAG_CYLINDER:
                blob = build_cylinder_shape(aabb);
                break;
            case SHAPE_TAG_CONVEX_HULL:
                blob = build_convex_hull_shape(&node->mesh->primitives[0]);
                break;
            case SHAPE_TAG_MESH:
                blob = build_mesh_shape(&node->mesh->primitives[0]);
                break;
            default:
                break;
            }

            if (!blob.empty()) {
                entries.push_back({std::string(name),
                                   tag,
                                   motionTag,
                                   mass,
                                   friction,
                                   restitution,
                                   std::move(blob)});
                printf("  node '%s': %s %s %zu bytes\n",
                       name,
                       motionTag == MOTION_TAG_DYNAMIC ? "dynamic" : "static",
                       rbShape.c_str(),
                       entries.back().shapeBlob.size());
            }
        }
    }

children:
    for (cgltf_size i = 0; i < node->children_count; i++) {
        collect_nodes(data, node->children[i], entries);
    }
}

// ── main ───────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input.glb> <output.jolt>\n", argv[0]);
        return 1;
    }

    const char* input_path  = argv[1];
    const char* output_path = argv[2];

    // ── Initialize Jolt (minimal — only need Factory + types for shape creation) ──
    RegisterDefaultAllocator();
    Factory::sInstance = new Factory();
    RegisterTypes();

    // ── Parse GLB ──────────────────────────────────────────────────────────
    cgltf_options options = {};
    cgltf_data*   data    = nullptr;
    cgltf_result  result  = cgltf_parse_file(&options, input_path, &data);
    if (result != cgltf_result_success) {
        fprintf(stderr, "jolt-shape-builder: failed to parse %s (err %d)\n", input_path, result);
        return 1;
    }
    result = cgltf_load_buffers(&options, data, input_path);
    if (result != cgltf_result_success) {
        fprintf(stderr, "jolt-shape-builder: failed to load buffers (err %d)\n", result);
        cgltf_free(data);
        return 1;
    }

    // Decompress meshopt-compressed buffer views (gltfpack output)
    if (decompress_meshopt(data) != 0) {
        fprintf(stderr, "jolt-shape-builder: failed to decompress meshopt buffers\n");
        cgltf_free(data);
        return 1;
    }

    // ── Collect qualifying nodes ───────────────────────────────────────────
    std::vector<Entry> entries;
    for (cgltf_size i = 0; i < data->scene->nodes_count; i++) {
        collect_nodes(data, data->scene->nodes[i], entries);
    }

    if (entries.empty()) {
        printf("jolt-shape-builder: no qualifying nodes found, skipping\n");
        cgltf_free(data);
        UnregisterTypes();
        delete Factory::sInstance;
        Factory::sInstance = nullptr;
        return 0;
    }

    // ── Write output ───────────────────────────────────────────────────────
    FILE* out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "jolt-shape-builder: cannot open %s for writing\n", output_path);
        cgltf_free(data);
        return 1;
    }

    fwrite(MAGIC, 1, 4, out);
    write_u32(out, VERSION);
    write_u32(out, (u32)entries.size());

    for (const Entry& e : entries) {
        u32 nameLen = (u32)e.nodeName.size();
        write_u32(out, nameLen);
        fwrite(e.nodeName.data(), 1, nameLen, out);
        write_u8(out, e.shapeTag);
        write_u8(out, e.motionTag);
        write_f32(out, e.mass);
        write_f32(out, e.friction);
        write_f32(out, e.restitution);
        u32 blobSize = (u32)e.shapeBlob.size();
        write_u32(out, blobSize);
        fwrite(e.shapeBlob.data(), 1, blobSize, out);
    }

    fclose(out);

    printf("jolt-shape-builder: wrote %zu entries to %s\n", entries.size(), output_path);

    // ── Cleanup ────────────────────────────────────────────────────────────
    cgltf_free(data);
    UnregisterTypes();
    delete Factory::sInstance;
    Factory::sInstance = nullptr;

    return 0;
}
