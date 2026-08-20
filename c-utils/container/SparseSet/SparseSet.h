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
};

SparseSet* ssNew(u32 elementSize);
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
