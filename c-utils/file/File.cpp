#include "File.h"
#include <stdio.h>
#include <sys/stat.h>
#include "Utils.h"
#include "platform/Platform.h"
#include "logger/Logger.h"
#include "string/String.h"

namespace utils {
char* R_relativePath(const char* path, char* in) {
    snprintf(in, 1024, "%s%s", platform.cwd, path);
    return in;
}

bool fileExists(const char* path) {
    struct stat buffer = {};
    return stat(path, &buffer) == 0;
}

void fileWrite(const char* path, const char* data) {
    FILE* file = fopen(path, "w");
    if (!file) {
        warn("file open error on write: %s", path);
        return;
    }
    fprintf(file, "%s", data);
    fclose(file);
}

void fileWriteBinary(const char* path, void* data, u32 size) {
    FILE* file = fopen(path, "wb+");
    if (!file) {
        warn("file open error on binary write: %s", path);
        return;
    }
    fwrite(data, 1, size, file);
    fclose(file);
}

void fileAppend(const char* path, const char* data) {
    FILE* file = fopen(path, "a");
    if (!file) {
        warn("file open error on append: %s", path);
        return;
    }
    fprintf(file, "%s", data);
    fclose(file);
}

String fileRead(const char* path) {
    if (!fileExists(path)) terminate("file: %s doesn't exist", path);

    String string = {};

    FILE* file = fopen(path, "rb+");
    fseek(file, 0, SEEK_END);
    u32 size = ftell(file);
    fseek(file, 0, SEEK_SET);
    stringSetSize(&string, size);

    if (!fread(string.data, size, 1, file)) {
        warn("fread 0 bytes %s", path);
    }
    fclose(file);
    return string;
}

void fileRead2(String* string, const char* path) {
    struct stat buffer = {};
    if (stat(path, &buffer) != 0) {
        terminate("file: %s doesn't exist", path);
    }
    FILE* file = fopen(path, "rb");
    fseek(file, 0, SEEK_END);
    u32 size = ftell(file);
    fseek(file, 0, SEEK_SET);
    stringSetSize(string, size);

    if (!fread(string->data, size, 1, file)) {
        warn("fread 0 bytes %s", path);
    }
    fclose(file);
}

char* fileRead3(const char* path, u32* fileSize) {
    struct stat buffer = {};
    if (stat(path, &buffer) != 0) {
        terminate("file: %s doesn't exist", path);
    }
    FILE* file = fopen(path, "rb");
    fseek(file, 0, SEEK_END);
    u32 size = ftell(file);
    if (fileSize) {
        *fileSize = size;
    }
    fseek(file, 0, SEEK_SET);
    char* out = static_cast<char*>(malloc(size));

    if (!fread(out, size, 1, file)) {
        warn("fread 0 bytes %s", path);
    }
    fclose(file);
    return out;
}
}  // namespace utils
