#include "ecs/system/System.h"
#include "ecs/system/lua/LuaSystem.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "rmlui/wrapper/src/crmlui.h"
static void added(void);
static void removed(void);

struct System creditsGui = {
    .name    = "creditsGui",
    .added   = added,
    .removed = removed,
};

static void* document;

static int creditsClose(void* _);

void added(void) {
    luaRegisterFunction("creditsClose", creditsClose);
    document = rmlNewDocument("gui/credits/credits.html");
    rmlLoadDocument(document);
    rmlShowDocument(document);
}

void removed(void) {
    rmlUnloadDocument(document);
    document = NULL;
}

int creditsClose(void* _) {
    guiManagerRemoveGuiNextFrame(&creditsGui);
    return 0;
}
