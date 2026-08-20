#pragma once

#include "ecs/system/scene/SceneSystem.h"

// Initialize and attach to the active camera entity
void topDownCameraInit(void);

// Set the entity the top-down camera should follow
void topDownCameraSetTarget(Scene* scene, u32 entityId);

// Set the orbit distance from target (meters)
void topDownCameraSetDistance(float distance);

// Get current yaw (radians)
float topDownCameraGetYaw(void);

// Update camera yaw from mouse input delta
void topDownCameraHandleInput(float dx);

// Apply scroll-wheel zoom delta directly (handles clamping internally)
void topDownCameraScrollZoom(float scrollY);

// Get current distance (meters)
float topDownCameraGetDistance(void);

// Call in preUpdate phase
void topDownCameraPreUpdate(void);

// Call in update phase (position camera)
void topDownCameraUpdate(void);

// Unproject screen coordinates to world-space ground ray origin + direction
void topDownCameraUnproject(float screenX, float screenY, vec3 outOrigin, vec3 outDir);
