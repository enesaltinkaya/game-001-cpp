#pragma once

extern struct System luaSystem;

using LuaFunction = int (*)(void*);

void* luaGetState(void);
void luaRegisterFunction(const char* name, LuaFunction luaFunction);
void luaLoadFile(const char* path);
void luaCallFunction(const char* functionName);
void luaDestroy(void);
