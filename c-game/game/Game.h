#pragma once

#include "gameState/GameState.h"
#include "ecs/system/System.h"  // IWYU pragma: keep

namespace game {
class GameSystem : public engine::System {
public:
    GameSystem();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern GameSystem gameSystem;
}  // namespace game
