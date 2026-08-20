#pragma once

typedef struct Platform {
    char isWindows;
    char seperator[2];
    char cwd[1024];
    char executablePath[1024];
    char dataDirectory[1024];
} Platform;

extern Platform platform;

void platformInit(void);
void platformDestroy(void);
void createDirectory(const char* name);
u64 memoryUsage(void);
int numberOfCores(void);
char* gnuBasename(char* path);

typedef struct MappedFile {
    void* data;
    u64 size;
    // Internal platform specific handles
    void* internal_handle_1;
    void* internal_handle_2;
} MappedFile;

// Opens (or creates) a file and maps it to memory.
// If 'create_and_resize' is true: Creates file (truncates if exists) and forces size to 'size'.
// If 'create_and_resize' is false: Opens existing file, 'size' is ignored (uses actual file size).
MappedFile* map_file(const char* path, u64 size, char create_and_resize);

// Unmaps and closes the file.
void unmap_file(MappedFile* map);

