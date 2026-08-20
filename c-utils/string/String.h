#pragma once
#include "../container/Array.h"

typedef struct String {
    char* data;
    u32 allocated;
    u32 size;
    char onHeap;
} String;

String* stringNew(const char* format, ...);
void stringDestroy(String* string);

void stringAppend(String* string, const char* data);
void stringAppendPrintf(String* string, const char* format, ...);
void stringSetSize(String* string, u32 size);
void stringReplace(String* string, const char* find, const char* replace);
char* strReplace(const char* source, const char* find, const char* replace);

String* stringDuplicate(String* string);
void stringTrim(String* string);
char stringStartsWith(String* string, const char* check);
char strStartsWith(const char* string, const char* check);
char stringEndsWith(String* string, const char* check);
char strEndsWithC(const char* string, const char* check);
void stringClear(String* string);
void stringPrintf(String* string, const char* format, ...);
char stringEquals(String* string, const char* str);
char strequals(const char* str1, const char* str2);
char strContains(const char* haystack, const char* needle);

void stringAppendBinary(String* string, void* data, u32 size);

char* strtmp(const char* format, ...);

Array(String*) stringSplit(String* string, const char* delim);
void stringArrayDestroy(Array(String*) array);

Array(char*) strSplit(const char* input, const char* delimiter);
void strSplitFree(Array(char*) result);

const char* strBaseName(const char* path);
void strToLowerInPlace(char* s);
void strToUpperInPlace(char* s);
