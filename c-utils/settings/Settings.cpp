#include "events/Events.h"
#include "file/File.h"
#include "json/Json.h"
#include "platform/Platform.h"
#include "memorymanager/MemoryManager.h"
#include "logger/Logger.h"
#include "string/String.h"
#include <assert.h>

typedef struct Template {
    const char* name;
    const char* type;
    double defaultValue;
} Template;

Array(Template) templates;

static void writeDefault(void);
static void readSettingsFile(void);
static char validateSetting(Json* settingsJson, Template* tpl);
static String* settingsPath;
static Json* json;

void settingsInit(void) {
    info("settings: initializing");

    arrayPut(templates, ((Template){"effects", "double", 80.}));
    arrayPut(templates, ((Template){"music", "double", 40.}));
    arrayPut(templates, ((Template){"fpsLimit", "double", 60.}));
    arrayPut(templates, ((Template){"fpsLimitChecked", "boolean", 1.}));
    arrayPut(templates, ((Template){"uiScale", "double", 0.}));
    arrayPut(templates, ((Template){"cursorScale", "double", 0.}));
    arrayPut(templates, ((Template){"showFps", "boolean", 1.}));
    arrayPut(templates, ((Template){"vsync", "boolean", 0.}));
    arrayPut(templates, ((Template){"aaMode", "double", 0.}));
    arrayPut(templates, ((Template){"aaCasStrength", "double", 50.}));

    arrayPut(templates, ((Template){"upscalerMode", "double", 2.}));
    arrayPut(templates, ((Template){"renderScale", "double", 1.}));
    arrayPut(templates, ((Template){"fullScreen", "boolean", 0.}));
    arrayPut(templates, ((Template){"busyLoopLinux", "boolean", 1.}));
    arrayPut(templates, ((Template){"moreShadows", "boolean", 1.}));
    arrayPut(templates, ((Template){"shadowsDisabled", "boolean", 0.}));
    arrayPut(templates, ((Template){"gtaoDisabled", "boolean", 0.}));
    arrayPut(templates, ((Template){"ssrDisabled", "boolean", 0.}));
    arrayPut(templates, ((Template){"bloomDisabled", "boolean", 0.}));
    arrayPut(templates, ((Template){"contactShadowDisabled", "boolean", 0.}));
    arrayPut(templates, ((Template){"fogMode", "double", 1.}));
    arrayPut(templates, ((Template){"taaEnabled", "boolean", 0.}));
    arrayPut(templates, ((Template){"taaWeight", "double", 0.9}));
    arrayPut(templates, ((Template){"taaGhost", "double", 1.0}));
    arrayPut(templates, ((Template){"taaDepth", "double", 0.06}));

    settingsPath = stringNew("%s%s%s%s", platform.cwd, "data", platform.seperator, "settings.json");
    debug("settings: path %s", settingsPath->data);
    if (!fileExists(settingsPath->data)) {  // file not found, write default
        writeDefault();
    }
    readSettingsFile();

    if (json == NULL) {  // parse failed, write default
        writeDefault();
        readSettingsFile();
    }

    for (i32 i = 0, si = arraySize(templates); i < si; i++) {
        Template* tpl = &templates[i];
        if (json_object_get(json, tpl->name) == NULL) {
            /* New key (tpl added after the user's file was last
             * written): seed the default in place instead of rewriting
             * the whole file, which would clobber the user's other
             * settings. */
            if (strcmp(tpl->type, "boolean") == 0) {
                jsonSetBool(json, tpl->name, (int)tpl->defaultValue);
            } else if (strcmp(tpl->type, "double") == 0) {
                jsonSetDouble(json, tpl->name, tpl->defaultValue);
            } else if (strcmp(tpl->type, "int") == 0) {
                jsonSetInt(json, tpl->name, (int)tpl->defaultValue);
            }
        } else if (!validateSetting(json, tpl)) {  // key type mismatch, write default
            writeDefault();
            readSettingsFile();
            break;
        }
    }
    arrayFree(templates);
}

void writeDefault(void) {
    Json* settingsJson = jsonNew();

    for (i32 i = 0, si = arraySize(templates); i < si; i++) {
        Template* tpl = &templates[i];
        if (strcmp(tpl->type, "boolean") == 0) {
            jsonSetBool(settingsJson, tpl->name, (int)tpl->defaultValue);
        }
        if (strcmp(tpl->type, "double") == 0) {
            jsonSetDouble(settingsJson, tpl->name, tpl->defaultValue);
        }
        if (strcmp(tpl->type, "number") == 0) {
            jsonSetInt(settingsJson, tpl->name, (int)tpl->defaultValue);
        }
    }

    char* settingsJsonString = jsonToStringPrettyAlloc(settingsJson);
    fileWrite(settingsPath->data, settingsJsonString);
    jsonFree(settingsJson);
    memoryFree(settingsJsonString);

    warn("settings: write default settings");
}

void settingsWrite(void) {
    char* settingsJsonString = jsonToStringPrettyAlloc(json);
    fileWrite(settingsPath->data, settingsJsonString);
    signalEmit("settingsSaved", NULL);
    memoryFree(settingsJsonString);
}

void settingsDestroy(void) {
    stringDestroy(settingsPath);
    jsonFree(json);
}

void readSettingsFile(void) {
    if (json) {
        jsonFree(json);
    }
    String settingsFileString = fileRead(settingsPath->data);
    json                      = jsonParse(settingsFileString.data);
    stringDestroy(&settingsFileString);
}

char validateSetting(Json* settingsJson, Template* tpl) {
    if (strcmp(tpl->type, "boolean") == 0) {
        if (!jsonIsBool(settingsJson, tpl->name)) {
            return 0;
        }
    }

    if (strcmp(tpl->type, "double") == 0) {
        if (!jsonIsDouble(settingsJson, tpl->name)) {
            return 0;
        }
    }

    if (strcmp(tpl->type, "int") == 0) {
        if (!jsonIsInt(json, tpl->name)) {
            return 0;
        }
    }

    return 1;
}

double settingsGetDouble(const char* key) {
    assert(key && "need key");
    assert(jsonIsDouble(json, key) && "not double");
    return jsonGetDouble(json, key);
}

int settingsGetInt(const char* key) {
    assert(key && "need key");
    assert(jsonIsInt(json, key) && "not int");
    return jsonGetInt(json, key);
}

char settingsGetBool(const char* key) {
    assert(key && "need key");
    assert(jsonIsBool(json, key) && "not bool");
    return jsonGetBool(json, key);
}

void settingsSetInt(const char* key, int value) {
    assert(key && "need key");
    assert(jsonIsInt(json, key) && "not int");
    jsonSetInt(json, key, value);
}

void settingsSetDouble(const char* key, double value) {
    assert(key && "need key");
    assert(jsonIsDouble(json, key) && "not double");
    jsonSetDouble(json, key, value);
}

void settingsSetBool(const char* key, char value) {
    assert(key && "need key");
    assert(jsonIsBool(json, key) && "not bool");
    jsonSetBool(json, key, value);
}
