#include "ecs/system/System.h"  // IWYU pragma: keep
#pragma once


namespace game {
class CameraGui : public engine::System {
public:
    CameraGui();
    void added() override;
    void removed() override;
    void update() override;
};

extern CameraGui cameraGui;

}  // namespace game
