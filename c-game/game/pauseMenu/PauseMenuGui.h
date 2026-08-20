#include "ecs/system/System.h"  // IWYU pragma: keep
#pragma once

namespace game {
class PauseMenuGui : public engine::System {
public:
    PauseMenuGui();
    void added() override;
    void removed() override;
};

extern PauseMenuGui pauseMenuGui;

char pauseMenuGuiIsShowing(void);
}  // namespace game
