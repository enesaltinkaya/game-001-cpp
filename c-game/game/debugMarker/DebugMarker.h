#pragma once

#include "ecs/system/System.h"

namespace game {

// TEMPORARY debug helper: spawns an unlit-white 50 cm cube at a hardcoded
// world position so a spot can be eyeballed in screenshots / gameplay.
// Remove once the debugging session is done.
class DebugMarkerSystem : public engine::System {
public:
    DebugMarkerSystem();
    void added() override;
    void removed() override;
};

extern DebugMarkerSystem debugMarkerSystem;
}  // namespace game
