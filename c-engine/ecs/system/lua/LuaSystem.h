#pragma once
#include "ecs/system/System.h"

namespace engine {
class LuaSystem : public System {
public:
    LuaSystem();
    void added() override;
};

extern LuaSystem luaSystem;

using LuaFunction = int (*)(void*);

void* luaGetState(void);
void luaRegisterFunction(const char* name, LuaFunction luaFunction);
void luaLoadFile(const char* path);
void luaCallFunction(const char* functionName);
void luaDestroy(void);
}  // namespace engine
