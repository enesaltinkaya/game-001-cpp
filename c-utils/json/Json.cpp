#include "jansson/git/build-linux/include/jansson.h"
#include <cstdlib>

namespace utils {
void jsonInit(void) {
    json_set_alloc_funcs(malloc, free);
}

const char* json_typeof_str(json_type type) {
    if (type == JSON_OBJECT) return "JSON_OBJECT";
    if (type == JSON_ARRAY) return "JSON_ARRAY";
    if (type == JSON_STRING) return "JSON_STRING";
    if (type == JSON_INTEGER) return "JSON_INTEGER";
    if (type == JSON_REAL) return "JSON_REAL";
    if (type == JSON_TRUE) return "JSON_TRUE";
    if (type == JSON_FALSE) return "JSON_FALSE";
    if (type == JSON_NULL) return "JSON_NULL";
    return "undefined";
}
}  // namespace utils
