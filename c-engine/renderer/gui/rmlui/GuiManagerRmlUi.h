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

extern std::vector<System*> rmluiGuis;

///////////////////////////////////
void guiManagerUpdateScale(void);
void guiManagerUpdateCursors(void);
void guiManagerToggleShowFps(void);
void guiManagerReleaseTexture(const char* name);
void guiManagerReleaseAllTextures(void);
}  // namespace engine
