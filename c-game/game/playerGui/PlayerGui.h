#include "ecs/system/System.h"  // IWYU pragma: keep
#pragma once

namespace game {
class PlayerGui : public engine::System {
public:
    PlayerGui();
    void added() override;
    void removed() override;
    void update() override;
};

extern PlayerGui playerGui;
}  // namespace game
