#include "ecs/system/System.h"  // IWYU pragma: keep
#pragma once

namespace game {
class MainMenuGui : public engine::System {
public:
    MainMenuGui();
    void added() override;
    void removed() override;
};

extern MainMenuGui mainMenuGui;
int settingsOpen(void* _);
void playGame(void);
}  // namespace game
