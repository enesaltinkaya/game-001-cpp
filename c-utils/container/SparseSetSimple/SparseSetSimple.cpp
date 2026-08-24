#include "SparseSetSimple.h"

namespace utils {
SparseSetSimple* ssSimpleNew() {
    return new SparseSetSimple();
}

void ssSimpleDestroy(void* pss) {
    delete static_cast<SparseSetSimple*>(pss);
}

void ssSimpleReserve(SparseSetSimple* ss, u32 capacity) {
    if (capacity <= ss->capacity) return;
    ss->dense.resize(capacity, 0);
    ss->sparse.resize(capacity, 0);
    ss->capacity = capacity;
}

bool ssSimpleContainsValue(SparseSetSimple* ss, u32 value) {
    if (value >= ss->capacity) {
        return false;
    }
    u32 sparseValue = ss->sparse[value];
    if (sparseValue >= ss->size) {
        return false;
    }
    u32 denseValue = ss->dense[sparseValue];
    return (denseValue == value);
}

bool ssSimpleRemoveByValue(SparseSetSimple* ss, u32 value) {
    if (!ssSimpleContainsValue(ss, value)) {
        return false;
    }

    u32 sparseValue        = ss->sparse[value];
    u32 lastDense          = ss->dense[ss->size - 1];
    ss->dense[sparseValue] = lastDense;
    ss->sparse[lastDense]  = sparseValue;
    ss->size--;
    return true;
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
}}  // namespace utils
