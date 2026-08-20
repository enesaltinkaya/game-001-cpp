#include <ctype.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <atomic>
#include <algorithm>
#include <stdio.h>
#include <cstdlib>
#include "string/String.h"

namespace utils {
static void maybeGrow(String* string, u32 grow);

String* stringNew(const char* format, ...) {
    String* string = new String{};
    string->onHeap = true;

    va_list args;
    String message = {};
    va_list args_copy;
    va_start(args, format);
    va_copy(args_copy, args);
    size_t len = vsnprintf(0, 0, format, args_copy);
    stringSetSize(&message, len + 1);
    vsnprintf(message.data, len + 1, format, args);
    va_end(args);

    stringAppend(string, message.data);
    stringDestroy(&message);

    return string;
}

void stringDestroy(String* string) {
    if (string->data != nullptr) {
        free(string->data);
        string->data = nullptr;
    }

    string->size      = 0;
    string->allocated = 0;

    if (string->onHeap) {
        delete string;
    }
}

#define max(a, b) (((a) > (b)) ? (a) : (b))

void maybeGrow(String* string, u32 grow) {
    if (string->allocated <= grow) {
        if (string->allocated == 0) {
            string->allocated = 10;
        }
        u32 newSize       = static_cast<u32>(std::max<double>(string->allocated * 1.5, grow + 1));  // +1 is for \0 for
        char* temp        = static_cast<char*>(realloc(string->data, newSize));
        string->data      = temp;
        string->allocated = newSize;
    }
}

void stringAppend(String* string, const char* data) {
    u32 size = strlen(data);
    maybeGrow(string, string->size + size);
    memcpy(string->data + string->size, data, size);
    string->size += size;
    string->data[string->size] = 0;
}

void stringSetSize(String* string, u32 size) {
    maybeGrow(string, size);
    string->size       = size;
    string->data[size] = 0;
}

void stringAppendPrintf(String* string, const char* format, ...) {
    va_list args;
    String message = {};
    va_list args_copy;
    va_start(args, format);
    va_copy(args_copy, args);
    size_t len = vsnprintf(0, 0, format, args_copy);
    stringSetSize(&message, len);
    vsnprintf(message.data, len + 1, format, args);
    va_end(args);

    stringAppend(string, message.data);
    stringDestroy(&message);
}

void stringReplace(String* string, const char* find, const char* replace) {
    char* replaced = strReplace(string->data, find, replace);
    stringClear(string);
    stringAppend(string, replaced);
    free(replaced);
}

void stringClear(String* string) {
    stringSetSize(string, 0);
}

String* stringDuplicate(String* string) {
    String* newString = stringNew("");
    stringPrintf(newString, string->data);
    return newString;
}

void stringTrim(String* string) {
    while (isspace(*string->data) != 0) {
        memcpy(string->data, string->data + 1, string->size);
        string->size--;
    }

    char* back = string->data + string->size;
    while (isspace(*--back) != 0) {
        string->size--;
    }

    string->data[string->size] = 0;
}

bool stringStartsWith(String* string, const char* check) {
    return strncmp(string->data, check, strlen(check)) == 0;
}

bool strStartsWith(const char* string, const char* check) {
    return strncmp(string, check, strlen(check)) == 0;
}

bool stringEndsWith(String* string, const char* check) {
    u32 checkLen = strlen(check);
    if (checkLen > string->size) {
        return false;
    }
    return strncmp(string->data + string->size - checkLen, check, checkLen) == 0;
}

bool strEndsWithC(const char* string, const char* check) {
    u32 checkLen  = strlen(check);
    u32 stringLen = strlen(string);
    if (checkLen > stringLen) {
        return false;
    }
    return strncmp(string + stringLen - checkLen, check, checkLen) == 0;
}

// void String::split( String*string,std::vector<String*>* vec, const char*
// delim) const {
//   char* dup   = strdup(str);
//   char* token = strtok(dup, delim);
//   while (token != NULL) {
//     vec->push_back(new String(token));
//     token = strtok(NULL, delim);
//   }
//   memoryFree(dup);
// }

void stringPrintf(String* string, const char* format, ...) {
    va_list args;
    String temp = {};
    va_list args_copy;
    va_start(args, format);
    va_copy(args_copy, args);
    size_t len = vsnprintf(0, 0, format, args_copy);
    stringSetSize(&temp, len + 1);
    vsnprintf(temp.data, len + 1, format, args);
    va_end(args);

    stringClear(string);
    stringAppend(string, temp.data);
    stringDestroy(&temp);
}

bool stringEquals(String* string, const char* str) {
    return !strcmp(string->data, str);
}

char* strReplace(const char* source, const char* find, const char* replace) {
    char* result;
    int i;
    int cnt     = 0;
    int newWlen = (int)strlen(replace);
    int oldWlen = (int)strlen(find);

    for (i = 0; source[i] != '\0'; i++) {
        if (strstr(&source[i], find) == &source[i]) {
            cnt++;
            i += oldWlen - 1;
        }
    }
    result = static_cast<char*>(malloc(i + (cnt * (newWlen - oldWlen)) + 1));

    i = 0;
    while (*source != 0) {
        if (strstr(source, find) == source) {
            strcpy(&result[i], replace);
            i += newWlen;
            source += oldWlen;
        } else {
            result[i++] = *source++;
        }
    }
    result[i] = '\0';
    return result;
}

void stringAppendBinary(String* string, void* data, u32 size) {
    maybeGrow(string, string->size + size);
    memcpy(string->data + string->size, data, size);
    string->size += size;
    string->data[string->size] = 0;
}

bool strequals(const char* str1, const char* str2) {
    return !strcmp(str1, str2);
}

#define NUM_TEMP_BUFFERS_POOL 256
#define TEMP_STRING_POOL_ITEM_MAX_SIZE 256

char* strtmp(const char* format, ...) {
    static char g_tempStringPool[NUM_TEMP_BUFFERS_POOL][TEMP_STRING_POOL_ITEM_MAX_SIZE];
    static std::atomic<int> g_poolCurrentIndex{0};
    int currentIndex                      = static_cast<int>(g_poolCurrentIndex.fetch_add(1, std::memory_order_relaxed));
    int buffer_idx                        = currentIndex % NUM_TEMP_BUFFERS_POOL;
    va_list args;
    va_start(args, format);
    vsnprintf(g_tempStringPool[buffer_idx], TEMP_STRING_POOL_ITEM_MAX_SIZE, format, args);
    va_end(args);
    return g_tempStringPool[buffer_idx];
}

// char* stringTempPrintf(const char* format, ...) {
//     static __thread char buf[1024];  // Thread-local storage
//     va_list args;
//     va_start(args, format);
//     vsnprintf(buf, sizeof(buf), format, args);
//     va_end(args);
//     return buf;
// }

std::vector<String*> stringSplit(String* string, const char* delim) {
    std::vector<String*> result = {};

    if (string == nullptr || string->data == nullptr || string->size == 0) {
        return result;
    }

    if (delim == nullptr || *delim == '\0') {
        result.push_back(stringDuplicate(string));
        return result;
    }

    u32 delim_len         = strlen(delim);
    const char* start_ptr = string->data;
    const char* delim_ptr;

    while ((delim_ptr = strstr(start_ptr, delim)) != NULL) {
        u32 token_len = delim_ptr - start_ptr;
        result.push_back(static_cast<String*>(stringNew("%.*s", token_len, start_ptr)));
        start_ptr = delim_ptr + delim_len;
    }

    u32 last_token_len = string->size - (start_ptr - string->data);
    result.push_back(static_cast<String*>(stringNew("%.*s", last_token_len, start_ptr)));
    return result;
}

void stringArrayDestroy(std::vector<String*> array) {
    for (i32 i = 0, si = static_cast<i32>(array.size()); i < si; i++) {
        stringDestroy(array[i]);
    }
}

bool strContains(const char* haystack, const char* needle) {
    return (strstr(haystack, needle) != nullptr);
}

std::vector<char*> strSplit(const char* input, const char* delimiter) {
    std::vector<char*> result = {};
    char* str           = strdup(input);
    for (char* token = strtok(str, delimiter); token; token = strtok(NULL, delimiter)) {
        result.push_back(strdup(token));
    }
    free(str);
    return result;
}

void strSplitFree(std::vector<char*> result) {
    for (i32 i = 0; i < static_cast<i32>(result.size()); i++) {
        free(result[i]);
    }
}

const char* strBaseName(const char* path) {
    const char* slash = strrchr(path, '/');
#ifdef _WIN32
    const char* backslash = strrchr(path, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
#endif
    return slash ? slash + 1 : path;
}

void strToLowerInPlace(char* s) {
    while (*s) {
        *s = static_cast<char>(tolower(static_cast<unsigned char>(*s)));
        s++;
    }
}

void strToUpperInPlace(char* s) {
    while (*s) {
        *s = static_cast<char>(toupper(static_cast<unsigned char>(*s)));
        s++;
    }
}
}  // namespace utils
