#pragma once

#include "Defines.h"                                    // IWYU pragma: keep
#include <cstddef>                                         // IWYU pragma: keep
#include <cstdlib>                                         // IWYU pragma: keep
#include <cstring>                                         // IWYU pragma: keep
#include <string>                                          // IWYU pragma: keep
#include <unordered_map>                                   // IWYU pragma: keep
#include <vector>                                          // IWYU pragma: keep
#include "container/SparseSet/SparseSet.h"              // IWYU pragma: keep
#include "container/SparseSetSimple/SparseSetSimple.h"  // IWYU pragma: keep
#include "database/sqlite/Sqlite.h"                     // IWYU pragma: keep
#include "datamanager/DataManager.h"                    // IWYU pragma: keep
#include "events/Events.h"                              // IWYU pragma: keep
#include "file/File.h"                                  // IWYU pragma: keep
#include "futuretask/FutureTask.h"                      // IWYU pragma: keep
#include "image/Image.h"                                // IWYU pragma: keep
#include "json/Json.h"                                  // IWYU pragma: keep
#include "logger/Logger.h"                              // IWYU pragma: keep
#include "platform/Platform.h"                          // IWYU pragma: keep
#include "settings/Settings.h"                          // IWYU pragma: keep
#include "string/String.h"                              // IWYU pragma: keep
#include "thread/Thread.h"                              // IWYU pragma: keep
#include "timer/Timer.h"                                // IWYU pragma: keep

namespace utils {
void utilsInit(void);
void utilsDestroy(void);
void terminate(const char* format, ...);
void gotoSleepMS(long int millis);
void gotoSleepNS(long int nanos);

double elapsedBegin(void);
double elapsedEnd(double start);
void randomChars(char* dest, long int length);
u32 randomU32(void);
float randomFloat(void);
double nanos(void);
double millies(void);
bool isDebug(void);
u32 colorHexToUInt(const char* hex);
void* customMemmem(const void* haystack, int haystack_len, const void* needle, int needle_len);
}  // namespace utils
