#include "ecs/system/lua/LuaSystem.h"
#include "Engine.h"
#include "ecs/system/System.h"
#include "ecs/system/sound/SoundSystem.h"
#include "lua/git/lauxlib.h"
#include "lua/git/lua.h"
#include "lua/git/lualib.h"

static i32 luacHoverSound(void* luaState);
static i32 luacClickSound(void* luaState);
static i32 luacExit(void* luaState);

static void added(void);

static lua_State* luaState;
struct System luaSystem = {
    .name  = "lua",
    .added = added,
};

void added(void) {
    luaState = luaL_newstate();
    luaL_openlibs(luaState);

    luaRegisterFunction("luacHoverSound", luacHoverSound);
    luaRegisterFunction("luacClickSound", luacClickSound);
    luaRegisterFunction("luacExit", luacExit);
}

void luaDestroy(void) {
    lua_close(luaState);
}

i32 luacHoverSound(void* luaState) {
    soundPlayHover();
    return 0;
}

i32 luacClickSound(void* luaState) {
    soundPlayClick();
    return 0;
}

void luaRegisterFunction(const char* name, LuaFunction luaFunction) {
    lua_register(luaState, name, reinterpret_cast<lua_CFunction>(luaFunction));
}

void* luaGetState(void) {
    return luaState;
}

i32 luacExit(void* luaState) {
    engineStop();
    return 0;
}

void luaLoadFile(const char* path) {
    struct String buffer = dataManagerRead(path);
    if (luaL_loadstring(luaState, buffer.data) || lua_pcall(luaState, 0, 0, 0)) {
        error("failed to load lua: %s", path);
    }
    // stringDestroy(&buffer);
}

void luaCallFunction(const char* functionName) {
    lua_getglobal(luaState, functionName);
    if (lua_pcall(luaState, 0, 0, 0)) {
        error("failed to call: %s", functionName);
        error("%s", lua_tostring(luaState, -1));
    }
}
