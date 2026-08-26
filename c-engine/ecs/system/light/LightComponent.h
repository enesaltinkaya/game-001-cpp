#pragma once

#include "ecs/system/scene/SceneSystem.h"

namespace engine {
enum LightType {
    LIGHT_DIRECTIONAL,
    LIGHT_POINT,
    LIGHT_SPOT,
};

/* 64 bytes, fully vec4-aligned — matches GpuLight in globalset.shader */
typedef struct GpuLight {
    vec4 positionAndRange;  /* xyz: world position, w: range (0=infinite) */
    vec4 directionAndType;  /* xyz: direction, w: LightType as float */
    vec4 colorAndIntensity; /* rgb: color, w: intensity */
    vec4 spotAngles;        /* x: cos(innerCone), y: cos(outerCone), zw: pad */
} GpuLight;

#define MAX_GPU_LIGHTS 1024

typedef struct LightUbo {
    ivec4 counts;                    /* x: directional, y: point, z: spot, w: total */
    GpuLight lights[MAX_GPU_LIGHTS]; /* sorted: directionals, then point, then spot */
} LightUbo;

typedef struct DirectionalLightUbo {
    vec4 direction; /* xyz: direction, w: intensity */
    vec4 color;     /* rgb: color, w: unused */
    vec4 ambient;   /* rgb: ambient color, w: unused */
    vec4 padding;   /* std430 alignment */
} DirectionalLightUbo;

#define SHADOW_CASCADE_COUNT 4

typedef struct ShadowUbo {
    mat4 shadowViewProjection[SHADOW_CASCADE_COUNT];
    vec4 cascadeSplits; /* view-space far plane of each cascade */
    vec4 shadowParams;  /* x: bias, y: normalBias, z: mapSize, w: 1/mapSize */
    u32 shadowMapIndex[SHADOW_CASCADE_COUNT];
    u32 cascadeCount;
    float lightSize;             /* unused (PCSS removed) */
    u32 temporalActive;          /* 1 when FSR or any temporal upscaler is active */
    u32 contactShadowImageIndex; /* bindless sampled index of contact shadow texture (0 = off) */
    u32 pad2;
    u32 pad3;
    u32 pad4;
    u32 pad5;
} ShadowUbo;

REGISTER_COMPONENT(Light);

typedef struct Light {
    enum LightType lightType;
    vec3 color;
    float intensity;
    float range;          /* point/spot: effective range; 0 = infinite */
    float innerConeAngle; /* spot only, radians */
    float outerConeAngle; /* spot only, radians */
    bool castsShadows;
} Light;
}  // namespace engine
