#include "SparseSetSimple.h"
#include "memorymanager/MemoryManager.h"

SparseSetSimple* ssSimpleNew(void) {
    SparseSetSimple* ss = static_cast<SparseSetSimple*>(memoryAlloc(sizeof *ss));
    return ss;
}

void ssSimpleDestroy(void* pss) {
    SparseSetSimple* ss = static_cast<SparseSetSimple*>(pss);
    arrayFree(ss->sparse);
    arrayFree(ss->dense);
    memoryFree(ss);
}

void ssSimpleReserve(SparseSetSimple* ss, u32 capacity) {
    arrsetlen(ss->dense, capacity);
    arrsetlen(ss->sparse, capacity);

    u32 from = ss->capacity * sizeof(u32);
    u32 size = (capacity - ss->capacity) * sizeof(u32);
    memset((char*)ss->dense + from, 0, size);
    memset((char*)ss->sparse + from, 0, size);

    ss->capacity = capacity;
}

char ssSimpleContainsValue(SparseSetSimple* ss, u32 value) {
    if (value >= ss->capacity) {
        return 0;
    }
    u32 sparseValue = ss->sparse[value];
    if (sparseValue >= ss->size) {
        return 0;
    }
    u32 denseValue = ss->dense[sparseValue];
    return (denseValue == value);
}

char ssSimpleRemoveByValue(SparseSetSimple* ss, u32 value) {
    if (!ssSimpleContainsValue(ss, value)) {
        return 0;
    }

    u32 sparseValue        = ss->sparse[value];
    u32 lastDense          = ss->dense[ss->size - 1];
    ss->dense[sparseValue] = lastDense;
    ss->sparse[lastDense]  = sparseValue;
    ss->size--;
    return 1;
}

void ssSimpleInsert(SparseSetSimple* ss, u32 value) {
    /** TODO: benchmark, contains check vs overwrite */
    // assert(!ssSimpleContainsValue(ss, value));

    if (value >= ss->capacity) {
        ssSimpleReserve(ss, value + 1);
    }
    ss->dense[ss->size] = value;
    ss->sparse[value]   = ss->size;
    ss->size++;
}

u32 ssSimpleGetValueByIndex(SparseSetSimple* ss, u32 index) {
    return ss->dense[index];
}

void ssSimpleClear(SparseSetSimple* ss) {
    ss->size = 0;
}
