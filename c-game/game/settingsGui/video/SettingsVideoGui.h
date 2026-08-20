#include "ecs/system/System.h"  // IWYU pragma: keep
#pragma once

namespace game {
class SettingsVideoGui : public engine::System {
public:
    SettingsVideoGui();
    void added() override;
    void removed() override;
};

extern SettingsVideoGui settingsVideoGui;

}  // namespace game
