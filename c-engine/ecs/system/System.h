#pragma once

namespace engine {
class System {
public:
    explicit System(const char* systemName) : name(systemName) {}
    virtual ~System() = default;

    virtual void added() {}
    virtual void removed() {}
    virtual void preUpdate() {}
    virtual void update() {}
    virtual void postUpdate() {}

    const char* name;
    i32 priority = 0;
    double cpuElapsedLastFrame = 0.0;
    double cpuElapsed          = 0.0;
    double gpuElapsed          = 0.0;
};

void systemPreUpdate(System* system);
void systemUpdate(System* system);
void systemPostUpdate(System* system);
}  // namespace engine