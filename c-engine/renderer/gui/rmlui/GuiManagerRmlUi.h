#pragma once
#include "ecs/system/System.h"
#include <vector>
#include "guis/debugGui/DebugGui.h"                    // IWYU pragma: keep
#include "guis/passStatsGui/PassStatsGui.h"            // IWYU pragma: keep
#include "guis/showFpsGui/ShowFpsGui.h"               // IWYU pragma: keep
#include "guis/statsGui/StatsGui.h"                    // IWYU pragma: keep

namespace engine {
class GuiManagerRmlUi : public System {
public:
    GuiManagerRmlUi();
    void added() override;
    void removed() override;
    void postUpdate() override;
};

extern GuiManagerRmlUi guiManagerRmlUi;

void guiManagerAddGuiNextFrame(System* gui);
void guiManagerRemoveGuiNextFrame(System* gui);

/* 1 if the mouse cursor is inside the panel region of any active
 * menu-style GUI (gui->menuGui). Game input that would conflict with
 * the menu (the player camera's click-hold-rotate) should be gated on
 * this. Only meaningful while the cursor is visible. */
char guiManagerIsMouseOverMenuGui(void);

extern std::vector<System*> rmluiGuis;

///////////////////////////////////
void guiManagerUpdateScale(void);
void guiManagerUpdateCursors(void);
void guiManagerToggleShowFps(void);
void guiManagerReleaseTexture(const char* name);
void guiManagerReleaseAllTextures(void);
}  // namespace engine
