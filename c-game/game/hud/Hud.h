#include "ecs/system/System.h"  // IWYU pragma: keep
#pragma once

namespace game {
class Hud : public engine::System {
public:
    Hud();
    void added() override;
    void removed() override;
    void update() override;
};

extern Hud hud;

// Spawn a floating damage number at world position.
// Value > 0: damage dealt (white). Value < 0: damage received (red).
void hudDamageNumber(float x, float y, float z, float value);
}  // namespace game
