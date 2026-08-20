#include "ecs/system/System.h"  // IWYU pragma: keep
#pragma once


namespace game {
class LatencyCircleGui : public engine::System {
public:
    LatencyCircleGui();
    void added() override;
    void removed() override;
    void update() override;
};

extern LatencyCircleGui latencyCircleGui;

}  // namespace game
