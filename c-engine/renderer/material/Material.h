#pragma once

#include "cglm/git/include/cglm/types.h"

// Base texture flags (0-4)
#define MAT_HAS_TEXTURE_COLOR 0
#define MAT_HAS_TEXTURE_ROUGHNESS_METALLIC 1
#define MAT_HAS_TEXTURE_NORMAL 2
#define MAT_HAS_TEXTURE_EMISSIVE 3
#define MAT_HAS_TEXTURE_OCCLUSION 4

#define MAT_ALPHA_OPAQUE 5
#define MAT_ALPHA_MASK 6
#define MAT_ALPHA_BLEND 7
#define MAT_IS_DOUBLE_SIDED 8

#define MAT_HAS_CLEARCOAT 9
#define MAT_HAS_TRANSMISSION 10
#define MAT_HAS_SHEEN 11
#define MAT_HAS_SPECULAR 12
#define MAT_HAS_IOR 13
#define MAT_HAS_VOLUME 14
#define MAT_HAS_EMISSIVE_FACTOR 15
#define MAT_HAS_ANISOTROPY 16
#define MAT_HAS_SPLATMAP 17

#define MAX_SPLAT_GROUPS 4
#define MAX_SPLAT_CHANNELS 4
#define MAX_SPLAT_DETAIL_TEXTURES (MAX_SPLAT_GROUPS * MAX_SPLAT_CHANNELS)  // 16

// UDIM 10x10 grid: weight textures are stored globally in TerrainData
// (SceneBuffer), not per-material.  The material only carries detail
// texture indices (albedo + normal per channel per group).
#define SPLAT_UDIM_COUNT_X 10
#define SPLAT_UDIM_COUNT_Y 10
#define SPLAT_UDIM_TILES   (SPLAT_UDIM_COUNT_X * SPLAT_UDIM_COUNT_Y)  // 100

typedef struct Material {
    // Base PBR properties
    vec4 baseColor;  // RGB + Alpha
    vec4 emissive;   // RGB + emissiveStrength
    vec4 rmas;       // Roughness, Metallic, AlphaCutoff, NormalScale

    // Extension properties packed for std430 alignment
    vec4 clearcoatSheenTransmission;  // x: clearcoat, y: clearcoatRoughness, z: sheenRoughness, w: transmission
    vec4 sheenColorAnisotropy;        // xyz: sheenColor, w: anisotropyStrength
    vec4 specularColorIor;            // xyz: specularColor, w: ior
    vec4 specularAnisotropyRotation;  // x: specularFactor, y: anisotropyRotation, zw: pad
    vec4 volumeFactors;               // x: thicknessFactor, y: attenuationDistance, zw: pad
    vec4 volumeColor;                 // xyz: attenuationColor, w: pad

    // Base texture transforms
    vec4 baseColorOffsetScale;  // xy=offset, zw=scale
    vec4 rmaOffsetScale;
    vec4 normalOffsetScale;
    vec4 emissionOffsetScale;
    vec4 occlusionOffsetScale;

    // Texture indices
    u32 colorTexture;
    u32 rmTexture;
    u32 normalTexture;
    u32 emissiveTexture;
    u32 occlusionTexture;

    // Sampler indices
    u32 colorTextureSampler;
    u32 rmTextureSampler;
    u32 normalTextureSampler;
    u32 emissiveTextureSampler;
    u32 occlusionTextureSampler;

    // Splatmap terrain blending — detail textures only.
    // Weight textures (UDIM tiles) are in TerrainData (SceneBuffer).
    // Per channel: albedo.ktx2 (RGB=diffuse, A=roughness)
    //            + normal.ktx2 (RG=normalXY, B=AO, A=displacement)
    u32 splatGroupCount;
    u32 splatPad[3];
    u32 splatAlbedoTextures[MAX_SPLAT_DETAIL_TEXTURES];   // 4 groups x 4 channels = 16
    u32 splatNormalTextures[MAX_SPLAT_DETAIL_TEXTURES];    // normal+AO+displacement packed

    // Meta
    u32 featureMask;
    u32 id;
    u32 refCount;
    u32 pad0;
    u32 pad1;
    u32 pad2;
} Material;
