#include "ecs/system/System.h"  // IWYU pragma: keep
#pragma once

namespace game {
class SettingsGui : public engine::System {
public:
    SettingsGui();
    void added() override;
    void removed() override;
};

extern SettingsGui settingsGui;

void settingsGuiHide(void);
void settingsGuiShow(void);
char settingsGuiIsShowing(void);
}  // namespace game
