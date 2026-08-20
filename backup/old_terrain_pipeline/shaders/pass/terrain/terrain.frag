#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_ARB_shading_language_include : enable

// ── Performance-tweak toggles (set to 0 to disable) ────────────────────
#define TERRAIN_ENABLE_POM 1
#define TERRAIN_ENABLE_SPLAT_PAINT 1
#define TERRAIN_ENABLE_CLIFF_TRIPLANAR 1
#define TERRAIN_ENABLE_SHADOWS 1
#define TERRAIN_ENABLE_IBL 1
#define TERRAIN_ENABLE_FORWARD_PLUS 1
#define TERRAIN_ENABLE_GRASS_VARIATION 1
// Set to 1 to shade Azgaar terrain cells with solid per-cell ID colors,
// bypassing terrain texture color sampling while keeping lighting/shadows.
#define TERRAIN_DEBUG_AZGAAR_CELL_IDS 0
// ──────────────────────────────────────────────────────────────────────

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec3 inWorldPos;
layout(location = 4) flat in uint inAzgaarCellId;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outNormal;
layout(location = 2) out vec4 outMaterial;

layout(push_constant) uniform PushConstants {
    uint materialId;
    uint wireFrame;
};

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"
#include "../../includes/pom.shader"
#include "../../includes/shadow.shader"
#include "../../includes/forwardplus.shader"
#include "../../includes/light_probe_gi.shader"

// ── Splatmap helpers ────────────────────────────────────────────────
#define SPLAT_DETAIL_TILE 1024.0
#define DEFAULT_GRASS_DETAIL_TILE 2048.0
#define DEFAULT_CLIFF_DETAIL_TILE 32.0
#define SPLAT_NORMAL_STRENGTH 2.0
// Fixed reference span (meters) used to convert the *_DETAIL_TILE repeat
// counts into a world-space tiling frequency. Deliberately NOT the streaming
// terrain AABB (sceneBuffer.terrain.worldMin/worldMax): that AABB shifts by a
// few percent whenever the Azgaar tile grid is recentered on the player, which
// would rescale and visibly shift the grass/detail pattern at every tile
// transition. A fixed reference anchors the pattern in world space regardless
// of which tiles are loaded. ~7000 m matches the typical 3x3 Azgaar grid span.
#define TERRAIN_DETAIL_REFERENCE_METERS 7000.0

// Per-layer POM depth in world-space meters
#define POM_DEPTH_GRASS 0.025
#define POM_DEPTH_CLIFF 0.15
#define POM_DEPTH_SPLAT 0.5

#define AZGAAR_INVALID_CELL_ID 0xffffffffu

struct SplatResult {
    vec3 albedo;
    float roughness;
    vec3 normal;  // tangent-space normal
    float ao;
    float displacement;
    float paintWeight;  // total splat paint weight (0 = unpainted)
};

vec4 sampleMaterialTexture(uint texIndex, uint samplerIndex, vec2 uv) {
    return texture(
        sampler2D(textures[nonuniformEXT(texIndex)], samplers[nonuniformEXT(samplerIndex)]),
        uv);
}

vec3 safeNormalize(vec3 v, vec3 fallback) {
    float len2 = dot(v, v);
    return (len2 > 1e-8) ? (v * inversesqrt(len2)) : fallback;
}

vec3 azgaarCellIdColor(uint cellId) {
    uint x = cellId * 747796405u + 2891336453u;
    x      = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
    x      = (x >> 22u) ^ x;

    vec3 c =
        vec3(float((x >> 0u) & 255u), float((x >> 8u) & 255u), float((x >> 16u) & 255u)) / 255.0;
    return vec3(0.18) + (c - vec3(0.18)) * 0.82;
}

mat3 buildTBN(vec3 geomNormal, vec4 tangentAttr, vec2 uv, vec3 worldPos) {
    vec3 N = safeNormalize(geomNormal, vec3(0.0, 1.0, 0.0));
    vec3 T = tangentAttr.xyz;

    if (dot(T, T) > 1e-6) {
        T      = safeNormalize(T - N * dot(N, T), vec3(1.0, 0.0, 0.0));
        vec3 B = safeNormalize(cross(N, T), vec3(0.0, 0.0, 1.0)) * tangentAttr.w;
        if (dot(B, B) > 1e-6) {
            return mat3(T, B, N);
        }
    }

    vec3 dpdx  = dFdx(worldPos);
    vec3 dpdy  = dFdy(worldPos);
    vec2 dUVdx = dFdx(uv);
    vec2 dUVdy = dFdy(uv);
    float det  = dUVdx.x * dUVdy.y - dUVdx.y * dUVdy.x;

    if (abs(det) > 1e-8) {
        float invDet = 1.0 / det;
        T            = (dpdx * dUVdy.y - dpdy * dUVdx.y) * invDet;
        vec3 B       = (dpdy * dUVdx.x - dpdx * dUVdy.x) * invDet;

        T = safeNormalize(T - N * dot(N, T), vec3(1.0, 0.0, 0.0));
        B = safeNormalize(B - N * dot(N, B) - T * dot(T, B),
                          safeNormalize(cross(N, T), vec3(0.0, 0.0, 1.0)));
        if (tangentAttr.w < 0.0) {
            B = -B;
        }
        return mat3(T, B, N);
    }

    vec3 up = (abs(N.y) < 0.999) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    T       = safeNormalize(cross(up, N), vec3(1.0, 0.0, 0.0));
    vec3 B  = safeNormalize(cross(N, T), vec3(0.0, 0.0, 1.0));
    if (tangentAttr.w < 0.0) {
        B = -B;
    }
    return mat3(T, B, N);
}

mat3 buildTerrainTBN(vec3 geomNormal) {
    vec3 N = safeNormalize(geomNormal, vec3(0.0, 1.0, 0.0));
    vec3 T = vec3(1.0, 0.0, 0.0) - N * N.x;
    T      = safeNormalize(T, vec3(1.0, 0.0, 0.0));

    vec3 B = vec3(0.0, 0.0, 1.0) - N * N.z - T * dot(T, vec3(0.0, 0.0, 1.0));
    B      = safeNormalize(B, safeNormalize(cross(T, N), vec3(0.0, 0.0, 1.0)));

    return mat3(T, B, N);
}

// ── Triplanar sampling for cliff textures ───────────────────────────
#define CLIFF_TRIPLANAR_SCALE (DEFAULT_CLIFF_DETAIL_TILE / 4096.0)

vec3 triplanarWeights(vec3 worldNormal, float sharpness) {
    vec3 w = abs(worldNormal);
    w      = pow(w, vec3(sharpness));
    return w / (w.x + w.y + w.z + 1e-6);
}

SplatResult sampleTerrainLayerTriplanar(uint albedoIdx,
                                        uint normalIdx,
                                        vec3 worldPos,
                                        vec3 worldNormal) {
    SplatResult r;
    vec3 w = triplanarWeights(worldNormal, 4.0);

    vec2 uvX = worldPos.zy * CLIFF_TRIPLANAR_SCALE;
    vec2 uvY = worldPos.xz * CLIFF_TRIPLANAR_SCALE;
    vec2 uvZ = worldPos.xy * CLIFF_TRIPLANAR_SCALE;

    if (albedoIdx != 0u) {
        vec4 a      = sampleMaterialTexture(albedoIdx, SAMPLER_LINEAR, uvX) * w.x +
                      sampleMaterialTexture(albedoIdx, SAMPLER_LINEAR, uvY) * w.y +
                      sampleMaterialTexture(albedoIdx, SAMPLER_LINEAR, uvZ) * w.z;
        r.albedo    = a.rgb;
        r.roughness = a.a;
    } else {
        r.albedo    = vec3(0.5);
        r.roughness = 0.5;
    }
    if (normalIdx != 0u) {
        vec4 n   = sampleMaterialTexture(normalIdx, SAMPLER_LINEAR, uvX) * w.x +
                   sampleMaterialTexture(normalIdx, SAMPLER_LINEAR, uvY) * w.y +
                   sampleMaterialTexture(normalIdx, SAMPLER_LINEAR, uvZ) * w.z;
        vec2 nxy = n.rg * 2.0 - 1.0;
        nxy *= SPLAT_NORMAL_STRENGTH;
        float nz       = sqrt(max(1.0 - dot(nxy, nxy), 0.0));
        r.normal       = vec3(nxy, nz);
        r.ao           = n.b;
        r.displacement = n.a;
    } else {
        r.normal       = vec3(0.0, 0.0, 1.0);
        r.ao           = 1.0;
        r.displacement = 0.0;
    }
    return r;
}

SplatResult sampleTerrainLayer(uint albedoIdx, uint normalIdx, vec2 tiledUV) {
    SplatResult r;
    if (albedoIdx != 0u) {
        vec4 a      = sampleMaterialTexture(albedoIdx, SAMPLER_LINEAR, tiledUV);
        r.albedo    = a.rgb;
        r.roughness = a.a;
    } else {
        r.albedo    = vec3(0.5);
        r.roughness = 0.5;
    }
    if (normalIdx != 0u) {
        vec4 n   = sampleMaterialTexture(normalIdx, SAMPLER_LINEAR, tiledUV);
        vec2 nxy = n.rg * 2.0 - 1.0;
        nxy *= SPLAT_NORMAL_STRENGTH;
        float nz       = sqrt(max(1.0 - dot(nxy, nxy), 0.0));
        r.normal       = vec3(nxy, nz);
        r.ao           = n.b;
        r.displacement = n.a;
    } else {
        r.normal       = vec3(0.0, 0.0, 1.0);
        r.ao           = 1.0;
        r.displacement = 0.0;
    }
    return r;
}

SplatResult blendSplatResults(SplatResult a, SplatResult b, float t) {
    SplatResult r;
    r.albedo       = mix(a.albedo, b.albedo, t);
    r.roughness    = mix(a.roughness, b.roughness, t);
    r.normal       = normalize(mix(a.normal, b.normal, t));
    r.ao           = mix(a.ao, b.ao, t);
    r.displacement = mix(a.displacement, b.displacement, t);
    r.paintWeight  = max(a.paintWeight, b.paintWeight);
    return r;
}

// Height-based blending: bias toward the layer with greater displacement.
#define HEIGHT_BLEND_DEPTH 0.2

// ── Noise helpers for terrain grass variation ─────────────────────
// Hash that works well on large float coordinates (split into int parts
// to avoid precision issues in fract()).
float grassHash21(vec2 p, float seed) {
    ivec2 ip = ivec2(mod(floor(p), 4096.0));
    uint n   = uint(ip.x) * 374761393u + uint(ip.y) * 668265263u + uint(seed);
    n        = n * 0x27d4eb2dU;
    return float((n >> 16) & 0x7fffu) / 32768.0;  // [0, 1)
}

// Gradient noise with smoothstep (not value noise) — no cell-center
// peaks, so UV perturbation doesn't create bright spots.
float grassGradientNoise(vec2 uv, float seed) {
    vec2 i = floor(uv);
    vec2 f = fract(uv);
    vec2 u = f * f * (3.0 - 2.0 * f);  // smoothstep

    vec4 g  = vec4(grassHash21(i, seed),
                   grassHash21(i + vec2(1, 0), seed + 1.0),
                   grassHash21(i + vec2(0, 1), seed + 2.0),
                   grassHash21(i + vec2(1, 1), seed + 3.0));
    vec4 ga = g * 6.2831853f;
    vec2 gx = vec2(cos(ga.x), cos(ga.y));

    float d00 = dot(gx.xx, vec2(0, 0) - f) + (grassHash21(i, seed + 100.0) - 0.5);
    float d10 = dot(gx.yy, vec2(1, 0) - f) + (grassHash21(i + vec2(1, 0), seed + 101.0) - 0.5);
    float d01 = dot(gx.xx, vec2(0, 1) - f) + (grassHash21(i + vec2(0, 1), seed + 102.0) - 0.5);
    float d11 = dot(gx.yy, vec2(1, 1) - f) + (grassHash21(i + vec2(1, 1), seed + 103.0) - 0.5);

    return mix(mix(d00, d10, u.x), mix(d01, d11, u.x), u.y);
}

float grassFBM(vec2 uv, float seed) {
    float v = grassGradientNoise(uv * 0.05, seed) * 0.65;
    v += grassGradientNoise(rotateUV(uv * 0.12, 0.7), seed + 50.0) * 0.35;
    return v;
}

// Per-tile UV perturbation: each texture tile gets a random offset +
// rotation + scale so adjacent tiles don't look identical.  Smoothly
// interpolated at tile boundaries to avoid seams.
vec2 grassPerturbUV(vec2 uv, float seed) {
    vec2 tile = floor(uv);
    vec2 frac = fract(uv);
    vec2 w    = frac * frac * frac * (frac * (frac * 6.0 - 15.0) + 10.0);

    // Hash per tile for offset, rotation, scale
    vec2 offset;
    float angle, scale;

    // Corner 00
    vec2 o00  = (vec2(grassHash21(tile, seed + 0.0), grassHash21(tile, seed + 1.0)) - 0.5) * 0.4;
    float a00 = (grassHash21(tile, seed + 2.0) - 0.5) * 0.8;
    float s00 = 0.75 + grassHash21(tile, seed + 3.0) * 0.5;

    // Corner 10
    vec2 o10  = (vec2(grassHash21(tile + vec2(1, 0), seed + 0.0),
                      grassHash21(tile + vec2(1, 0), seed + 1.0)) -
                 0.5) *
                0.4;
    float a10 = (grassHash21(tile + vec2(1, 0), seed + 2.0) - 0.5) * 0.8;
    float s10 = 0.75 + grassHash21(tile + vec2(1, 0), seed + 3.0) * 0.5;

    // Corner 01
    vec2 o01  = (vec2(grassHash21(tile + vec2(0, 1), seed + 0.0),
                      grassHash21(tile + vec2(0, 1), seed + 1.0)) -
                 0.5) *
                0.4;
    float a01 = (grassHash21(tile + vec2(0, 1), seed + 2.0) - 0.5) * 0.8;
    float s01 = 0.75 + grassHash21(tile + vec2(0, 1), seed + 3.0) * 0.5;

    // Corner 11
    vec2 o11  = (vec2(grassHash21(tile + vec2(1, 1), seed + 0.0),
                      grassHash21(tile + vec2(1, 1), seed + 1.0)) -
                 0.5) *
                0.4;
    float a11 = (grassHash21(tile + vec2(1, 1), seed + 2.0) - 0.5) * 0.8;
    float s11 = 0.75 + grassHash21(tile + vec2(1, 1), seed + 3.0) * 0.5;

    // Interpolate
    offset = mix(mix(o00, o10, w.x), mix(o01, o11, w.x), w.y);
    angle  = mix(mix(a00, a10, w.x), mix(a01, a11, w.x), w.y);
    scale  = mix(mix(s00, s10, w.x), mix(s01, s11, w.x), w.y);

    // Apply transform: rotate + scale around tile center, then add offset
    vec2 center = frac - 0.5;
    float ca = cos(angle), sa = sin(angle);
    vec2 rotated = vec2(center.x * ca - center.y * sa, center.x * sa + center.y * ca);
    rotated *= scale;
    return rotated + 0.5 + offset;
}

// Sample a terrain layer using textureGrad (for POM-offset UVs)
SplatResult sampleTerrainLayerGrad(uint albedoIdx,
                                   uint normalIdx,
                                   vec2 tiledUV,
                                   vec2 ddx,
                                   vec2 ddy) {
    SplatResult r;
    if (albedoIdx != 0u) {
        vec4 a      = sampleMaterialTextureGrad(albedoIdx, SAMPLER_LINEAR, tiledUV, ddx, ddy);
        r.albedo    = a.rgb;
        r.roughness = a.a;
    } else {
        r.albedo    = vec3(0.5);
        r.roughness = 0.5;
    }
    if (normalIdx != 0u) {
        vec4 n   = sampleMaterialTextureGrad(normalIdx, SAMPLER_LINEAR, tiledUV, ddx, ddy);
        vec2 nxy = n.rg * 2.0 - 1.0;
        nxy *= SPLAT_NORMAL_STRENGTH;
        float nz       = sqrt(max(1.0 - dot(nxy, nxy), 0.0));
        r.normal       = vec3(nxy, nz);
        r.ao           = n.b;
        r.displacement = n.a;
    } else {
        r.normal       = vec3(0.0, 0.0, 1.0);
        r.ao           = 1.0;
        r.displacement = 0.0;
    }
    return r;
}

SplatResult blendSplatResultsHeight(SplatResult a, SplatResult b, float t) {
    float ha = a.displacement + (1.0 - t);
    float hb = b.displacement + t;
    float ma = max(ha, hb) - HEIGHT_BLEND_DEPTH;
    float wa = max(ha - ma, 0.0);
    float wb = max(hb - ma, 0.0);
    float ht = (wa + wb > 1e-5) ? (wb / (wa + wb)) : t;

    SplatResult r;
    r.albedo       = mix(a.albedo, b.albedo, ht);
    r.roughness    = mix(a.roughness, b.roughness, ht);
    r.normal       = normalize(mix(a.normal, b.normal, ht));
    r.ao           = mix(a.ao, b.ao, ht);
    r.displacement = mix(a.displacement, b.displacement, ht);
    r.paintWeight  = max(a.paintWeight, b.paintWeight);
    return r;
}

// ── Main splatmap terrain sampling ──────────────────────────────────
SplatResult sampleSplatTerrain(vec2 meshUV,
                               vec3 worldPos,
                               Material material,
                               vec3 worldNormal,
                               vec3 viewDirTS,
                               float pomFade) {
    // UDIM tile selection is computed from world-space position
    // (mesh UVs may be zeroed by gltfpack when no material references them).

    // World-space tiling for detail textures (avoids chunk-boundary seams).
    // Uses the FIXED reference span, not the streaming terrain AABB, so the
    // pattern stays anchored in world space across tile-grid rebuilds.
    // (terrainWorldSize is still needed below for the splat UDIM grid mapping.)
    vec2 terrainWorldSize = sceneBuffer.terrain.worldMax.xz - sceneBuffer.terrain.worldMin.xz;

    vec2 tiledUV = worldPos.xz * (SPLAT_DETAIL_TILE / TERRAIN_DETAIL_REFERENCE_METERS);
    vec2 tiledUVDefaultGrass =
        worldPos.xz * (DEFAULT_GRASS_DETAIL_TILE / TERRAIN_DETAIL_REFERENCE_METERS);

    // Slope-based default layer (grass on flat, cliff on steep)
    float slope      = 1.0 - max(dot(normalize(worldNormal), vec3(0.0, 1.0, 0.0)), 0.0);
    float cliffBlend = smoothstep(0.1, 0.4, slope);

    // Pre-compute UV gradients BEFORE any POM offset (critical for correct mip selection)
    vec2 dUVdx_splat = dFdx(tiledUV);
    vec2 dUVdy_splat = dFdy(tiledUV);
    vec2 dUVdx_grass = dFdx(tiledUVDefaultGrass);
    vec2 dUVdy_grass = dFdy(tiledUVDefaultGrass);

    // ── POM: determine dominant layer and apply UV offset ──────────────
    float hScaleSplat = POM_DEPTH_SPLAT * SPLAT_DETAIL_TILE / TERRAIN_DETAIL_REFERENCE_METERS;
    float hScaleGrass =
        POM_DEPTH_GRASS * DEFAULT_GRASS_DETAIL_TILE / TERRAIN_DETAIL_REFERENCE_METERS;

    // Single-pass splat blending — collect layer data first, then apply
    // POM offset using the dominant layer (eliminates POM pre-scan).
    vec2 terrainUV =
        clamp((worldPos.xz - sceneBuffer.terrain.worldMin.xz) / max(terrainWorldSize, vec2(1.0)),
              vec2(0.0),
              vec2(1.0));
    vec2 splatGrid = clamp(terrainUV * 10.0, vec2(0.0), vec2(9.999999));
    ivec2 tile     = ivec2(floor(splatGrid));
    vec2 localUV   = fract(splatGrid);
    uint tileIdx   = uint(tile.y) * 10u + uint(tile.x);

// Collect layer data in one pass (weights + texture indices)
// Capped at 2 to limit detail texture samples (16 -> 4 max)
#define MAX_ACTIVE_SPLAT_LAYERS 2
    float layerWeights[MAX_ACTIVE_SPLAT_LAYERS];
    float layerHeights[MAX_ACTIVE_SPLAT_LAYERS];
    uint layerAlbedoIdx[MAX_ACTIVE_SPLAT_LAYERS];
    uint layerNormalIdx[MAX_ACTIVE_SPLAT_LAYERS];
    uint activeLayerCount  = 0u;
    float rawTotalWeight   = 0.0;
    float dominantWeight   = 0.0;
    uint dominantNormalIdx = 0u;

    for (uint g = 0u; g < sceneBuffer.terrain.splatGroupCount && g < MAX_SPLAT_GROUPS; g++) {
        uint weightTexId = sceneBuffer.terrain.splatGroups[g].weightTextures[tileIdx];
        if (weightTexId == 0u) continue;
        vec4 weights = sampleMaterialTexture(weightTexId, SAMPLER_SPLATMAP, localUV);
        weights.a    = 1.0 - weights.a;
        uint base    = g * MAX_SPLAT_CHANNELS;

        for (uint ch = 0u; ch < MAX_SPLAT_CHANNELS; ch++) {
            float w = weights[ch];
            if (w < 0.001) continue;

            uint albedoIdx = material.splatAlbedoTextures[base + ch];
            uint normalIdx = material.splatNormalTextures[base + ch];
            if (albedoIdx == 0u || normalIdx == 0u) continue;
            if (activeLayerCount >= MAX_ACTIVE_SPLAT_LAYERS) break;

            layerWeights[activeLayerCount]   = w;
            layerAlbedoIdx[activeLayerCount] = albedoIdx;
            layerNormalIdx[activeLayerCount] = normalIdx;
            activeLayerCount++;
            rawTotalWeight += w;
            if (w > dominantWeight) {
                dominantWeight    = w;
                dominantNormalIdx = normalIdx;
            }
        }
    }

    // Apply POM UV offset using the dominant layer found above
    if (TERRAIN_ENABLE_POM != 0 && pomFade > 0.001 && activeLayerCount > 0u) {
        if (rawTotalWeight > 0.5 && dominantNormalIdx != 0u) {
            tiledUV = pomOffsetUV(dominantNormalIdx,
                                  SAMPLER_LINEAR,
                                  tiledUV,
                                  viewDirTS,
                                  hScaleSplat,
                                  pomFade,
                                  dUVdx_splat,
                                  dUVdy_splat);
        } else if (cliffBlend < 0.5 && sceneBuffer.terrain.grassNormalIndex != 0u) {
            tiledUVDefaultGrass = pomOffsetUV(sceneBuffer.terrain.grassNormalIndex,
                                              SAMPLER_LINEAR,
                                              tiledUVDefaultGrass,
                                              viewDirTS,
                                              hScaleGrass,
                                              pomFade,
                                              dUVdx_grass,
                                              dUVdy_grass);
        }
    }

    // Sample default grass layer using textureGrad (correct mip with POM-offset UVs)
    // NOTE: grassPerturbUV must NOT be applied after POM offset — the nonlinear
    // transform would invalidate the POM displacement.  Perturbation is applied
    // to the color variation instead (via FBM noise) to break tiling.
    SplatResult grassResult = sampleTerrainLayerGrad(sceneBuffer.terrain.grassAlbedoIndex,
                                                     sceneBuffer.terrain.grassNormalIndex,
                                                     tiledUVDefaultGrass,
                                                     dUVdx_grass,
                                                     dUVdy_grass);

// ── Low-frequency color variation (warm/cool + brightness) ───────
#if TERRAIN_ENABLE_GRASS_VARIATION
    {
        float n          = grassFBM(worldPos.xz * 0.08, 10.0);
        vec3 warmShift   = vec3(1.04, 1.02, 0.96);
        vec3 coolShift   = vec3(0.96, 1.00, 1.03);
        vec3 tint        = mix(coolShift, warmShift, n);
        float brightness = 0.94 + n * 0.12;
        grassResult.albedo *= tint * brightness;
    }

    // ── High-frequency micro variation (small-scale detail) ──────────
    {
        float mn          = grassFBM(worldPos.xz * 0.35, 60.0);
        vec3 microTint    = mix(vec3(0.98, 1.00, 1.01), vec3(1.02, 1.01, 0.98), mn);
        float microBright = 0.96 + mn * 0.08;
        grassResult.albedo *= microTint * microBright;
    }
#endif

    // Blend grass → cliff based on slope (triplanar for cliff)
    SplatResult result;
#if TERRAIN_ENABLE_CLIFF_TRIPLANAR
    if (cliffBlend > 0.01) {
        SplatResult cliffResult = sampleTerrainLayerTriplanar(sceneBuffer.terrain.cliffAlbedoIndex,
                                                              sceneBuffer.terrain.cliffNormalIndex,
                                                              worldPos,
                                                              worldNormal);
        result                  = blendSplatResultsHeight(grassResult, cliffResult, cliffBlend);
    } else {
        result = grassResult;
    }
#else
    result = grassResult;
#endif

// ── Splat-painted UDIM refinement layer ────────────────────────
#if TERRAIN_ENABLE_SPLAT_PAINT
    // Sample detail textures at POM-corrected UVs (UVs already offset above)
    float layerRoughness[MAX_ACTIVE_SPLAT_LAYERS];
    vec3 layerAlbedo[MAX_ACTIVE_SPLAT_LAYERS];
    vec3 layerNormal[MAX_ACTIVE_SPLAT_LAYERS];
    float layerAo[MAX_ACTIVE_SPLAT_LAYERS];

    for (uint i = 0u; i < activeLayerCount; i++) {
        vec4 albedoSample = sampleMaterialTextureGrad(layerAlbedoIdx[i],
                                                      SAMPLER_LINEAR,
                                                      tiledUV,
                                                      dUVdx_splat,
                                                      dUVdy_splat);
        vec4 normalSample = sampleMaterialTextureGrad(layerNormalIdx[i],
                                                      SAMPLER_LINEAR,
                                                      tiledUV,
                                                      dUVdx_splat,
                                                      dUVdy_splat);
        vec2 nxy          = normalSample.rg * 2.0 - 1.0;
        nxy *= SPLAT_NORMAL_STRENGTH;
        float nz = sqrt(max(1.0 - dot(nxy, nxy), 0.0));

        layerHeights[i]   = normalSample.a;
        layerAlbedo[i]    = albedoSample.rgb;
        layerRoughness[i] = albedoSample.a;
        layerNormal[i]    = vec3(nxy, nz);
        layerAo[i]        = normalSample.b;
    }

    // Compute height-biased weights and accumulate
    if (activeLayerCount > 0u && rawTotalWeight > 0.001) {
        float maxH = -1.0;
        for (uint i = 0u; i < activeLayerCount; i++) {
            maxH = max(maxH, layerHeights[i] + layerWeights[i]);
        }
        float threshold = maxH - HEIGHT_BLEND_DEPTH;

        float totalWeight = 0.0;
        SplatResult splatAccum;
        splatAccum.albedo       = vec3(0.0);
        splatAccum.roughness    = 0.0;
        splatAccum.normal       = vec3(0.0);
        splatAccum.ao           = 0.0;
        splatAccum.displacement = 0.0;

        for (uint i = 0u; i < activeLayerCount; i++) {
            float biased = layerHeights[i] + layerWeights[i];
            float hw     = max(biased - threshold, 0.0);
            splatAccum.albedo += layerAlbedo[i] * hw;
            splatAccum.roughness += layerRoughness[i] * hw;
            splatAccum.normal += layerNormal[i] * hw;
            splatAccum.ao += layerAo[i] * hw;
            splatAccum.displacement += layerHeights[i] * hw;
            totalWeight += hw;
        }

        if (totalWeight > 1e-5) {
            float invW = 1.0 / totalWeight;
            splatAccum.albedo *= invW;
            splatAccum.roughness *= invW;
            splatAccum.normal = normalize(splatAccum.normal);
            splatAccum.ao *= invW;
            splatAccum.displacement *= invW;

            float splatInfluence = clamp(rawTotalWeight * 2.0, 0.0, 1.0);
            result               = blendSplatResultsHeight(result, splatAccum, splatInfluence);
        }
    }
#endif

    result.paintWeight = rawTotalWeight;
    return result;
}

// ── Main ────────────────────────────────────────────────────────────
void main() {
    vec3 geometryNormal = safeNormalize(inNormal, vec3(0.0, 1.0, 0.0));

    bool azgaarCellDebug = false;
#if TERRAIN_DEBUG_AZGAAR_CELL_IDS
    azgaarCellDebug = inAzgaarCellId != AZGAAR_INVALID_CELL_ID;
#endif

    Material material = materialBuffer.materials[materialId];

    // Azgaar top-face overlay: CPU precomputes a blended debug tint per vertex.
    // Border vertices average neighboring cell colors, so seams disappear and
    // triple-cell junctions fade smoothly instead of snapping cell-by-cell.
    vec3 azgaarOverlayColor = clamp(inTangent.rgb, vec3(0.0), vec3(1.0));
    float azgaarDebugStrength =
        azgaarCellDebug
            ? 0.0
            : (inAzgaarCellId != AZGAAR_INVALID_CELL_ID ? 1.0 : 0.0);

    // POM: compute tangent-space view direction and distance fade
    mat3 terrainTBN = buildTerrainTBN(geometryNormal);
    vec3 V_world    = normalize(sceneBuffer.cameras[0].position.xyz - inWorldPos);
    vec3 viewDirTS  = normalize(transpose(terrainTBN) * V_world);

    float dist    = length(sceneBuffer.cameras[0].position.xyz - inWorldPos);
    float pomFade = (TERRAIN_ENABLE_POM != 0 && sceneBuffer.terrain.pomEnabled != 0u)
                        ? 1.0 - smoothstep(POM_FADE_START, POM_FADE_END, dist)
                        : 0.0;

    // Splatmap terrain shading. In Azgaar-cell debug mode, keep the normal
    // lighting path but use a solid ID-derived albedo and skip terrain texture sampling.
    SplatResult splatResult;
    if (azgaarCellDebug) {
        splatResult.albedo       = azgaarCellIdColor(inAzgaarCellId);
        splatResult.roughness    = 0.85;
        splatResult.normal       = vec3(0.0, 0.0, 1.0);
        splatResult.ao           = 1.0;
        splatResult.displacement = 0.0;
        splatResult.paintWeight  = 0.0;
    } else {
        splatResult =
            sampleSplatTerrain(inUV, inWorldPos, material, geometryNormal, viewDirTS, pomFade);
    }
    vec3 baseColor       = splatResult.albedo;
    float roughness      = clamp(splatResult.roughness, 0.04, 1.0);
    const float metallic = 0.0;

    // Normal mapping
    vec3 N = safeNormalize(terrainTBN * splatResult.normal, geometryNormal);
    if (!gl_FrontFacing) N = -N;

    // View
    vec3 V      = normalize(sceneBuffer.cameras[0].position.xyz - inWorldPos);
    float NdotV = max(dot(N, V), 0.001);

    const vec3 F0 = vec3(0.04);

    // Directional light
    DirectionalLight dirLight = sceneBuffer.directionalLight;
    vec3 lightDir             = normalize(dirLight.direction.xyz);
    vec3 lightColor           = dirLight.color.rgb * dirLight.direction.w;
    vec3 L                    = -lightDir;
    vec3 H                    = normalize(V + L);
    float NdotL               = max(dot(N, L), 0.0);
    float HdotV               = max(dot(H, V), 0.0);

    float D = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    vec3 F  = fresnelSchlick(HdotV, F0);

    vec3 specular = (D * G * F) / (4.0 * NdotV * NdotL + 0.0001);

// Shadows
#if TERRAIN_ENABLE_SHADOWS
    vec4 shadowFull     = sampleShadowFull(inWorldPos, N);
    float contactShadow = sampleContactShadow();
    vec3 shadow         = shadowFull.rgb * contactShadow;
    // Cascade-only shadow for ambient/IBL darkening.
    // Contact shadows are screen-space self-occlusion and should not
    // suppress IBL (they already soften direct light via `shadow`).
    float cascadeShadow = shadowFull.a;
#else
    vec3 shadow         = vec3(1.0);
    float cascadeShadow = 1.0;
#endif

    // Micro-shadow from displacement heightfield
    float warpedNdotL = pow(NdotL, 2.5);
    float aperture    = splatResult.ao * splatResult.ao;
    float aoShadow    = clamp(warpedNdotL + aperture - 1.0, 0.0, 1.0);
    aoShadow *= aoShadow;
    float heightShadow = clamp(splatResult.displacement * 0.25 + warpedNdotL, 0.0, 1.0);
    heightShadow *= heightShadow;
    float microSh = min(aoShadow, heightShadow);

    vec3 kD = vec3(1.0) - F;
    vec3 Lo = (kD * baseColor / PI + specular) * lightColor * NdotL * shadow * microSh;

// IBL ambient
#if TERRAIN_ENABLE_IBL
    vec3 ambientDiffuse  = vec3(0.8) * baseColor;
    vec3 ambientSpecular = vec3(0.0);
    if (sceneBuffer.ibl.enabled != 0u) {
        vec3 R                 = reflect(-V, N);
        float iblIntensity     = sceneBuffer.ibl.intensity;
        float iblSpecIntensity = sceneBuffer.ibl.specularIntensity;
        float maxLod           = sceneBuffer.ibl.prefilterMapMaxLod;
        float specLod          = sqrt(roughness) * maxLod;

        if (sceneBuffer.ibl.prefilterMapIndex != 0u && sceneBuffer.ibl.brdfLutIndex != 0u) {
            vec3 irradiance;
            if (sceneBuffer.ibl.hasSH != 0u) {
                vec3 rN        = mat3(sceneBuffer.ibl.envRotation) * N;
                const float A0 = PI, A1 = 2.0 * PI / 3.0;
                const float Y00 = 0.282095, Y1x = 0.488603;
                irradiance = sceneBuffer.ibl.shL0_M0.rgb * A0 * Y00;
                irradiance += sceneBuffer.ibl.shL1_Mp1.rgb * A1 * Y1x * rN.x;
                irradiance += sceneBuffer.ibl.shL1_Mn1.rgb * A1 * Y1x * rN.y;
                irradiance += sceneBuffer.ibl.shL1_M0.rgb * A1 * Y1x * rN.z;
                irradiance = max(irradiance, vec3(0.0));
            } else {
                irradiance = vec3(0.5);
            }

            vec3 prefilteredColor =
                textureLod(
                    samplerCube(cubeTextures[nonuniformEXT(sceneBuffer.ibl.prefilterMapIndex)],
                                samplers[SAMPLER_LINEAR]),
                    normalize(mat3(sceneBuffer.ibl.envRotation) * R),
                    clamp(specLod, 0.0, maxLod))
                    .rgb;

            const float BLEND_START = 0.7;
            const float BLEND_END   = 0.9;
            float specBlend =
                clamp((roughness - BLEND_START) / (BLEND_END - BLEND_START), 0.0, 1.0);
            specBlend *= specBlend;
            vec3 specColor = mix(prefilteredColor, irradiance / PI, specBlend);

            vec2 brdf = texture(sampler2D(textures[nonuniformEXT(sceneBuffer.ibl.brdfLutIndex)],
                                          samplers[SAMPLER_CLAMP_LINEAR]),
                                vec2(NdotV, roughness))
                            .rg;
            vec3 specFactor = F0 * brdf.x + brdf.y;
            ambientDiffuse  = (vec3(1.0) - specFactor) * irradiance * baseColor / PI * iblIntensity;
            ambientSpecular = specColor * specFactor * iblIntensity * iblSpecIntensity;
        }
    }
#else
    vec3 ambientDiffuse  = vec3(0.8) * baseColor;
    vec3 ambientSpecular = vec3(0.0);
#endif

    // Screen AO
    float screenAo = 1.0;
    if (sceneBuffer.shadow.aoImageIndex != 0u) {
        vec2 screenUV = gl_FragCoord.xy / sceneBuffer.cameras[0].viewport;
        screenAo      = texture(sampler2D(textures[nonuniformEXT(sceneBuffer.shadow.aoImageIndex)],
                                          samplers[SAMPLER_CLAMP_LINEAR]),
                                screenUV)
                            .r;
    }

    // Attenuate ambient in shadow
    float shadowAmbientFade = mix(1.0 - SHADOW_DARKNESS, 1.0, mix(1.0, cascadeShadow, NdotL));
    float ao                = splatResult.ao;
    vec3 color = (ambientDiffuse + ambientSpecular) * screenAo * shadowAmbientFade * ao + Lo;

    // Light probe GI indirect diffuse
    vec3 probeIrradiance = sampleLightProbeGi(inWorldPos, N);
    color += probeIrradiance * baseColor / PI;

    // Terrain ambient bounce
    {
        float terrainNdotL = max(dot(N, L), 0.0);
        float bounceFactor = 1.0 - terrainNdotL;
        bounceFactor *= bounceFactor;
        color += baseColor * lightColor * 0.15 * bounceFactor;
    }

// Forward+ point/spot lights
#if TERRAIN_ENABLE_FORWARD_PLUS
    vec3 T_aniso = vec3(0.0);
    vec3 B_aniso = vec3(0.0);
    color += evaluateForwardPlusLights(inWorldPos,
                                       N,
                                       V,
                                       NdotV,
                                       F0,
                                       roughness,
                                       metallic,
                                       baseColor,
                                       T_aniso,
                                       B_aniso,
                                       0.0);
#endif

    if (azgaarDebugStrength > 0.001) {
        vec3 azgaarDebugColor = azgaarOverlayColor * max(max(color.r, color.g), color.b);
        azgaarDebugColor      = max(azgaarDebugColor, azgaarOverlayColor * 0.45);
        color                 = mix(color, azgaarDebugColor, 0.35 * azgaarDebugStrength);
    }

    if (any(isnan(color)) || any(isinf(color))) color = vec3(0.0);
    color = clamp(color, vec3(0.0), vec3(65504.0));

    // Wireframe debug overlay: red edges
    if (wireFrame != 0u) {
        color = vec3(1.0, 0.0, 0.0);
    }

    outColor    = vec4(color, 1.0);
    outNormal   = OctEncode(normalize(N));
    outMaterial = vec4(roughness, 0.0, splatResult.displacement, 1.0);
}
