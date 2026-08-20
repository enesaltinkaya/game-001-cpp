#pragma once

typedef struct ArenaAllocator {
    void* memory;
    u64 size;
    u64 cursor;
} ArenaAllocator;

void* arenaInit(u64 size);
void arenaDestroy(ArenaAllocator* arena);
void* arenaAlloc(ArenaAllocator* arena, u64 size);
void arenaClear(ArenaAllocator* arena);
