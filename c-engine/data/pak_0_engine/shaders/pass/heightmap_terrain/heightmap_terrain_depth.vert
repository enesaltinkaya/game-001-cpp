#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_ARB_shading_language_include : enable

// Depth/velocity pre-pass vertex shader for the heightmap terrain pass.
//
// Same implicit lattice as the scene pass (see heightmap_terrain.vert):
// gl_VertexIndex enumerates the 3 corners of each lattice triangle of a
// plain TRIANGLE list (vkCmdDraw(6 * seg * seg, 1)), the height texture
// lifts them, and the shader emits the clip-space positions for current
// and previous camera (motion vectors for FSR) plus the view-space normal.
// Rendering these tiles into the depth pre-pass keeps downstream passes
// (contact shadows, HiZ, occlusion) and FSR velocity consistent with
// the surface the scene pass draws.

layout(push_constant, std430) uniform HeightmapPC {
    vec4 tile;   // x = originX, y = originZ, z = sizeMeters, w = gridSegments
    vec4 flags;  // x = heightScale, y = texDim (TEX), z/w unused here
} pc;

layout(set = 1, binding = 0) uniform sampler2D heightTex;

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

layout(location = 0) out vec4 outClipCurrent;
layout(location = 1) out vec3 outViewNormal;
layout(location = 2) out vec4 outClipPrev;
layout(location = 3) out vec3 outWorldNormal;

void main() {
    float size   = pc.tile.z;
    float texDim = pc.flags.y;
    float invTex = 1.0 / texDim;
    float k      = 1.0 - invTex;

    int seg   = int(pc.tile.w + 0.5);
    int vi    = gl_VertexIndex;
    int ci    = vi % 3; // corner in triangle: 0, 1, 2
    int ti    = vi / 3;
    int q     = ti / 2; // quad index
    int which = ti % 2; // 0 = (a, c, d), 1 = (a, d, b)
    int px    = q % seg;
    int pz    = q / seg;

    // a=(0,0) b=(1,0) c=(0,1) d=(1,1) in cell units. Winding matches the
    // scene pass / azgaar mesh: (v1-v0) x (v2-v0) points +Y (front face up).
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

    vec2 uvs = (vec2(localX, localZ) / size) * k + 0.5 * invTex;

    float h = texture(heightTex, uvs).r * pc.flags.x;

    // Border-aware finite differences (one-sided stencil on the border
    // texels): the ±1-texel outward fetch would leave [0,1] and wrap
    // (REPEAT sampler) to the opposite tile edge — a garbage steep normal
    // that polluted contact shadows along every seam. See
    // heightmap_terrain.vert.
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

    vec4 worldPos4 = vec4(worldPos, 1.0);
    gl_Position    = sceneBuffer.cameras[0].viewProjection * worldPos4;
    outClipCurrent = sceneBuffer.cameras[0].viewProjectionNoJitter * worldPos4;
    outClipPrev    = sceneBuffer.cameras[0].prevViewProjectionNoJitter * worldPos4;
    outViewNormal  = normalize((sceneBuffer.cameras[0].view * vec4(normal, 0.0)).xyz);
    outWorldNormal = normal;
}