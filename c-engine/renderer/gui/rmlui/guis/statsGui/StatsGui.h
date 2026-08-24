#pragma once
#include "ecs/system/System.h"

namespace engine {
class StatsGui : public System {
public:
    StatsGui();
    void added() override;
    void removed() override;
    void update() override;
};

extern StatsGui statsGui;

void statsGuiToggle(void);
}  // namespace engine
