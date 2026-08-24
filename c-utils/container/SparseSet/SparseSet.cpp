#include "SparseSet.h"
#include <cassert>
#include <cstring>
#include "Utils.h"

namespace utils {
SparseSet* ssNew(u32 elementSize) {
    SparseSet* ss = new SparseSet();
    ss->elementSize = elementSize;
    ss->destroy     = nullptr;
    ss->swapIn      = nullptr;
    return ss;
}

SparseSet* ssNewWithDestructor(u32 elementSize, void (*destroy)(void*), void (*swapIn)(void*, void*)) {
    SparseSet* ss = ssNew(elementSize);
    ss->destroy   = destroy;
    ss->swapIn    = swapIn;
    return ss;
}

void ssDestroy(void* pss) {
    SparseSet* ss = static_cast<SparseSet*>(pss);
    if (ss->destroy) {
        for (u32 i = 0; i < ss->size; i++) {
            ss->destroy(ss->data.data() + (static_cast<size_t>(i) * ss->elementSize));
        }
    }
    delete ss;
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
    char* dst = ss->data.data() + (static_cast<size_t>(sparseValue) * ss->elementSize);
    char* src = ss->data.data() + (static_cast<size_t>((ss->size - 1) * ss->elementSize));
    if (dst != src) {
        // For C++ elements, transfer contents with a proper move-assign so
        // the destination's old members are freed and src stays valid; a raw
        // byte copy would alias the same heap buffers in both slots.
        if (ss->swapIn) {
            ss->swapIn(dst, src);
        } else {
            std::memcpy(dst, src, ss->elementSize);
        }
    } else if (ss->destroy) {
        // Removing the last live element: its object now sits in a dead slot
        // and would never be destroyed otherwise.
        ss->destroy(src);
    }
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
    if (ss->destroy) {
        for (u32 i = 0; i < ss->size; i++) {
            ss->destroy(ss->data.data() + (static_cast<size_t>(i) * ss->elementSize));
        }
    }
    ss->size = 0;
    ss->dense.clear();
    ss->sparse.clear();
}}  // namespace utils
