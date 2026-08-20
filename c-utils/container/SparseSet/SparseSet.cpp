#include "SparseSet.h"
#include <cassert>
#include <cstring>
#include "Utils.h"

namespace utils {
SparseSet* ssNew(u32 elementSize) {
    SparseSet* ss = new SparseSet();
    ss->elementSize = elementSize;
    return ss;
}

void ssDestroy(void* pss) {
    delete static_cast<SparseSet*>(pss);
}

void ssReserve(SparseSet* ss, u32 capacity) {
    if (capacity <= ss->capacity) return;

    ss->data.resize(static_cast<size_t>(capacity) * ss->elementSize);
    ss->dense.resize(capacity, 0);
    ss->sparse.resize(capacity, 0);

    ss->capacity = capacity;
}

bool ssContainsValue(SparseSet* ss, u32 value) {
    if (value >= ss->capacity) return false;
    u32 sparseValue = ss->sparse[value];
    if (sparseValue >= ss->size) return false;
    u32 denseValue = ss->dense[sparseValue];
    return denseValue == value;
}

void* ssGetDataByValue(SparseSet* ss, u32 value) {
    if (!ssContainsValue(ss, value)) return nullptr;

    u32 sparseValue = ss->sparse[value];
    u32 address     = ss->elementSize * sparseValue;
    return ss->data.data() + address;
}

void* ssGetDataByIndex(SparseSet* ss, u32 index) {
    assert(index < ss->size);
    return ss->data.data() + (static_cast<size_t>(ss->elementSize) * index);
}

u32 ssGetValueByIndex(SparseSet* ss, u32 index) {
    assert(index < ss->size);
    return ss->dense[index];
}

bool ssRemoveByValue(SparseSet* ss, u32 value) {
    if (!ssContainsValue(ss, value)) {
        return false;
    }

    u32 sparseValue        = ss->sparse[value];
    u32 lastDense          = ss->dense[ss->size - 1];
    ss->dense[sparseValue] = lastDense;
    ss->sparse[lastDense]  = sparseValue;
    std::memcpy(ss->data.data() + static_cast<size_t>(sparseValue) * ss->elementSize,
                ss->data.data() + static_cast<size_t>(ss->size - 1) * ss->elementSize,
                ss->elementSize);
    ss->size--;
    return true;
}

void* ssInsert(SparseSet* ss, u32 value, const void* data) {
    assert(!ssContainsValue(ss, value));

    if (value >= ss->capacity) {
        ssReserve(ss, value + 1);
    }
    u32 address = ss->elementSize * ss->size;
    std::memcpy(ss->data.data() + address, data, ss->elementSize);
    ss->dense[ss->size] = value;
    ss->sparse[value]   = ss->size;
    ss->size++;
    return ss->data.data() + address;
}

void* ssNewItem(SparseSet* ss, u32 value) {
    assert(!ssContainsValue(ss, value));

    if (value >= ss->capacity) {
        ssReserve(ss, value + 1);
    }
    u32 address = ss->elementSize * ss->size;
    std::memset(ss->data.data() + address, 0, ss->elementSize);
    ss->dense[ss->size] = value;
    ss->sparse[value]   = ss->size;
    ss->size++;
    return ss->data.data() + address;
}

void ssClear(SparseSet* ss) {
    ss->size = 0;
    ss->dense.clear();
    ss->sparse.clear();
}}  // namespace utils
