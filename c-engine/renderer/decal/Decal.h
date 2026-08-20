#pragma once

#include "Utils.h"

enum DecalProjectionAxis {
    DECAL_PROJECT_Y_DOWN = 0,
    DECAL_PROJECT_Z_FORWARD = 1,
    DECAL_PROJECT_CUSTOM = 2,
};

enum DecalFlags {
    DECAL_FLAG_NONE        = 0,
    DECAL_FLAG_GROUND_ONLY = 1u << 0,
    DECAL_FLAG_EMISSIVE    = 1u << 1,
    DECAL_FLAG_DEPTH_FADE  = 1u << 2,
    DECAL_FLAG_WORLD_UV    = 1u << 3,
    // Route decals whose rectangles overlap at junctions (e.g. Azgaar
    // roads/trails). Routed to a dedicated "union" layer so overlapping rects
    // take the max coverage instead of accumulating alpha (which would darken).
    DECAL_FLAG_ROAD        = 1u << 4,
};

struct DecalInstance {
    vec3 position;
    vec3 halfExtents;
    versor rotation;
    vec4 color;
    u32 textureId;
    u32 flags;
    float opacity;
    float normalThreshold;
    float edgeFeather;
    float uvScale[2];
    float time;
};

#define DECAL_INVALID_HANDLE UINT32_MAX
#define DECAL_PROCEDURAL_CIRCLE_TEXTURE UINT32_MAX

u32 decalAdd(const DecalInstance* decal);
void decalRemove(u32 handle);
void decalClearPersistent(void);
void decalClearTransient(void);
void decalSubmitTransient(const DecalInstance* decal);

const DecalInstance* decalGetPersistent(size_t* count);
const DecalInstance* decalGetTransient(size_t* count);
