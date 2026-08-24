#pragma once

#include "string/String.h"
#include "libzip/git/lib/zip.h"

namespace utils {
void dataManagerInit(void);
void dataManagerDestroy(void);

u32 dataManagerGetSize(const char* path);
u32 dataManagerGetCRC(const char* path);
bool dataManagerFileExists(const char* path);

String dataManagerRead(const char* path);

// Reads a specific chunk of a file without loading the whole thing
void dataManagerReadChunk(const char* path, void* buffer, u32 offset, u32 size);

struct ZipFile {
    struct zip_file* zipfile;
    u32 pos;
    u32 size;
};

// Returns an array of Strings with all file paths in paks matching the given
// extension (e.g. ".ktx2").  Caller must destroy each String.
std::vector<String> dataManagerListFiles(const char* extension);

void* dmRmlopen(const char* path);
void dmRmlclose(void* file);
unsigned long long dmRmlread(void* buffer, unsigned long long size, void* file);
int dmRmlseek(void* file, unsigned long long offset, int origin);
unsigned long long dmRmltell(void* file);
}  // namespace utils
