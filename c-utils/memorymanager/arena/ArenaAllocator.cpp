#include "Utils.h"

#define ARENA_ALIGN 8

typedef struct ArenaAllocator {
    void* memory;
    uint64_t size;
    uint64_t cursor;
} ArenaAllocator;

void* arenaInit(uint64_t size) {
    ArenaAllocator* arena = static_cast<ArenaAllocator*>(malloc(sizeof *arena));
    arena->memory         = malloc(size);
    arena->size           = size;
    arena->cursor         = 0;
    return arena;
}

void arenaDestroy(ArenaAllocator* arena) {
    free(arena->memory);
    free(arena);
}

void* arenaAlloc(ArenaAllocator* arena, uint64_t size) {
    size = (size + ARENA_ALIGN - 1) & ~(ARENA_ALIGN - 1);
    if (arena->cursor + size > arena->size) terminate("arena full");
    void* ptr = (char*)arena->memory + arena->cursor;
    arena->cursor += size;
    return memset(ptr, 0, size);
}

void arenaClear(ArenaAllocator* arena) {
    arena->cursor = 0;
}
