#pragma once
#include <vector>

namespace utils {
struct SparseSet {
    std::vector<u32> dense;
    std::vector<u32> sparse;
    std::vector<char> data;

    u32 elementSize;
    u32 size;
    u32 capacity;

    // Optional C++ element callbacks (both nullptr for POD elements).
    // destroy: fully destroy the element in place, including any sub-
    //   resources owned by the element (called on clear/destroy, and when
    //   swap-removing the last live element).
    // swapIn: transfer the element at src over the element at dst
    //   (destroying dst's current contents first), leaving src in a valid
    //   state; no-op when dst == src (used by swap-remove).
    // Both must be passed together.
    void (*destroy)(void*);
    void (*swapIn)(void* dst, void* src);
};

SparseSet* ssNew(u32 elementSize);
// Like ssNew but registers callbacks for non-trivial (C++) element types,
// e.g. components containing std:: members.  The same set must always be used
// with the same callbacks for its whole lifetime.
SparseSet* ssNewWithDestructor(u32 elementSize, void (*destroy)(void*), void (*swapIn)(void*, void*));
void ssDestroy(void* pss);

void ssReserve(SparseSet* ss, u32 capacity);
bool ssContainsValue(SparseSet* ss, u32 value);
void* ssGetDataByValue(SparseSet* ss, u32 value);
void* ssGetDataByIndex(SparseSet* ss, u32 index);
u32 ssGetValueByIndex(SparseSet* ss, u32 index);
bool ssRemoveByValue(SparseSet* ss, u32 value);

void* ssInsert(SparseSet* ss, u32 value, const void* data);
void* ssNewItem(SparseSet* ss, u32 value);
void ssClear(SparseSet* ss);
}  // namespace utils
