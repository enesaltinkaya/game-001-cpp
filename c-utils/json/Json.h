#pragma once

#include "jansson/git/build-linux/include/jansson.h"

namespace utils {
void jsonInit(void);
const char* json_typeof_str(json_type type);

#define Json json_t
#define jsonParseErrorString(string, error) json_loads((string), 0, (error))
#define jsonParse(string) json_loads((string), 0, 0)
#define jsonParseN(string, length) json_loadb((string), (length), 0, 0)
#define jsonNew() json_object()
#define jsonFree(object) json_decref((object))

// set values
#define jsonSetString(object, key, value) json_object_set_new((object), (key), json_string((value)))
#define jsonSetStringN(object, key, value, len) json_object_set_new((object), (key), json_stringn((value), (len)))
#define jsonSetInt(object, key, value) json_object_set_new((object), (key), json_integer((value)))
#define jsonSetBool(object, key, value) json_object_set_new((object), (key), json_boolean((value)))
#define jsonSetDouble(object, key, value) json_object_set_new((object), (key), json_real((value)))
#define jsonSetObject(object, key, value) json_object_set_new((object), (key), (value))
#define jsonSetArray(object, key, value) json_object_set_new((object), (key), (value))

// get values
#define jsonGetString(object, key) json_string_value(json_object_get((object), (key)))
#define jsonGetInt(object, key) json_integer_value(json_object_get((object), (key)))
#define jsonGetBool(object, key) json_is_true(json_object_get((object), (key)))
#define jsonGetDouble(object, key) json_real_value(json_object_get((object), (key)))
#define jsonGetObject(object, key) json_object_get((object), (key))
#define jsonGetArray(object, key) json_object_get((object), (key))

// check values
#define jsonIsString(object, key) json_is_string(json_object_get((object), (key)))
#define jsonIsInt(object, key) json_is_integer(json_object_get((object), (key)))
#define jsonIsBool(object, key) json_is_boolean(json_object_get((object), (key)))
#define jsonIsDouble(object, key) json_is_real(json_object_get((object), (key)))
#define jsonIsObject(object, key) json_is_object(json_object_get((object), (key)))
#define jsonIsArray(object, key) json_is_array((object), (key))

// array add values
#define jsonArrayNew() json_array()
#define jsonArrayAddString(array, value) json_array_append_new((array), json_string((value)))
#define jsonArrayAddInt(array, value) json_array_append_new((array), json_integer((value)))
#define jsonArrayAddBool(array, value) json_array_append_new((array), json_boolean((value)))
#define jsonArrayAddDouble(array, value) json_array_append_new((array), json_real((value)))
#define jsonArrayAddObject(array, value) json_array_append_new((array), (value))

// array get values
#define jsonArraySize(array) json_arraySize(array)
#define jsonArrayGetString(array, index) json_string_value(json_array_get((array), (index)))
#define jsonArrayGetInt(array, index) json_integer_value(json_array_get((array), (index)))
#define jsonArrayGetBool(array, index) json_is_true(json_array_get((array), (index)))
#define jsonArrayGetDouble(array, index) json_real_value(json_array_get((array), (index)))
#define jsonArrayGetObject(array, index) json_array_get((array), (index))

// these toString functions allocate memory, dont forget to free
#define jsonToStringAlloc(object) json_dumps((object), JSON_COMPACT)
#define jsonToStringPrettyAlloc(object) json_dumps((object), JSON_INDENT(4))
}  // namespace utils
