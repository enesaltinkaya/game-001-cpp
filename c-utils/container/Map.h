#pragma once
#include "stb/git/stb_ds.h"  // IWYU pragma: keep

#define Map(keyType, valueType) \
    struct {                    \
        keyType key;            \
        valueType value;        \
    }*

#define mapContainsKey(map, key) (hmgeti((map), key) != -1)
#define mapPut(map, key, value) hmput((map), (key), (value))
#define mapGet(map, key) hmget((map), (key))
#define mapRemove(map, key) hmdel((map), key)
#define mapSize(map) hmlen((map))
#define mapFree(map) hmfree((map))
#define mapClear(map) ((map) ? stbds_header((map) - 1)->length = 1 : 0)

#define StrMap(b)  \
    struct {       \
        char* key; \
        b value;   \
    }*

#define strmapContainsKey(strMap, key) (shgeti((strMap), key) != -1)
#define strmapPut(strMap, key, value) \
    if (!(strMap)) {                  \
        sh_new_strdup((strMap));      \
    };                                \
    shput((strMap), key, value)
#define strmapGet(strMap, key) shget((strMap), key)
#define strmapRemove(strMap, key) shdel((strMap), (key))
#define strmapSize(strMap) shlen((strMap))
#define strmapFree(strMap) shfree((strMap))
