#pragma once

extern struct System luaSystem;

typedef int (*LuaFunction)(void* luaState);

void* luaGetState(void);
void luaRegisterFunction(const char* name, LuaFunction luaFunction);
void luaLoadFile(const char* path);
void luaCallFunction(const char* functionName);
void luaDestroy(void);
