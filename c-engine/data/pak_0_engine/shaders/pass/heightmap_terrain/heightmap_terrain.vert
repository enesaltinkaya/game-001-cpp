#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_ARB_shading_language_include : enable

// Implicit-lattice vertex shader for the heightmap terrain pass.
//
// There is no VBO/IBO: each draw is vkCmdDraw(6 * seg * seg, 1) over a
// plain TRIANGLE list and the vertex shader enumerates the lattice
// triangle corners from gl_VertexIndex (vertex i is corner i % 3 of
// triangle i / 3).  Each lattice quad of the tile (full tile edge, both
// border endpoints included) is split into the two front-facing triangles
// (a, c, d) and (a, d, b) with a=(x,z), b=(x+1,z), c=(x,z+1), d=(x+1,z+1)
// — the same winding as the experimental azgaar mesh pass, i.e.
// (v1-v0) x (v2-v0) points +Y so the up-facing side is CCW in window
// space (frontFace=CCW, back-face culling).  Border corners land exactly
// on tile borders and sample the shared border texels — neighbouring tiles
// produce bit-identical border heights (watertight: no cracks, no LOD
// pinning, no transition meshes).  Lattice corners shared by adjacent
// triangles are recomputed by the vertex shader (a few extra texture
// fetches); the surface is still exactly the tensor-product bilinear
// surface the CPU/physics grids define (heights are the final metres baked
// into the tile's height texture — macro band from the .map + geometry-band
// fBm).
//
// Heights are addressed at texel centres: uv = (local/size) * (TEX-1)/TEX +
// 0.5/TEX, so bilinear filtering through texel centres IS the bilinear
// interpolation of the TEX^2 control points: GPU surface == CPU-sampled
// surface == Jolt heightfield surface.
//
// The corner normal comes from ±1-texel neighbours (1 texel == size/(TEX-1)
// metres); it is rasterizer-interpolated across the triangle, and the
// fragment shader refines it with a micro-band procedural perturbation
// (4-16 m wavelengths that no grid resolves).

layout(push_constant, std430) uniform HeightmapPC {
    vec4 tile;   // x = originX, y = originZ, z = sizeMeters, w = gridSegments
    vec4 flags;  // x = heightScale, y = texDim (TEX, e.g. 512),
                 // z = wireFrame, w = debugHeightRamp
} pc;

layout(set = 1, binding = 0) uniform sampler2D heightTex;

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec3 outWorldPos;

void main() {
    float size   = pc.tile.z;
    float texDim = pc.flags.y;
    float invTex = 1.0 / texDim;
    float k      = 1.0 - invTex; // (TEX-1)/TEX

    int seg   = int(pc.tile.w + 0.5);
    int vi    = gl_VertexIndex;
    int ci    = vi % 3; // corner in triangle: 0, 1, 2
    int ti    = vi / 3;
    int q     = ti / 2; // quad index
    int which = ti % 2; // 0 = (a, c, d), 1 = (a, d, b)
    int px    = q % seg;
    int pz    = q / seg;

    // a=(0,0) b=(1,0) c=(0,1) d=(1,1) in cell units. Winding matches the
    // azgaar mesh pass: (v1-v0) x (v2-v0) points +Y (front face up).
    float u, v;
    if (which == 0) {
        u = float(ci == 2);  // a, c, d
        v = float(ci >= 1);
    } else {
        u = float(ci >= 1);  // a, d, b
        v = float(ci == 1);
    }

    float cell   = size / float(seg);
    float localX = (float(px) + u) * cell;
    float localZ = (float(pz) + v) * cell;

    // Texel-centre addressing (see header comment).
    vec2 uvs = (vec2(localX, localZ) / size) * k + 0.5 * invTex;

    float h = texture(heightTex, uvs).r * pc.flags.x;

    // ±1 texel in UV space == size/(TEX-1) metres in world space. On the
    // border texels the outward fetch would leave [0,1] and — with the
    // REPEAT addressing of the linear sampler — wrap to the OPPOSITE edge
    // of the same tile (2 km away), producing a garbage near-horizontal
    // normal that the fragment shader then shades as a cliff. Use a
    // one-sided stencil (span 1 texel) on the border texels instead; the
    // inward fetch stays inside the tile.
    float cellTexel = size / (texDim - 1.0);
    float spanX     = 2.0 * cellTexel;
    float spanZ     = 2.0 * cellTexel;
    float hL, hR, hD, hU;
    if (uvs.x < invTex) {
        hL = h;
        hR = texture(heightTex, uvs + vec2(invTex, 0.0)).r * pc.flags.x;
        spanX = cellTexel;
    } else if (uvs.x > 1.0 - invTex) {
        hR = h;
        hL = texture(heightTex, uvs - vec2(invTex, 0.0)).r * pc.flags.x;
        spanX = cellTexel;
    } else {
        hL = texture(heightTex, uvs - vec2(invTex, 0.0)).r * pc.flags.x;
        hR = texture(heightTex, uvs + vec2(invTex, 0.0)).r * pc.flags.x;
    }
    if (uvs.y < invTex) {
        hD = h;
        hU = texture(heightTex, uvs + vec2(0.0, invTex)).r * pc.flags.x;
        spanZ = cellTexel;
    } else if (uvs.y > 1.0 - invTex) {
        hU = h;
        hD = texture(heightTex, uvs - vec2(0.0, invTex)).r * pc.flags.x;
        spanZ = cellTexel;
    } else {
        hD = texture(heightTex, uvs - vec2(0.0, invTex)).r * pc.flags.x;
        hU = texture(heightTex, uvs + vec2(0.0, invTex)).r * pc.flags.x;
    }

    vec3 worldPos = vec3(pc.tile.x + localX, h, pc.tile.y + localZ);
    vec3 normal   = normalize(vec3((hL - hR) / spanX, 1.0, (hD - hU) / spanZ));

    gl_Position = sceneBuffer.cameras[0].viewProjection * vec4(worldPos, 1.0);
    outNormal   = normal;
    outWorldPos = worldPos;
}