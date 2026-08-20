#pragma once
#include "ecs/system/System.h"

namespace engine {
class RmluiShowFpsGui : public System {
public:
    RmluiShowFpsGui();
    void added() override;
    void removed() override;
    void update() override;
};

extern RmluiShowFpsGui rmluiShowFpsGui;
}  // namespace engine
