#include <stdlib.h>

extern "C" {
void* memoryAlloc(size_t size) { return malloc(size); }
void  memoryFree(void* ptr)    { free(ptr); }
void* memoryRealloc(void* ptr, size_t size) { return realloc(ptr, size); }
}