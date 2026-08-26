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

    /* GUI systems that own an RML document set this (and clear it in
     * removed()) so the gui manager can query the document — e.g.
     * mouse-over hit-testing for menu GUIs. */
    void* rmlDocument = nullptr;
    /* Menu-style GUIs (debug gui, settings) set this: while the mouse
     * cursor is inside the GUI's panel region, the player camera's
     * click-hold-rotate is disabled — the click belongs to the menu
     * (guiManagerIsMouseOverMenuGui). */
    char menuGui = 0;
};

void systemPreUpdate(System* system);
void systemUpdate(System* system);
void systemPostUpdate(System* system);
}  // namespace engine