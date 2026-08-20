#include "ecs/system/transform/TransformComponent.h"

namespace engine {
void transformDbInit(void) {
    if (!utils::sqliteTableExists("transform")) {
        utils::sqliteExecute(
            "CREATE TABLE IF NOT EXISTS transform ("
            "name TEXT PRIMARY KEY, "
            "transform BLOB);");
    }
}

void transformDbSave(const char* name, Transform* transformIn) {
    void* stmt = utils::sqliteStatement("REPLACE INTO transform (name, transform) VALUES (?, ?);");
    utils::sqliteBindText(stmt, 1, name);
    utils::sqliteBindBlob(stmt, 2, transformIn, sizeof(Transform));
    utils::sqliteStep(stmt);
    utils::sqliteFinalize(stmt);
}

char transformDbLoad(const char* name, Transform* transformOut) {
    void* stmt  = utils::sqliteStatement("SELECT transform FROM transform WHERE name = ?;");
    char result = 0;
    utils::sqliteBindText(stmt, 1, name);
    if (utils::sqliteStep(stmt)) {
        void* transformBlob = utils::sqliteGetBlob(stmt, 0);
        memcpy(transformOut, transformBlob, sizeof(Transform));
        result = 1;
    }
    utils::sqliteFinalize(stmt);
    return result;
}
}  // namespace engine
