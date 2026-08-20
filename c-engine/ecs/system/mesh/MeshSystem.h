#pragma once
#include "ecs/system/System.h"

namespace engine {
class MeshSystem : public System {
public:
    MeshSystem();
    void added() override;
    void postUpdate() override;
};

extern MeshSystem meshSystem;
}  // namespace engine
