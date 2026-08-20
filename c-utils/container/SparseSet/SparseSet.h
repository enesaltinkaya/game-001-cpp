#pragma once

typedef struct SparseSet {
    u32* dense;
    u32* sparse;
    char* data;

    u32 elementSize;
    u32 size;
    u32 capacity;
} SparseSet;

SparseSet* ssNew(u32 elementSize);
void ssDestroy(void* pss);

void ssReserve(SparseSet* ss, u32 capacity);
char ssContainsValue(SparseSet* ss, u32 value);
void* ssGetDataByValue(SparseSet* ss, u32 value);
void* ssGetDataByIndex(const SparseSet* ss, u32 index);
u32 ssGetValueByIndex(SparseSet* ss, u32 index);
char ssRemoveByValue(SparseSet* ss, u32 value);

void* ssInsert(SparseSet* ss, u32 value, const void* data);
void* ssNewItem(SparseSet* ss, u32 value);
void ssClear(SparseSet* ss);
