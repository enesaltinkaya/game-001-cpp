#include "Credits.h"
#include "ecs/system/System.h"
#include "ecs/system/lua/LuaSystem.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "rmlui/wrapper/src/crmlui.h"
namespace game {

CreditsGui creditsGui;

CreditsGui::CreditsGui() : engine::System("creditsGui") {}

static void* document;

static int creditsClose(void* _);

void CreditsGui::added() {
    engine::luaRegisterFunction("creditsClose", creditsClose);
    document = rmlNewDocument("gui/credits/credits.html");
    rmlLoadDocument(document);
    rmlShowDocument(document);
}

void CreditsGui::removed() {
    rmlUnloadDocument(document);
    document = nullptr;
}

int creditsClose(void* _) {
    engine::guiManagerRemoveGuiNextFrame(&creditsGui);
    return 0;
}
}  // namespace game
