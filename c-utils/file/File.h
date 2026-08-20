#pragma once

#include "string/String.h"

namespace utils {
char* R_relativePath(const char* path, char* in);
inline char* relative(const char* path) { char buf[1024] = {}; return R_relativePath(path, buf); }

bool fileExists(const char* path);

void fileWrite(const char* path, const char* data);
void fileWriteBinary(const char* path, void* data, u32 size);
void fileAppend(const char* path, const char* data);

String fileRead(const char* path);
void fileRead2(String* string, const char* path);
char* fileRead3(const char* path, u32* fileSize);
}  // namespace utils
