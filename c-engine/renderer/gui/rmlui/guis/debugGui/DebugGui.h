#pragma once
#include "ecs/system/System.h"

namespace engine {
class DebugGui : public System {
public:
    DebugGui();
    void added() override;
    void removed() override;
    void update() override;
};

extern DebugGui debugGui;

void debugGuiToggle(void);
}  // namespace engine
