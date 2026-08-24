#pragma once
#include "ecs/system/System.h"

namespace engine {
class PassStatsGui : public System {
public:
    PassStatsGui();
    void added() override;
    void removed() override;
    void update() override;
};

extern PassStatsGui passStatsGui;

void passStatsGuiToggle(void);
}  // namespace engine
