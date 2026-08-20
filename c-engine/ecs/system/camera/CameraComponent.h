#pragma once
#include "ecs/system/scene/SceneSystem.h"

enum CameraType {
    PERSPECTIVE,
    ORTHOGRAPHIC,
};

typedef struct CameraUbo {
    mat4 view;
    mat4 projection;
    mat4 viewProjection;
    mat4 invViewProjection;
    mat4 invView;
    mat4 invProjection;
    mat4 viewProjectionNoJitter;
    mat4 invViewProjectionNoJitter;
    mat4 prevViewProjection;
    mat4 prevViewProjectionNoJitter;
    vec4 frustumPlanes[6];
    vec4 renderLocation;
    vec4 renderDirection;
    vec2 viewport;
    u32 frameIndex;
    float znear;
    float zfar;
    float exposure;  /* scene exposure multiplier applied before tonemapping */
    float jitterX;   /* sub-pixel jitter in UV space (0 when jitter off) */
    float jitterY;
    float prevJitterX;
    float prevJitterY;
    float _pad0[2];
    float _pad1[4];
    float _pad2[4];
} CameraUbo;

REGISTER_COMPONENT(Camera);

typedef struct Camera {
    struct CameraUbo cameraUbo;
    mat4 prevViewProjection;
    mat4 prevViewProjectionNoJitter;
    float aspectRatio;
    float yfov;
    float zfar;
    float znear;

    float yaw;
    float pitch;
    u32 frameIndex;

    /* Photographic exposure multiplier (applied to HDR before tonemapping).
     * 1.0 = neutral. Lower = darker, higher = brighter.
     * Eevee default is roughly equivalent to 1.0 here after the
     * candela→watt conversion that SceneParser already applies. */
    float exposure;

} Camera;
