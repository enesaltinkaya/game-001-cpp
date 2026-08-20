#pragma once

namespace utils {
void settingsInit(void);
void settingsDestroy(void);
void settingsWrite(void);

double settingsGetDouble(const char* key);
bool settingsGetBool(const char* key);
int settingsGetInt(const char* key);

void settingsSetInt(const char* key, int value);
void settingsSetDouble(const char* key, double value);
void settingsSetBool(const char* key, bool value);
}  // namespace utils
