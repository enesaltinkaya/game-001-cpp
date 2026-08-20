#include "ecs/system/System.h"
#include "ecs/system/lua/LuaSystem.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "rmlui/wrapper/src/crmlui.h"
static void added(void);
static void removed(void);

System creditsGui = {
    .name                = "creditsGui",
    .added               = added,
    .removed             = removed,
    .preUpdate           = nullptr,
    .update              = nullptr,
    .postUpdate          = nullptr,
    .cpuElapsedLastFrame = 0.0,
    .cpuElapsed          = 0.0,
    .gpuElapsed          = 0.0,
    .priority            = 0,
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
    document = nullptr;
}

int creditsClose(void* _) {
    guiManagerRemoveGuiNextFrame(&creditsGui);
    return 0;
}
