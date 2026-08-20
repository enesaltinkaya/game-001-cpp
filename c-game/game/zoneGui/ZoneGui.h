#include "ecs/system/System.h"  // IWYU pragma: keep
#pragma once

// Gameplay HUD overlay that announces the current Azgaar region (province /
// state) with a World-of-Warcraft-style fade in / hold / fade out each time the
// player crosses into a new zone.
namespace game {
class ZoneGui : public engine::System {
public:
    ZoneGui();
    void added() override;
    void removed() override;
    void update() override;
};

extern ZoneGui zoneGui;
}  // namespace game
