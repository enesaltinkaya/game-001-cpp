#include "ecs/system/System.h"  // IWYU pragma: keep
#pragma once


namespace game {
class MainMenu : public engine::System {
public:
    MainMenu();
    void added() override;
    void removed() override;
    void preUpdate() override;
};

extern MainMenu mainMenu;
}  // namespace game
