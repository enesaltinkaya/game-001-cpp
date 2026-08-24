#pragma once

#include "ecs/system/System.h"
namespace engine {
class CameraSystem : public System {
public:
    CameraSystem();
    void added() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern CameraSystem cameraSystem;

struct Camera;
struct Entity* cameraGetEntity(void);
}  // namespace engine
