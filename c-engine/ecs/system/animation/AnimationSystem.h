#pragma once

#include "ecs/system/System.h"

/*
 * Animation System
 * 
 * Handles skeleton animation playback, blending, and events.
 * Updates joint transforms for all animated entities.
 */

namespace engine {
class AnimationSystem : public System {
public:
    AnimationSystem();
    void added() override;
    void removed() override;
    void update() override;
};

extern AnimationSystem animationSystem;

/*
 * Initialize the animation system
 * Called automatically during ECS initialization
 */
void animationSystemInit(void);

/*
 * Destroy the animation system and free all resources
 */
void animationSystemDestroy(void);
}  // namespace engine
