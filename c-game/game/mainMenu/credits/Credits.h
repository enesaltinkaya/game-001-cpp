#include "ecs/system/System.h"  // IWYU pragma: keep
#pragma once

namespace game {
class CreditsGui : public engine::System {
public:
    CreditsGui();
    void added() override;
    void removed() override;
};

extern CreditsGui creditsGui;
}  // namespace game
