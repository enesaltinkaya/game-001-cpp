#include "ecs/system/System.h"  // IWYU pragma: keep
#pragma once

namespace game {
class PlayerActionsGui : public engine::System {
public:
    PlayerActionsGui();
    void added() override;
    void removed() override;
    void update() override;
};

extern PlayerActionsGui playerActionsGui;
}  // namespace game
