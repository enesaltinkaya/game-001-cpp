#pragma once
#include <vector>

namespace utils {
struct String {
    char* data;
    u32 allocated;
    u32 size;
    bool onHeap;
};

String* stringNew(const char* format, ...);
void stringDestroy(String* string);

void stringAppend(String* string, const char* data);
void stringAppendPrintf(String* string, const char* format, ...);
void stringSetSize(String* string, u32 size);
void stringReplace(String* string, const char* find, const char* replace);
char* strReplace(const char* source, const char* find, const char* replace);

String* stringDuplicate(String* string);
void stringTrim(String* string);
bool stringStartsWith(String* string, const char* check);
bool strStartsWith(const char* string, const char* check);
bool stringEndsWith(String* string, const char* check);
bool strEndsWithC(const char* string, const char* check);
void stringClear(String* string);
void stringPrintf(String* string, const char* format, ...);
bool stringEquals(String* string, const char* str);
bool strequals(const char* str1, const char* str2);
bool strContains(const char* haystack, const char* needle);

void stringAppendBinary(String* string, void* data, u32 size);

char* strtmp(const char* format, ...);

std::vector<String*> stringSplit(String* string, const char* delim);
void stringArrayDestroy(std::vector<String*> array);

std::vector<char*> strSplit(const char* input, const char* delimiter);
void strSplitFree(std::vector<char*> result);

const char* strBaseName(const char* path);
void strToLowerInPlace(char* s);
void strToUpperInPlace(char* s);
}  // namespace utils
