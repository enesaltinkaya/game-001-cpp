#pragma once
#include "ecs/system/System.h"


namespace engine {
typedef struct Scene Scene;

class LightSystem : public System {
public:
    LightSystem();
    void added() override;
    void update() override;
    void postUpdate() override;
};

extern LightSystem lightSystem;

/* Legacy compatibility stub. Lighting is rebuilt each frame from visible scenes. */
void lightMarkDirty(Scene* scene, u32 entity);
}  // namespace engine
