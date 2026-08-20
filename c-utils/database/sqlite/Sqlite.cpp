#include "Sqlite.h"
#include <stdio.h>
#include "Utils.h"
#include "file/File.h"
#include "platform/Platform.h"
#include "sqlite/git/build-linux/sqlite3.h"
#include "logger/Logger.h"
#include "string/String.h"

namespace utils {
static sqlite3* db;

static char* errorMessage = nullptr;
#define checkdb(expr) \
    if ((expr) != SQLITE_OK) terminate("SQL db error: %s", sqlite3_errmsg(db));
#define checkQuery(expr) \
    if ((expr) != SQLITE_OK) terminate("SQL query error: %s", errorMessage);

void sqliteInit(const char* directory) {
    info("database: initializing");
    

    char pathBuf[1024];
    char* path = R_relativePath(strtmp("data/%s/db.db", directory), pathBuf);
    createDirectory(path);

    checkdb(sqlite3_open(path, &db));

    sqliteExecute("PRAGMA journal_mode = WAL;");
    sqliteExecute("PRAGMA synchronous = NORMAL;");
    sqliteExecute("PRAGMA cache_size = -134217;");
    sqliteExecute("PRAGMA temp_store = MEMORY;");
    sqliteExecute("PRAGMA mmap_size = 134217728;");

    debug("database: db engine sqlite %s", SQLITE_VERSION);
}

void sqliteDestroy(void) {
    sqlite3_close(db);
}

void sqliteExecute(const char* query) {
    checkQuery(sqlite3_exec(db, query, 0, 0, &errorMessage));
}

void* sqliteStatement(const char* query) {
    sqlite3_stmt* stmt;
    checkdb(sqlite3_prepare_v2(db, query, -1, &stmt, 0));
    return stmt;
}

void sqliteReset(void* statement) {
    checkdb(sqlite3_reset(static_cast<sqlite3_stmt*>(statement)));
}

void sqliteFinalize(void* statement) {
    checkdb(sqlite3_finalize(static_cast<sqlite3_stmt*>(statement)));
}

void sqliteBindText(void* statement, int index, const char* param) {
    checkdb(sqlite3_bind_text(static_cast<sqlite3_stmt*>(statement), index, param, -1, SQLITE_STATIC));
}

void sqliteBindInt(void* statement, int index, int param) {
    checkdb(sqlite3_bind_int(static_cast<sqlite3_stmt*>(statement), index, param));
}

void sqliteBindBlob(void* statement, int index, void* data, u64 dataSize) {
    checkdb(sqlite3_bind_blob(static_cast<sqlite3_stmt*>(statement), index, data, static_cast<int>(dataSize), SQLITE_STATIC));
}

bool sqliteStep(void* statement) {
    return sqlite3_step(static_cast<sqlite3_stmt*>(statement)) == SQLITE_ROW;
}

int sqliteGetInt(void* statement, int index) {
    return sqlite3_column_int(static_cast<sqlite3_stmt*>(statement), index);
}

const char* sqliteGetString(void* statement, int index) {
    return reinterpret_cast<const char*>(sqlite3_column_text(static_cast<sqlite3_stmt*>(statement), index));
}

void* sqliteGetBlob(void* statement, int index) {
    return const_cast<void*>(sqlite3_column_blob(static_cast<sqlite3_stmt*>(statement), index));
}

bool sqliteTableExists(const char* tableName) {
    sqlite3_stmt* stmt;
    char sql[256];
    bool exists = false;
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s);", tableName);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            exists = true;
        }
        sqlite3_finalize(stmt);
    }
    return exists;
}

int sqliteCount(const char* query) {
    void* statement = sqliteStatement(query);
    sqliteStep(statement);
    int count = sqliteGetInt(statement, 0);
    sqliteFinalize(statement);
    return count;
}

void* sqliteGetDb(void) {
    return db;
}
}  // namespace utils
