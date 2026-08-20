#include "MemoryManager.h"
#include <assert.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include "Utils.h"
#include "buddy_alloc/git/buddy_alloc.h"
#include "logger/Logger.h"
#include "thread/Thread.h"

// ---from buddy_alloc docs---
// Design
// The allocator was designed with the following requirements in mind.
// Allocation and deallocation operations should behave in a similar and predictable way regardless of the state of the allocator.
// The allocator's metadata size should be predictable based on the arena's size and not dependent on the state of the allocator.
// The allocator's metadata location should be external to the arena.
// Returned memory should be aligned to known and specified block size.

// The following were not design goals
// To be used by multiple threads at the same time without additional locking.
// To be a general purpose malloc() replacement.

#define MEMORY_ALLOCATION_SIZE 2ULL * 1024 * 1024 * 1024

static unsigned char* memoryArena;
static unsigned char* memoryMetaData;
static struct buddy*  memoryAllocatorHandle;

static MemoryAllocatorType memoryAllocatorType;
static Thread bodyLock;

void memoryInit(MemoryAllocatorType allocator) {
    printf("initializing allocator: %s\n", allocator == ALLOCATOR_SYSTEM ? "System" : "Buddy");
    memoryAllocatorType = allocator;
    if (memoryAllocatorType == ALLOCATOR_BUDDY) {
        size_t metaSize       = buddy_sizeof(MEMORY_ALLOCATION_SIZE);
        memoryMetaData        = static_cast<unsigned char*>(malloc(metaSize));
        memoryArena           = static_cast<unsigned char*>(malloc(MEMORY_ALLOCATION_SIZE));
        memoryAllocatorHandle = buddy_init(memoryMetaData, memoryArena, MEMORY_ALLOCATION_SIZE);
    } else if (memoryAllocatorType == ALLOCATOR_SYSTEM) {
    }
}

void memoryDestroy(void) {
    threadLock(&bodyLock);
    if (isDebug() && memoryAllocatorType == ALLOCATOR_BUDDY && !buddy_is_empty(memoryAllocatorHandle)) {
        setlocale(LC_NUMERIC, "");
        error("buddy allocator spillage: %'ld bytes",
              buddy_arena_size(memoryAllocatorHandle) - buddy_arena_free_size(memoryAllocatorHandle));
    }

    if (memoryMetaData) free(memoryMetaData);
    if (memoryArena) free(memoryArena);
    threadUnlock(&bodyLock);
}

void* memoryAlloc(size_t size) {
    assert(size);
    void* result = {};
    if (memoryAllocatorType == ALLOCATOR_BUDDY) {
        threadLock(&bodyLock);
        result = buddy_malloc(memoryAllocatorHandle, size);
        memset(result, 0, size);
        threadUnlock(&bodyLock);
    } else if (memoryAllocatorType == ALLOCATOR_SYSTEM) {
        result = calloc(1, size);
    }
    return result;
}

void* memoryRealloc(void* ptr, size_t size) {
    void* result = {};
    if (memoryAllocatorType == ALLOCATOR_BUDDY) {
        threadLock(&bodyLock);
        result = buddy_realloc(memoryAllocatorHandle, ptr, size, 0);
        threadUnlock(&bodyLock);
    } else if (memoryAllocatorType == ALLOCATOR_SYSTEM) {
        result = realloc(ptr, size);
    }
    return result;
}

void memoryFree(void* ptr) {
    if (memoryAllocatorType == ALLOCATOR_BUDDY) {
        threadLock(&bodyLock);
        buddy_free(memoryAllocatorHandle, ptr);
        threadUnlock(&bodyLock);
    } else if (memoryAllocatorType == ALLOCATOR_SYSTEM) {
        free(ptr);
    }
}

MemoryAllocatorType getMemoryAllocType(void) {
    return memoryAllocatorType;
}
