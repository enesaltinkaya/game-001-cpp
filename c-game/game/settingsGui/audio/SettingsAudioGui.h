#include "ecs/system/System.h"  // IWYU pragma: keep
#pragma once

namespace game {
class SettingsAudioGui : public engine::System {
public:
    SettingsAudioGui();
    void added() override;
    void removed() override;
};

extern SettingsAudioGui settingsAudioGui;
}  // namespace game
