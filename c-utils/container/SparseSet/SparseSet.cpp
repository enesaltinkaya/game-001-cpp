#include "SparseSet.h"
#include <stddef.h>
#include "Utils.h"
#include "memorymanager/MemoryManager.h"
#include "string/String.h"  // IWYU pragma: keep
#include <assert.h>

SparseSet* ssNew(u32 elementSize) {
    SparseSet* ss = static_cast<SparseSet*>(memoryAlloc(sizeof *ss));
    ss->elementSize = elementSize;
    return ss;
}

void ssDestroy(void* pss) {
    SparseSet* ss = static_cast<SparseSet*>(pss);
    arrayFree(ss->sparse);
    arrayFree(ss->dense);
    memoryFree(ss->data);
    memoryFree(ss);
}

void ssReserve(SparseSet* ss, u32 capacity) {
    if (capacity <= ss->capacity) return;
    char* temp = static_cast<char*>(memoryRealloc(ss->data, (u64)capacity * ss->elementSize));
    if (!temp) terminate("sparseset realloc failed");
    ss->data = temp;

    arraySetSize(ss->dense, capacity);
    arraySetSize(ss->sparse, capacity);

    u64 from = (u64)ss->capacity * sizeof(u32);
    u64 size = (u64)(capacity - ss->capacity) * sizeof(u32);
    memset((char*)ss->dense + from, 0, size);
    memset((char*)ss->sparse + from, 0, size);

    ss->capacity = capacity;
}

char ssContainsValue(SparseSet* ss, u32 value) {
    if (value >= ss->capacity) return 0;
    u32 sparseValue = ss->sparse[value];
    if (sparseValue >= ss->size) return 0;
    u32 denseValue = ss->dense[sparseValue];
    return denseValue == value;
}

void* ssGetDataByValue(SparseSet* ss, u32 value) {
    if (!ssContainsValue(ss, value)) return NULL;

    u32 sparseValue = ss->sparse[value];
    u32 address     = ss->elementSize * sparseValue;
    return ss->data + address;
}

void* ssGetDataByIndex(const SparseSet* ss, u32 index) {
    assert(index < ss->size);
    return ss->data + ((size_t)(ss->elementSize * index));
}

u32 ssGetValueByIndex(SparseSet* ss, u32 index) {
    assert(index < ss->size);
    return ss->dense[index];
}

char ssRemoveByValue(SparseSet* ss, u32 value) {
    if (!ssContainsValue(ss, value)) {
        return 0;
    }

    u32 sparseValue        = ss->sparse[value];
    u32 lastDense          = ss->dense[ss->size - 1];
    ss->dense[sparseValue] = lastDense;
    ss->sparse[lastDense]  = sparseValue;
    u64 dstOffset          = (u64)sparseValue * ss->elementSize;
    u64 srcOffset          = (((u64)ss->size - 1) * ss->elementSize);
    memcpy(ss->data + dstOffset, ss->data + srcOffset, ss->elementSize);
    ss->size--;
    return 1;
}

void* ssInsert(SparseSet* ss, u32 value, const void* data) {
    assert(!ssContainsValue(ss, value));

    if (value >= ss->capacity) {
        ssReserve(ss, value + 1);
    }
    u32 address = ss->elementSize * ss->size;
    memcpy(ss->data + address, data, ss->elementSize);
    ss->dense[ss->size] = value;
    ss->sparse[value]   = ss->size;
    ss->size++;
    return ss->data + address;
}

void* ssNewItem(SparseSet* ss, u32 value) {
    assert(!ssContainsValue(ss, value));

    if (value >= ss->capacity) {
        ssReserve(ss, value + 1);
    }
    u32 address = ss->elementSize * ss->size;
    memset(ss->data + address, 0, ss->elementSize);
    ss->dense[ss->size] = value;
    ss->sparse[value]   = ss->size;
    ss->size++;
    return ss->data + address;
}

void ssClear(SparseSet* ss) {
    ss->size = 0;
    arrayClear(ss->dense);
    arrayClear(ss->sparse);
}
