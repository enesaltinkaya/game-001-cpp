#pragma once

// does not hold data, only u32 integer
typedef struct SparseSetSimple {
    u32* dense;
    u32* sparse;

    u32 size;
    u32 capacity;
} SparseSetSimple;

SparseSetSimple* ssSimpleNew(void);
void ssSimpleDestroy(void* pss);

void ssSimpleReserve(SparseSetSimple* ss, u32 capacity);
char ssSimpleContainsValue(SparseSetSimple* ss, u32 value);
void ssSimpleInsert(SparseSetSimple* ss, u32 value);
char ssSimpleRemoveByValue(SparseSetSimple* ss, u32 value);
u32 ssSimpleGetValueByIndex(SparseSetSimple* ss, u32 index);
void ssSimpleClear(SparseSetSimple* ss);
