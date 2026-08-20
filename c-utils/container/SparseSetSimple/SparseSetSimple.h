#pragma once
#include <vector>

// does not hold data, only u32 integer
namespace utils {
struct SparseSetSimple {
    std::vector<u32> dense;
    std::vector<u32> sparse;

    u32 size;
    u32 capacity;
};

SparseSetSimple* ssSimpleNew();
void ssSimpleDestroy(void* pss);

void ssSimpleReserve(SparseSetSimple* ss, u32 capacity);
bool ssSimpleContainsValue(SparseSetSimple* ss, u32 value);
void ssSimpleInsert(SparseSetSimple* ss, u32 value);
bool ssSimpleRemoveByValue(SparseSetSimple* ss, u32 value);
u32 ssSimpleGetValueByIndex(SparseSetSimple* ss, u32 index);
void ssSimpleClear(SparseSetSimple* ss);
}  // namespace utils
