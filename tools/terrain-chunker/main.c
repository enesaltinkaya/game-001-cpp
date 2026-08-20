#define _DEFAULT_SOURCE
/**
 * terrain-chunker: Subdivides a terrain mesh in a GLB into spatial grid chunks.
 *
 * Reads a meshopt-compressed GLB, finds the terrain mesh (by name),
 * subdivides it into an NxM grid, and writes a single GLB with one mesh per
 * chunk.  All chunks share the same GLB file — no separate files.
 *
 * Usage: terrain-chunker <input.glb> <output.glb> <grid_x> <grid_y>
 *
 * Pipeline integration (0-blender-terrain.py):
 *   gltfpack -> terrain-chunker -> jolt-shape-builder
 *
 * Output:
 *   - One mesh per chunk (attributes match the source terrain)
 *   - One node per chunk named "terrain_chunk_X_Y"
 *   - Contiguous (non-interleaved) attribute storage for glTF compliance
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <stdbool.h>
#include <float.h>

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#define MESHOPT_IMPLEMENTATION
#include "meshoptimizer.h"

typedef uint32_t u32;
typedef uint8_t  u8;
typedef float    f32;

/* ── helpers ─────────────────────────────────────────────────────────────── */

static void *mem_alloc(void *u, size_t s) { (void)u; return malloc(s); }
static void  mem_free (void *u, void *p) { (void)u; free(p); }

static int decompress_meshopt(cgltf_data *data) {
    for (size_t i = 0; i < data->buffer_views_count; i++) {
        if (!data->buffer_views[i].has_meshopt_compression) continue;
        cgltf_meshopt_compression *mc = &data->buffer_views[i].meshopt_compression;
        const unsigned char *src = (const unsigned char *)mc->buffer->data;
        if (!src) return -1;
        src += mc->offset;

        void *buf = malloc(mc->count * mc->stride);
        if (!buf) return -1;

        int rc = -1;
        switch (mc->mode) {
        case cgltf_meshopt_compression_mode_attributes:
            rc = meshopt_decodeVertexBuffer(buf, mc->count, mc->stride, src, mc->size); break;
        case cgltf_meshopt_compression_mode_triangles:
            rc = meshopt_decodeIndexBuffer(buf, mc->count, mc->stride, src, mc->size); break;
        case cgltf_meshopt_compression_mode_indices:
            rc = meshopt_decodeIndexSequence(buf, mc->count, mc->stride, src, mc->size); break;
        case cgltf_meshopt_compression_mode_invalid:
        case cgltf_meshopt_compression_mode_max_enum:
            free(buf); return -1;
        }
        if (rc != 0) { free(buf); return -1; }

        switch (mc->filter) {
        case cgltf_meshopt_compression_filter_octahedral:
            meshopt_decodeFilterOct(buf, mc->count, mc->stride); break;
        case cgltf_meshopt_compression_filter_quaternion:
            meshopt_decodeFilterQuat(buf, mc->count, mc->stride); break;
        case cgltf_meshopt_compression_filter_exponential:
            meshopt_decodeFilterExp(buf, mc->count, mc->stride); break;
        case cgltf_meshopt_compression_filter_none:
        case cgltf_meshopt_compression_filter_color:
        case cgltf_meshopt_compression_filter_max_enum:
            break;
        }
        data->buffer_views[i].data = buf;
    }
    return 0;
}

static int str_has_ci(const char *s, const char *sub) {
    if (!s || !sub) return 0;
    return strcasestr(s, sub) != NULL;
}

/* ── dynamic array helpers ───────────────────────────────────────────────── */

typedef struct { f32 *data; u32 count; u32 cap; } FloatArray;
typedef struct { u32 *data; u32 count; u32 cap; } U32Array;
typedef struct { u8  *data; u32 count; u32 cap; } U8Array;

static void farr_init(FloatArray *a)    { a->data = NULL; a->count = 0; a->cap = 0; }
static void farr_push(FloatArray *a, f32 v) {
    if (a->count >= a->cap) {
        a->cap = a->cap ? a->cap * 2 : 256;
        a->data = realloc(a->data, a->cap * sizeof(f32));
    }
    a->data[a->count++] = v;
}

static void barr_init(U8Array *a)       { a->data = NULL; a->count = 0; a->cap = 0; }
static void barr_set(U8Array *a, u32 idx, u8 v) {
    while (idx >= a->cap) {
        a->cap = a->cap ? a->cap * 2 : 256;
        a->data = realloc(a->data, a->cap * sizeof(u8));
    }
    if (idx >= a->count) a->count = idx + 1;
    a->data[idx] = v;
}

/* ── chunk (contiguous attribute storage) ────────────────────────────────── */

typedef struct {
    u32 vert_count;
    u32 idx_count;
    f32 *positions;  /* 3 floats per vertex */
    f32 *normals;    /* 3 floats per vertex */
    f32 *uvs;        /* 2 floats per vertex */
    f32 *tangents;   /* 4 floats per vertex */
    u32 *idxs;
    int has_normals;
    int has_uvs;
    int has_tangents;
} Chunk;

static void chunk_free(Chunk *c) {
    free(c->positions); free(c->normals);
    free(c->uvs);       free(c->tangents);
    free(c->idxs);
    *c = (Chunk){0};
}

/* ── GLB writer ──────────────────────────────────────────────────────────── */

static void write_u32_le(FILE *f, u32 v) {
    fputc((u8)(v      ), f);
    fputc((u8)(v >>  8), f);
    fputc((u8)(v >> 16), f);
    fputc((u8)(v >> 24), f);
}

/* ── terrain data ────────────────────────────────────────────────────────── */

typedef struct {
    f32 *positions;
    f32 *normals;
    f32 *uvs;
    f32 *tangents;
    u32 *indices;
    u32  vert_count;
    u32  idx_count;
    int  has_normals;
    int  has_uvs;
    int  has_tangents;
} TerrainData;

/* Build chunks using vertex-based triangle assignment:
 *   1. For each chunk, test every triangle
 *   2. A triangle belongs to a chunk if ANY of its 3 vertices falls
 *      within the chunk's XZ bounds
 *   3. Collect all vertices referenced by assigned triangles
 *   4. Remap to local indices
 *
 * This guarantees continuous coverage at chunk boundaries. Triangles
 * straddling a boundary will be assigned to both adjacent chunks,
 * and depth testing handles the overlap seamlessly. */
static void build_chunks(TerrainData *td, Chunk *chunks, u32 total_chunks,
                         u32 grid_x, u32 grid_y, f32 bbMin[3], f32 bbMax[3]) {
    f32 dx = (bbMax[0] - bbMin[0]) / (f32)grid_x;
    f32 dz = (bbMax[2] - bbMin[2]) / (f32)grid_y;

    u32 tri_count = td->idx_count / 3;

    U8Array in_chunk;
    barr_init(&in_chunk);
    u32 *vmap = malloc(td->vert_count * sizeof(u32));

    for (u32 gx = 0; gx < grid_x; gx++) {
        for (u32 gy = 0; gy < grid_y; gy++) {
            u32 ci = gy * grid_x + gx;
            f32 x0 = bbMin[0] + (f32)gx * dx;
            f32 x1 = x0 + dx;
            f32 z0 = bbMin[2] + (f32)gy * dz;
            f32 z1 = z0 + dz;

            /* Reset tracking */
            if (td->vert_count > in_chunk.cap) {
                in_chunk.cap = td->vert_count;
                in_chunk.data = malloc(in_chunk.cap * sizeof(u8));
            }
            in_chunk.count = td->vert_count;
            memset(in_chunk.data, 0, td->vert_count * sizeof(u8));

            /* Pass 1: assign triangles where any vertex falls in this chunk,
               mark referenced vertices */
            for (u32 t = 0; t < tri_count; t++) {
                bool hit = false;
                for (u32 v = 0; v < 3; v++) {
                    u32 vi = td->indices[t * 3 + v];
                    f32 vx = td->positions[vi * 3];
                    f32 vz = td->positions[vi * 3 + 2];
                    if (vx >= x0 && vx < x1 && vz >= z0 && vz < z1) {
                        hit = true;
                        break;
                    }
                }
                if (hit) {
                    in_chunk.data[td->indices[t * 3]]  = 1;
                    in_chunk.data[td->indices[t * 3+1]] = 1;
                    in_chunk.data[td->indices[t * 3+2]] = 1;
                }
            }

            /* Pass 2: build vertex remap + count */
            u32 vc = 0;
            for (u32 vi = 0; vi < td->vert_count; vi++) {
                vmap[vi] = in_chunk.data[vi] ? (u32)vc++ : (u32)-1;
            }

            /* Pass 3: count indices */
            u32 ic = 0;
            for (u32 t = 0; t < tri_count; t++) {
                bool hit = false;
                for (u32 v = 0; v < 3; v++) {
                    u32 vi = td->indices[t * 3 + v];
                    f32 vx = td->positions[vi * 3];
                    f32 vz = td->positions[vi * 3 + 2];
                    if (vx >= x0 && vx < x1 && vz >= z0 && vz < z1) {
                        hit = true;
                        break;
                    }
                }
                if (hit) ic += 3;
            }

            if (vc == 0 || ic == 0) continue;

            /* Allocate contiguous attribute buffers */
            chunks[ci].vert_count  = vc;
            chunks[ci].idx_count   = ic;
            chunks[ci].positions   = malloc(vc * 3 * sizeof(f32));
            chunks[ci].idxs        = malloc(ic * sizeof(u32));
            chunks[ci].has_normals = td->has_normals;
            chunks[ci].has_uvs     = td->has_uvs;
            chunks[ci].has_tangents = td->has_tangents;

            if (td->has_normals) chunks[ci].normals  = malloc(vc * 3 * sizeof(f32));
            if (td->has_uvs)     chunks[ci].uvs      = malloc(vc * 2 * sizeof(f32));
            if (td->has_tangents)chunks[ci].tangents = malloc(vc * 4 * sizeof(f32));

            /* Fill vertex data (contiguous per-attribute) */
            for (u32 vi = 0; vi < td->vert_count; vi++) {
                if (!in_chunk.data[vi]) continue;
                u32 li = vmap[vi];
                chunks[ci].positions[li*3]   = td->positions[vi*3];
                chunks[ci].positions[li*3+1] = td->positions[vi*3+1];
                chunks[ci].positions[li*3+2] = td->positions[vi*3+2];
                if (td->has_normals) {
                    chunks[ci].normals[li*3]   = td->normals[vi*3];
                    chunks[ci].normals[li*3+1] = td->normals[vi*3+1];
                    chunks[ci].normals[li*3+2] = td->normals[vi*3+2];
                }
                if (td->has_uvs) {
                    chunks[ci].uvs[li*2]   = td->uvs[vi*2];
                    chunks[ci].uvs[li*2+1] = td->uvs[vi*2+1];
                }
                if (td->has_tangents) {
                    chunks[ci].tangents[li*4]   = td->tangents[vi*4];
                    chunks[ci].tangents[li*4+1] = td->tangents[vi*4+1];
                    chunks[ci].tangents[li*4+2] = td->tangents[vi*4+2];
                    chunks[ci].tangents[li*4+3] = td->tangents[vi*4+3];
                }
            }

            /* Fill index data */
            u32 ic2 = 0;
            for (u32 t = 0; t < tri_count; t++) {
                bool hit = false;
                for (u32 v = 0; v < 3; v++) {
                    u32 vi = td->indices[t * 3 + v];
                    f32 vx = td->positions[vi * 3];
                    f32 vz = td->positions[vi * 3 + 2];
                    if (vx >= x0 && vx < x1 && vz >= z0 && vz < z1) {
                        hit = true;
                        break;
                    }
                }
                if (hit) {
                    chunks[ci].idxs[ic2++] = vmap[td->indices[t * 3]];
                    chunks[ci].idxs[ic2++] = vmap[td->indices[t * 3+1]];
                    chunks[ci].idxs[ic2++] = vmap[td->indices[t * 3+2]];
                }
            }

            printf("  chunk(%u,%u): %u verts, %u idx\n", gx, gy, vc, ic);
        }
    }

    free(vmap);
    free(in_chunk.data);
}

/* ── build and write GLB ─────────────────────────────────────────────────── */

static void write_glb(FILE *f, u32 grid_x, u32 grid_y,
                      Chunk *chunks, u32 total_chunks) {
    /* Determine which attributes exist (all chunks share the same source terrain) */
    int has_normals = 0, has_uvs = 0, has_tangents = 0;
    for (u32 i = 0; i < total_chunks; i++) {
        if (chunks[i].vert_count == 0) continue;
        has_normals  = chunks[i].has_normals;
        has_uvs      = chunks[i].has_uvs;
        has_tangents = chunks[i].has_tangents;
        break;
    }

    /* Count total bytes per attribute stream + indices */
    size_t total_pos = 0, total_nrm = 0, total_uv = 0, total_tan = 0, total_idx = 0;
    for (u32 i = 0; i < total_chunks; i++) {
        if (chunks[i].vert_count == 0) continue;
        total_pos += (size_t)chunks[i].vert_count * 3 * sizeof(f32);
        if (chunks[i].has_normals)
            total_nrm += (size_t)chunks[i].vert_count * 3 * sizeof(f32);
        if (chunks[i].has_uvs)
            total_uv  += (size_t)chunks[i].vert_count * 2 * sizeof(f32);
        if (chunks[i].has_tangents)
            total_tan += (size_t)chunks[i].vert_count * 4 * sizeof(f32);
        total_idx += (size_t)chunks[i].idx_count * sizeof(u32);
    }

    /* Buffer layout (single buffer, contiguous attribute streams):
     *   [positions...][normals...][uvs...][tangents...][indices...]
     * Each attribute stream is contiguous — no interleaving.
     * This avoids all the glTF stride/byteLength edge cases. */
    size_t off_pos = 0;
    size_t off_nrm = total_pos;
    size_t off_uv  = total_pos + total_nrm;
    size_t off_tan = total_pos + total_nrm + total_uv;
    size_t off_idx = total_pos + total_nrm + total_uv + total_tan;
    size_t bin_size = off_idx + total_idx;

    /* Build JSON in memory */
    size_t json_cap = 256 * 1024;
    char *json = malloc(json_cap);
    size_t json_len = 0;

    #define JAPPEND(fmt, ...) do { \
        int n = snprintf(json + json_len, json_cap - json_len, fmt , ##__VA_ARGS__); \
        if (n < 0) break; \
        size_t need = (size_t)n + 1; \
        if (json_len + need > json_cap) { \
            json_cap = json_cap * 2; \
            json = realloc(json, json_cap); \
        } \
        json_len += (size_t)n; \
    } while(0)

    JAPPEND("{\"asset\":{\"version\":\"2.0\",\"generator\":\"terrain-chunker\"},");

    /* Compute bufferView indices and accessor count per chunk */
    u32 bv_pos = 0, bv_nrm, bv_uv, bv_tan, bv_idx;
    {
        u32 next = 1;
        if (has_normals)  { bv_nrm = next; next++; } else bv_nrm = 0;
        if (has_uvs)      { bv_uv  = next; next++; } else bv_uv = 0;
        if (has_tangents) { bv_tan = next; next++; } else bv_tan = 0;
        bv_idx = next;
    }
    u32 accs_per_chunk = 2 + (has_normals ? 1 : 0) + (has_uvs ? 1 : 0) + (has_tangents ? 1 : 0);

    /* bufferViews: only for attribute streams that have data */
    JAPPEND("\"bufferViews\":[");
    JAPPEND("{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34962}",
            off_pos, total_pos);
    if (has_normals)
        JAPPEND(",{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34962}",
                off_nrm, total_nrm);
    if (has_uvs)
        JAPPEND(",{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34962}",
                off_uv, total_uv);
    if (has_tangents)
        JAPPEND(",{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34962}",
                off_tan, total_tan);
    JAPPEND(",{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34963}",
            off_idx, total_idx);
    JAPPEND("],");

    /* accessors: per-chunk offsets into the global streams */
    JAPPEND("\"accessors\":[");
    {
        size_t p_off = 0, n_off = 0, u_off = 0, t_off = 0, i_off = 0;
        u32 vi = 0;
        for (u32 ci = 0; ci < total_chunks; ci++) {
            if (chunks[ci].vert_count == 0) continue;
            u32 vc = chunks[ci].vert_count;
            u32 ic = chunks[ci].idx_count;

            if (vi > 0) JAPPEND(",");

            JAPPEND("{\"bufferView\":%u,\"byteOffset\":%zu,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\"}",
                    bv_pos, p_off, vc);
            if (has_normals)
                JAPPEND(",{\"bufferView\":%u,\"byteOffset\":%zu,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\"}",
                        bv_nrm, n_off, vc);
            if (has_uvs)
                JAPPEND(",{\"bufferView\":%u,\"byteOffset\":%zu,\"componentType\":5126,\"count\":%u,\"type\":\"VEC2\"}",
                        bv_uv, u_off, vc);
            if (has_tangents)
                JAPPEND(",{\"bufferView\":%u,\"byteOffset\":%zu,\"componentType\":5126,\"count\":%u,\"type\":\"VEC4\"}",
                        bv_tan, t_off, vc);
            JAPPEND(",{\"bufferView\":%u,\"byteOffset\":%zu,\"componentType\":5125,\"count\":%u,\"type\":\"SCALAR\"}",
                    bv_idx, i_off, ic);

            p_off += vc * 3 * sizeof(f32);
            if (has_normals)  n_off += vc * 3 * sizeof(f32);
            if (has_uvs)      u_off += vc * 2 * sizeof(f32);
            if (has_tangents) t_off += vc * 4 * sizeof(f32);
            i_off += ic * sizeof(u32);
            vi++;
        }
    }
    JAPPEND("],");

    /* meshes: one per chunk, only declaring attributes that exist */
    JAPPEND("\"meshes\":[");
    {
        u32 vi = 0;
        for (u32 ci = 0; ci < total_chunks; ci++) {
            if (chunks[ci].vert_count == 0) continue;
            u32 gx = ci % grid_x;
            u32 gy = ci / grid_x;
            u32 acc_base = vi * accs_per_chunk;
            if (vi) JAPPEND(",");
            JAPPEND("{\"name\":\"terrain_chunk_%u_%u\",\"primitives\":[{\"attributes\":{",
                    gx, gy);
            JAPPEND("\"POSITION\":%u", acc_base);
            if (has_normals)
                JAPPEND(",\"NORMAL\":%u", acc_base + 1);
            u32 off = 1 + (has_normals ? 1 : 0);
            if (has_uvs)
                JAPPEND(",\"TEXCOORD_0\":%u", acc_base + off);
            off += (has_uvs ? 1 : 0);
            if (has_tangents)
                JAPPEND(",\"TANGENT\":%u", acc_base + off);
            off += (has_tangents ? 1 : 0);
            JAPPEND("},\"indices\":%u}]}", acc_base + off);
            vi++;
        }
    }
    JAPPEND("],");

    /* nodes: one per chunk */
    JAPPEND("\"nodes\":[");
    {
        u32 vi = 0;
        for (u32 ci = 0; ci < total_chunks; ci++) {
            if (chunks[ci].vert_count == 0) continue;
            u32 gx = ci % grid_x;
            u32 gy = ci / grid_x;
            if (vi) JAPPEND(",");
            JAPPEND("{\"name\":\"terrain_chunk_%u_%u\",\"mesh\":%u,\"extras\":{\"rigidBodyShape\":\"MESH\"}}", gx, gy, vi);
            vi++;
        }
    }
    JAPPEND("],");

    /* scene + scenes (required by glTF 2.0) */
    JAPPEND("\"scenes\":[{\"nodes\":");
    {
        u32 vi = 0;
        u32 valid = 0;
        for (u32 ci = 0; ci < total_chunks; ci++)
            if (chunks[ci].vert_count > 0) valid++;
        JAPPEND("[%u", 0);
        for (u32 i = 1; i < valid; i++) JAPPEND(",%u", (u32)i);
        JAPPEND("]");
    }
    JAPPEND("}],\"scene\":0,");

    JAPPEND("\"buffers\":[{\"byteLength\":%zu}]}", bin_size);

    #undef JAPPEND

    /* Pad JSON to 4-byte boundary */
    size_t json_size = json_len;
    while ((json_size & 3u) != 0) json[json_size++] = ' ';

    /* ── write GLB binary ─────────────────────────────────────────────── */
    u32 total_size = 28 + (u32)json_size + (u32)bin_size;

    write_u32_le(f, 0x46546C67);  /* "glTF" */
    write_u32_le(f, 2);
    write_u32_le(f, total_size);

    write_u32_le(f, (u32)json_size);
    write_u32_le(f, 0x4E4F534A);  /* "JSON" */
    fwrite(json, 1, json_size, f);

    /* BIN chunk — magic must be "BIN\0" (with null), per glTF 2.0 spec: 0x004E4942 */
    write_u32_le(f, (u32)bin_size);
    write_u32_le(f, 0x004E4942);  /* "BIN\0" (0x00 = null) */

    /* Write vertex data per attribute stream */
    for (u32 i = 0; i < total_chunks; i++) {
        if (chunks[i].vert_count == 0) continue;
        fwrite(chunks[i].positions, sizeof(f32), chunks[i].vert_count * 3, f);
    }
    if (has_normals) {
        for (u32 i = 0; i < total_chunks; i++) {
            if (chunks[i].vert_count == 0) continue;
            fwrite(chunks[i].normals, sizeof(f32), chunks[i].vert_count * 3, f);
        }
    }
    if (has_uvs) {
        for (u32 i = 0; i < total_chunks; i++) {
            if (chunks[i].vert_count == 0) continue;
            fwrite(chunks[i].uvs, sizeof(f32), chunks[i].vert_count * 2, f);
        }
    }
    if (has_tangents) {
        for (u32 i = 0; i < total_chunks; i++) {
            if (chunks[i].vert_count == 0) continue;
            fwrite(chunks[i].tangents, sizeof(f32), chunks[i].vert_count * 4, f);
        }
    }
    /* Write index data */
    for (u32 i = 0; i < total_chunks; i++) {
        if (chunks[i].idx_count == 0) continue;
        fwrite(chunks[i].idxs, sizeof(u32), chunks[i].idx_count, f);
    }

    free(json);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <input.glb> <output.glb> <grid_x> <grid_y>\n", argv[0]);
        return 1;
    }

    const char *input_path  = argv[1];
    const char *output_path = argv[2];
    u32 grid_x = (u32)atoi(argv[3]);
    u32 grid_y = (u32)atoi(argv[4]);

    if (grid_x < 1 || grid_y < 1) {
        fprintf(stderr, "Error: grid dimensions must be >= 1\n");
        return 1;
    }

    /* ── parse GLB ──────────────────────────────────────────────────────── */
    cgltf_options options = {0};
    cgltf_data *data = NULL;

    cgltf_result result = cgltf_parse_file(&options, input_path, &data);
    if (result != cgltf_result_success) {
        fprintf(stderr, "Failed to parse %s\n", input_path);
        return 1;
    }
    if (cgltf_load_buffers(&options, data, input_path) != cgltf_result_success) {
        fprintf(stderr, "Failed to load buffers\n");
        cgltf_free(data);
        return 1;
    }
    if (decompress_meshopt(data) != 0) {
        fprintf(stderr, "Failed to decompress meshopt buffers\n");
        cgltf_free(data);
        return 1;
    }

    printf("terrain-chunker: loaded %s (%u meshes, %u nodes)\n",
           input_path, (u32)data->meshes_count, (u32)data->nodes_count);

    /* ── find terrain mesh ──────────────────────────────────────────────── */
    int terrain_mesh_idx = -1;

    for (u32 ni = 0; ni < (u32)data->nodes_count; ni++) {
        cgltf_node *node = &data->nodes[ni];
        if (node->mesh && node->name && str_has_ci(node->name, "terrain")) {
            terrain_mesh_idx = (int)(node->mesh - data->meshes);
            printf("terrain-chunker: terrain at node[%u] \"%s\" -> mesh[%u]\n",
                   ni, node->name, terrain_mesh_idx);
            break;
        }
    }

    if (terrain_mesh_idx < 0) {
        fprintf(stderr, "Error: no terrain mesh found\n");
        cgltf_free(data);
        return 1;
    }

    /* ── extract terrain data ───────────────────────────────────────────── */
    cgltf_primitive *prim = &data->meshes[terrain_mesh_idx].primitives[0];

    cgltf_accessor *pos_acc = NULL, *norm_acc = NULL, *uv_acc = NULL,
                    *tan_acc = NULL, *idx_acc = NULL;
    for (u32 ai = 0; ai < prim->attributes_count; ai++) {
        switch (prim->attributes[ai].type) {
        case cgltf_attribute_type_position: pos_acc = prim->attributes[ai].data; break;
        case cgltf_attribute_type_normal:   norm_acc = prim->attributes[ai].data; break;
        case cgltf_attribute_type_texcoord: uv_acc   = prim->attributes[ai].data; break;
        case cgltf_attribute_type_tangent:  tan_acc  = prim->attributes[ai].data; break;
        default: break;
        }
    }
    idx_acc = prim->indices;

    if (!pos_acc || !idx_acc) {
        fprintf(stderr, "Error: terrain mesh missing POSITION or indices\n");
        cgltf_free(data);
        return 1;
    }

    u32 vert_count = (u32)pos_acc->count;
    u32 idx_count  = (u32)idx_acc->count;

    TerrainData td = {0};
    td.positions  = malloc(vert_count * 3 * sizeof(f32));
    td.indices    = malloc(idx_count * sizeof(u32));
    td.vert_count = vert_count;
    td.idx_count  = idx_count;
    td.has_normals  = norm_acc != NULL;
    td.has_uvs      = uv_acc   != NULL;
    td.has_tangents = tan_acc  != NULL;
    cgltf_accessor_unpack_floats(pos_acc, td.positions, vert_count * 3);
    cgltf_accessor_unpack_indices(idx_acc, td.indices, sizeof(u32), idx_count);
    if (norm_acc) {
        td.normals = malloc(vert_count * 3 * sizeof(f32));
        cgltf_accessor_unpack_floats(norm_acc, td.normals, vert_count * 3);
    }
    if (uv_acc) {
        td.uvs = malloc(vert_count * 2 * sizeof(f32));
        cgltf_accessor_unpack_floats(uv_acc, td.uvs, vert_count * 2);
    }
    if (tan_acc) {
        td.tangents = malloc(vert_count * 4 * sizeof(f32));
        cgltf_accessor_unpack_floats(tan_acc, td.tangents, vert_count * 4);
    }

    printf("terrain-chunker: terrain %u verts, %u indices\n", vert_count, idx_count);

    /* ── bounding box ───────────────────────────────────────────────────── */
    f32 bbMin[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
    f32 bbMax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

    for (u32 i = 0; i < vert_count; i++) {
        for (int d = 0; d < 3; d++) {
            f32 v = td.positions[i*3 + d];
            if (v < bbMin[d]) bbMin[d] = v;
            if (v > bbMax[d]) bbMax[d] = v;
        }
    }

    printf("terrain-chunker: bbox (%.2f,%.2f,%.2f) -> (%.2f,%.2f,%.2f) grid %ux%u\n",
           bbMin[0], bbMin[1], bbMin[2], bbMax[0], bbMax[1], bbMax[2], grid_x, grid_y);

    /* ── build chunks ───────────────────────────────────────────────────── */
    u32 total_chunks = grid_x * grid_y;
    Chunk *chunks = calloc(total_chunks, sizeof(Chunk));
    if (!chunks) {
        fprintf(stderr, "Error: out of memory\n");
        free(td.positions); free(td.indices);
        if (td.normals) free(td.normals);
        if (td.uvs) free(td.uvs);
        if (td.tangents) free(td.tangents);
        cgltf_free(data);
        return 1;
    }

    build_chunks(&td, chunks, total_chunks, grid_x, grid_y, bbMin, bbMax);

    u32 valid_count = 0;
    for (u32 i = 0; i < total_chunks; i++) {
        if (chunks[i].vert_count > 0) valid_count++;
    }

    printf("terrain-chunker: %u/%u valid chunks\n", valid_count, total_chunks);

    if (valid_count == 0) {
        fprintf(stderr, "Error: no chunks produced\n");
        for (u32 i = 0; i < total_chunks; i++) chunk_free(&chunks[i]);
        free(chunks); free(td.positions); free(td.indices);
        if (td.normals) free(td.normals); if (td.uvs) free(td.uvs); if (td.tangents) free(td.tangents);
        cgltf_free(data);
        return 1;
    }

    /* ── write output GLB ───────────────────────────────────────────────── */
    printf("terrain-chunker: writing %s ...\n", output_path);

    FILE *f = fopen(output_path, "wb");
    if (!f) {
        fprintf(stderr, "Error: cannot open %s\n", output_path);
        for (u32 i = 0; i < total_chunks; i++) chunk_free(&chunks[i]);
        free(chunks); free(td.positions); free(td.indices);
        if (td.normals) free(td.normals); if (td.uvs) free(td.uvs); if (td.tangents) free(td.tangents);
        cgltf_free(data);
        return 1;
    }

    write_glb(f, grid_x, grid_y, chunks, total_chunks);
    fclose(f);

    /* Compute output size for display */
    size_t total_size = 0;
    for (u32 i = 0; i < total_chunks; i++) {
        if (chunks[i].vert_count == 0) continue;
        total_size += (size_t)chunks[i].vert_count * 3 * sizeof(f32);
        if (chunks[i].has_normals)  total_size += (size_t)chunks[i].vert_count * 3 * sizeof(f32);
        if (chunks[i].has_uvs)      total_size += (size_t)chunks[i].vert_count * 2 * sizeof(f32);
        if (chunks[i].has_tangents) total_size += (size_t)chunks[i].vert_count * 4 * sizeof(f32);
        total_size += (size_t)chunks[i].idx_count * sizeof(u32);
    }

    printf("terrain-chunker: wrote %.1f MB (%u chunks)\n",
           total_size / (1024.0 * 1024.0), valid_count);

    /* Cleanup */
    for (u32 i = 0; i < total_chunks; i++) chunk_free(&chunks[i]);
    free(chunks);
    free(td.positions); free(td.indices);
    if (td.normals) free(td.normals);
    if (td.uvs) free(td.uvs);
    if (td.tangents) free(td.tangents);
    cgltf_free(data);

    return 0;
}
