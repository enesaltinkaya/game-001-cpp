#pragma once

namespace utils {
void sqliteInit(const char* directory);
void sqliteDestroy(void);

void* sqliteGetDb(void);
void sqliteExecute(const char* query);
void* sqliteStatement(const char* query);
void sqliteReset(void* statement);
void sqliteFinalize(void* statement);

void sqliteBindText(void* statement, int index, const char* param);
void sqliteBindInt(void* statement, int index, int param);
void sqliteBindBlob(void* statement, int index, void* data, u64 dataSize);

int sqliteGetInt(void* statement, int index);
const char* sqliteGetString(void* statement, int index);
void* sqliteGetBlob(void* statement, int index);

bool sqliteTableExists(const char* tableName);
bool sqliteStep(void* statement);

int sqliteCount(const char* query);
}  // namespace utils
