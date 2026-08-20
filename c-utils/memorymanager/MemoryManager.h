#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MemoryAllocatorType {
    ALLOCATOR_BUDDY,
    ALLOCATOR_SYSTEM,
} MemoryAllocatorType;

void memoryInit(MemoryAllocatorType memoryAllocator);
void memoryDestroy(void);
MemoryAllocatorType getMemoryAllocType(void);

void* memoryAlloc(size_t size);

void* memoryRealloc(void* ptr, size_t size);
void memoryFree(void* ptr);

#ifdef __cplusplus
}
#endif
