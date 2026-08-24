#pragma once
#include "stdint.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint32_t uint;
typedef uint64_t u64;

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

typedef void* (*FnPtrPtr)(void*);
typedef void (*FnPtr)(void*);
typedef void (*FnVoid)(void);

#define vec2_print(vec) utils::info("(%.2f, %.2f)", (vec)[0], (vec)[1])
#define vec3_print(vec) utils::info("(%.2f, %.2f, %.2f)", (vec)[0], (vec)[1], (vec)[2])
#define vec4_print(vec) utils::info("(%.2f, %.2f, %.2f, %.2f)", (vec)[0], (vec)[1], (vec)[2], (vec)[3])
#define vec4s_print(vec) utils::info("(%.2f, %.2f, %.2f, %.2f)", (vec).x, (vec).y, (vec).z, (vec).w)

#define BILLION 1000000000.
#define MILLION 1000000.
