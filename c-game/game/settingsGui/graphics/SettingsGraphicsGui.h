#include "ecs/system/System.h"  // IWYU pragma: keep
#pragma once

namespace game {
class SettingsGraphicsGui : public engine::System {
public:
    SettingsGraphicsGui();
    void added() override;
    void removed() override;
};

extern SettingsGraphicsGui settingsGraphicsGui;
}  // namespace game
