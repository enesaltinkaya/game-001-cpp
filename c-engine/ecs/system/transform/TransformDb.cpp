#include "ecs/system/transform/TransformComponent.h"

void transformDbInit(void) {
    if (!sqliteTableExists("transform")) {
        sqliteExecute(
            "CREATE TABLE IF NOT EXISTS transform ("
            "name TEXT PRIMARY KEY, "
            "transform BLOB);");
    }
}

void transformDbSave(const char* name, Transform* transformIn) {
    void* stmt = sqliteStatement("REPLACE INTO transform (name, transform) VALUES (?, ?);");
    sqliteBindText(stmt, 1, name);
    sqliteBindBlob(stmt, 2, transformIn, sizeof(Transform));
    sqliteStep(stmt);
    sqliteFinalize(stmt);
}

char transformDbLoad(const char* name, Transform* transformOut) {
    void* stmt  = sqliteStatement("SELECT transform FROM transform WHERE name = ?;");
    char result = 0;
    sqliteBindText(stmt, 1, name);
    if (sqliteStep(stmt)) {
        void* transformBlob = sqliteGetBlob(stmt, 0);
        memcpy(transformOut, transformBlob, sizeof(Transform));
        result = 1;
    }
    sqliteFinalize(stmt);
    return result;
}
