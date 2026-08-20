#pragma once
#include "stb/git/stb_ds.h"               // IWYU pragma: keep
#include "memorymanager/MemoryManager.h"  // IWYU pragma: keep

#define Array(type) type*

#define arrayPut(array, value) arrput((array), (value))
#define arrayInsert(array, index, value) arrins(array, index, value)
#define arraySize(array) arrlenu(array)

#define arrayLast(array) arrlast
#define arrayPop(array) arrpop(array)

// workaround stbds_arrfree bug (use of STBDS_FREE outside of STB_DS_IMPLEMENTATION)
// https://github.com/nothings/stb/issues/1632
#define arrayFree(array) ((void)((array) ? memoryFree(stbds_header(array)) : (void)0), (array) = NULL)
#define arrayDeleteSwap(array, index) arrdelswap(array, index)
#define arrayDeleteSlow(array, index) arrdel(array, index)
#define arraySetSize(array, size) arrsetlen(array, size)
#define arraySetSizeZeroed(array, newSize)                                          \
    do {                                                                            \
        u64 _oldSize = arraySize(array);                                            \
        arraySetSize((array), (newSize));                                           \
        if (newSize > _oldSize) {                                                   \
            memset(&(array)[_oldSize], 0, sizeof(*(array)) * (newSize - _oldSize)); \
        }                                                                           \
    } while (0)
#define arraySetCap(array, size) arrsetcap(array, size)
#define arrayClear(array) ((array) ? stbds_header(array)->length = 0 : 0)

#define foreach(item, array)                                                                   \
    for (i32 _keep_##__LINE__ = 1, _count_##__LINE__ = 0, _size_##__LINE__ = arraySize(array); \
         _keep_##__LINE__ && _count_##__LINE__ != _size_##__LINE__;                            \
         _keep_##__LINE__ = !_keep_##__LINE__, _count_##__LINE__++)                            \
        for (item = (array)[_count_##__LINE__]; _keep_##__LINE__; _keep_##__LINE__ = !_keep_##__LINE__)

#define foreachptr(item, array)                                                                \
    for (i32 _keep_##__LINE__ = 1, _count_##__LINE__ = 0, _size_##__LINE__ = arraySize(array); \
         _keep_##__LINE__ && _count_##__LINE__ != _size_##__LINE__;                            \
         _keep_##__LINE__ = !_keep_##__LINE__, _count_##__LINE__++)                            \
        for (item = &(array)[_count_##__LINE__]; _keep_##__LINE__; _keep_##__LINE__ = !_keep_##__LINE__)
