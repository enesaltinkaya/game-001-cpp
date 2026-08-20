#pragma once

#include "ecs/system/scene/SceneSystem.h"

// Initialize the third-person orbit camera and attach to the active camera entity
namespace game {
void thirdPersonCameraInit(void);

// Set the entity the camera should follow
void thirdPersonCameraSetTarget(engine::Scene* scene, u32 entityId);

// Update camera yaw/pitch from mouse input deltas
void thirdPersonCameraHandleInput(float dx, float dy);

// Set the orbit distance from target (meters)
void thirdPersonCameraSetDistance(float distance);

// Get current orbit distance (meters)
float thirdPersonCameraGetDistance(void);

// Get current camera yaw (radians)
float thirdPersonCameraGetYaw(void);

// Get current camera pitch (radians)
float thirdPersonCameraGetPitch(void);

// Set camera yaw/pitch directly (radians)
void thirdPersonCameraSetAngles(float yaw, float pitch);

// Update camera position (call once per frame in update phase)
void thirdPersonCameraUpdate(void);

// Set the shared movement state (for sky-look logic)
void thirdPersonCameraSetMoving(bool moving);
void thirdPersonCameraSetAnyDrag(bool anyDrag);
void thirdPersonCameraSetMouseDy(float dy);

// Get the shared movement state (set during update)
bool thirdPersonCameraIsMoving(void);
bool thirdPersonCameraIsAnyDrag(void);
}  // namespace game
