#include "ecs/system/System.h"  // IWYU pragma: keep
#pragma once

namespace game {
class CompassGui : public engine::System {
public:
    CompassGui();
    void added() override;
    void removed() override;
    void update() override;
};

extern CompassGui compassGui;
}  // namespace game
