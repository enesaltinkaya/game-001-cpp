#pragma once

#include "ecs/system/System.h"

namespace game {
class AzgaarStreamingSystem : public engine::System {
public:
    AzgaarStreamingSystem();
    void added() override;
    void removed() override;
    void update() override;
};

extern AzgaarStreamingSystem azgaarStreamingSystem;
}  // namespace game
